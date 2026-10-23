//! The measurement loop and report shared by the Rust runners -- `bench` (the arena decoder against
//! the Rust ecosystem) and `stream` (the streaming decoder against a hand-written wire loop). Per
//! scenario: load its input from the shared payload directory (skipping, with a note, a scenario this
//! build has no decoder for or whose input the directory lacks), gate on every supported arm agreeing
//! on the checksum -- and for a real-world dataset, on the C++ arms' checksum the dump recorded -- time
//! the arms, print a row per arm (and an NDJSON record with `RAPIDPROTO_BENCH_JSON`) plus the
//! scenario's memory report if it makes one, then drop the input -- the varint sweeps' pools reach
//! ~80 MB each, so inputs are never all resident at once.
//!
//! Timing: `ROUNDS` samples per arm; within a round every supported arm runs back to back, their order
//! rotated per round to cancel slot bias (the C++ harness does the same), and the median sample is the
//! arm's cost. A sample is `inner` consecutive decode calls, so the timer is amortized; a pool's calls
//! rotate over its buffers, every arm stepping through them in lockstep. `inner` is calibrated per
//! scenario so the baseline's sample takes ~10 ms (payloads span 12 bytes to 46 MB); `ITERS` fixes it.
//! Throughput is the bytes one call decodes ([`Inputs::bytes`]) over the median time per call (decimal
//! GB/s, as the C++ benches print). Rust measures no in-process counters: read instructions/byte with
//! an external `perf stat -e instructions:u` over the `perf_*` examples.
//!
//! Run discipline, or the numbers lie: pinned (`taskset -c N`), aligned (`RUSTFLAGS=
//! "-Cllvm-args=-align-all-functions=6"`, so an unrelated edit relaying the binary does not move
//! throughput by code placement alone), `performance` governor, turbo/SMT off, one run at a time on a
//! quiesced box.
//!
//! Env: `RAPIDPROTO_BENCH_DIR` (required) the payload directory; `RAPIDPROTO_BENCH_CHECK_ONLY` run only
//! the agreement gate (no timing -- valid on any box); `RAPIDPROTO_BENCH_JSON` also emit one `rec:"arm"`
//! NDJSON line per measurement (`cyc_b`/`ins_b` are null) and one `rec:"mem"` line per memory report;
//! `RAPIDPROTO_BENCH_ONLY=<substring>` run only the scenarios whose name contains it (as in the C++
//! benches); `ARMS` a comma-separated arm allowlist (arm 0, the baseline, is always kept); `ITERS`
//! decode calls per sample (default: calibrated); `ROUNDS` samples per arm (default 20).

use std::hint::black_box;
use std::path::PathBuf;
use std::time::Instant;

use crate::payloads::{self, Inputs, Source};
use crate::{Arm, ArmsFn, Decode};

/// One measured scenario: its name, the name of the C++ bench row it mirrors (`None` when C++ has no
/// such row), where its input lives, the arms that decode it (arm 0 = the baseline), and the memory
/// report it makes, if any.
pub struct Scenario {
    pub name: String,
    pub cpp_name: Option<String>,
    pub source: Source,
    pub arms: ArmsFn,
    pub mem: Option<MemFn>,
}

/// One cold decode of the input into a fresh arena, returning the arena's `(bytes_used,
/// bytes_reserved)` -- the C++ arena bench's `mem` record (`arena_used`, `arena_held`).
pub type MemFn = fn(&[u8]) -> (usize, usize);

/// What a runner reports itself as, and its scenario-specific flags.
pub struct Report {
    /// The NDJSON `decoder` field (`rust-ecosystem`, `rust-stream`).
    pub decoder: &'static str,
    /// Whether `(scenario, arm)` relied on undefined behaviour (flagged `‡` and in the NDJSON).
    pub ub: fn(&str, &str) -> bool,
    /// Printed once before the table (a legend for the flags above, say).
    pub legend: &'static str,
}

struct Options {
    dir: PathBuf,
    check_only: bool,
    json: bool,
    only: Option<String>,
    arm_filter: Option<Vec<String>>,
    inner: Option<usize>,
    rounds: usize,
}

fn env_usize(key: &str) -> Option<usize> {
    std::env::var(key).ok().and_then(|v| v.parse().ok())
}

impl Options {
    fn from_env() -> Self {
        Options {
            dir: payloads::require_dir(),
            check_only: std::env::var_os("RAPIDPROTO_BENCH_CHECK_ONLY").is_some(),
            json: std::env::var_os("RAPIDPROTO_BENCH_JSON").is_some(),
            only: std::env::var("RAPIDPROTO_BENCH_ONLY")
                .ok()
                .filter(|s| !s.is_empty()),
            arm_filter: std::env::var("ARMS").ok().map(|v| {
                v.split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect()
            }),
            inner: env_usize("ITERS"),
            rounds: env_usize("ROUNDS").unwrap_or(20),
        }
    }
}

// Keep the arms named in `filter` (plus arm 0, the baseline, always -- ratios need a reference); with
// no filter, keep every arm. Lets a run pick arms at runtime without recompiling.
fn select_arms(arms: Vec<Arm>, filter: Option<&[String]>) -> Vec<Arm> {
    match filter {
        None => arms,
        Some(keep) => arms
            .into_iter()
            .enumerate()
            .filter(|(i, a)| *i == 0 || keep.iter().any(|k| k == a.name))
            .map(|(_, a)| a)
            .collect(),
    }
}

fn median(xs: &[f64]) -> f64 {
    let mut v = xs.to_vec();
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let n = v.len();
    if n % 2 == 1 {
        v[n / 2]
    } else {
        (v[n / 2 - 1] + v[n / 2]) / 2.0
    }
}

/// The checksum of one pass over every input buffer -- what the agreement gate compares.
fn pool_checksum(f: &Decode, inputs: &Inputs) -> u64 {
    inputs
        .buffers
        .iter()
        .fold(0u64, |s, b| s.wrapping_add(f(b)))
}

// The baseline sample's target duration when `ITERS` is unset.
const TARGET_SAMPLE_NS: f64 = 10e6;

/// Decode calls per sample: `ITERS`, else enough for the baseline's sample to take ~10 ms.
fn calibrate(base: &Decode, inputs: &Inputs, fixed: Option<usize>) -> usize {
    if let Some(n) = fixed {
        return n.max(1);
    }
    let n = inputs.buffers.len();
    let t0 = Instant::now();
    let mut acc = 0u64;
    for b in &inputs.buffers {
        acc = acc.wrapping_add(base(black_box(b)));
    }
    black_box(acc);
    let per_call = t0.elapsed().as_nanos() as f64 / n as f64;
    ((TARGET_SAMPLE_NS / per_call.max(1.0)).ceil() as usize).clamp(1, 10_000_000)
}

/// Median ns per decode call for each arm (`None` for an arm unsupported for this scenario).
fn measure(arms: &[Arm], inputs: &Inputs, rounds: usize, inner: usize) -> Vec<Option<f64>> {
    let sup: Vec<usize> = (0..arms.len())
        .filter(|&i| arms[i].decoder().is_some())
        .collect();
    let m = sup.len();
    let bufs = &inputs.buffers;
    // Each arm's own rotation index; every arm makes the same number of calls, so all stay in lockstep.
    let mut at = vec![0usize; arms.len()];
    for &i in &sup {
        let f = arms[i].decoder().unwrap();
        for _ in 0..3 {
            black_box(f(black_box(&bufs[at[i] % bufs.len()])));
            at[i] += 1;
        }
    }
    let mut samples: Vec<Vec<f64>> = vec![Vec::with_capacity(rounds); arms.len()];
    for r in 0..rounds {
        for j in 0..m {
            let idx = sup[(j + r) % m];
            let f = arms[idx].decoder().unwrap();
            let t0 = Instant::now();
            let mut acc = 0u64;
            for _ in 0..inner {
                acc = acc.wrapping_add(f(black_box(&bufs[at[idx] % bufs.len()])));
                at[idx] += 1;
            }
            let ns = t0.elapsed().as_nanos() as f64 / inner as f64;
            black_box(acc);
            samples[idx].push(ns);
        }
    }
    (0..arms.len())
        .map(|i| (!samples[i].is_empty()).then(|| median(&samples[i])))
        .collect()
}

/// The scenario's memory report, if it makes one (deterministic, so check-only runs make it too).
/// `cpp` is the C++ row name as a JSON value -- the C++ record's `shape`.
fn report_mem(sc: &Scenario, inputs: &Inputs, cpp: &str, json: bool, decoder: &str) {
    let Some(mem) = sc.mem else {
        return;
    };
    let (used, held) = mem(&inputs.buffers[0]);
    println!(
        "{:<26} arena memory, one decode: used {used} B, held {held} B (held/used {:.3})",
        sc.name,
        held as f64 / used as f64
    );
    if json {
        println!(
            "{{\"rec\":\"mem\",\"decoder\":\"{decoder}\",\"scenario\":\"{}\",\"shape\":{cpp},\
             \"arena_used\":{used},\"arena_held\":{held}}}",
            sc.name
        );
    }
}

/// The loop the `perf_*` examples run under `perf stat -e instructions:u`: decode the scenario named
/// `SCENARIO` with arm `ARM` (default `default_arm`) `ITERS` times, rotating over a pool, then report
/// the bytes decoded -- instructions over those bytes is the instructions/byte to set beside the C++
/// bench's `ins/B`. `ITERS` defaults to ~2 GB of input, so loading the payload stays a small share of
/// the count.
pub fn perf(scenarios: Vec<Scenario>, default_arm: &str) {
    let name = std::env::var("SCENARIO").expect("set SCENARIO to a scenario name (e.g. records)");
    let arm_name = std::env::var("ARM").unwrap_or_else(|_| default_arm.to_string());
    let dir = payloads::require_dir();
    let sc = scenarios
        .into_iter()
        .find(|s| s.name == name)
        .unwrap_or_else(|| panic!("unknown SCENARIO {name:?}"));
    let inputs = payloads::load(&dir, &sc.source);
    let arms = (sc.arms)();
    let arm = arms
        .iter()
        .find(|a| a.name == arm_name)
        .unwrap_or_else(|| panic!("scenario {name:?} has no arm {arm_name:?}"));
    let f = arm
        .decoder()
        .unwrap_or_else(|| panic!("arm {arm_name:?} is unsupported for {name:?}"));
    let bytes = inputs.bytes;
    let iters = env_usize("ITERS").unwrap_or(((2e9 / bytes).ceil() as usize).max(1));
    let bufs = &inputs.buffers;
    let mut checksum = 0u64;
    for i in 0..iters {
        checksum = checksum.wrapping_add(black_box(f(black_box(&bufs[i % bufs.len()]))));
    }
    eprintln!(
        "scenario={name} arm={arm_name} iters={iters} bytes/iter={bytes:.0} total={:.0} \
         checksum={checksum}",
        bytes * iters as f64
    );
}

/// Run every selected scenario (load, agree, time, report) and return the process exit code.
pub fn run(scenarios: Vec<Scenario>, report: &Report) -> i32 {
    let opts = Options::from_env();
    if !opts.check_only {
        eprintln!(
            "\nNOTE: throughput is meaningful only pinned (taskset -c N), aligned \
             (-Cllvm-args=-align-all-functions=6), governor=performance, turbo/SMT off, one run at a \
             time. ins/B via an external `perf stat -e instructions:u`.\n"
        );
        if !report.legend.is_empty() {
            println!("\n{}\n", report.legend);
        }
        println!(
            "\n{:<26} {:<18} {:>9}  {:>9}",
            "scenario", "arm", "GB/s", "vs base"
        );
    }
    for sc in scenarios {
        if opts
            .only
            .as_ref()
            .is_some_and(|o| !sc.name.contains(o.as_str()))
        {
            continue;
        }
        let name = sc.name.as_str();
        let arms = (sc.arms)();
        // A scenario this build cannot decode at all (its decoders were not generated) is listed with
        // the reason, not dropped.
        if let [first, ..] = arms.as_slice() {
            if arms.iter().all(|a| a.decoder().is_none()) {
                let reason = first.unsupported_reason().unwrap_or("");
                eprintln!("note: scenario {name} SKIPPED -- {reason}");
                continue;
            }
        }
        if !payloads::available(&opts.dir, &sc.source) {
            eprintln!(
                "note: scenario {name} SKIPPED -- its input is not in the payload directory (dump it \
                 from a C++ arena bench built with the dataset fetched; see payloads.rs)"
            );
            continue;
        }
        let inputs = payloads::load(&opts.dir, &sc.source);
        let mut arms = select_arms(arms, opts.arm_filter.as_deref());
        // An arm that relies on UB for this scenario (quick-protobuf's unaligned packed-double slice)
        // only behaves in a release build; in a debug build the alignment UB-check aborts the process.
        // Drop it there so debug runs still work -- it is measured in release only, and flagged there.
        if cfg!(debug_assertions) {
            arms.retain(|a| !(report.ub)(name, a.name));
        }
        // Supported arms must agree on the checksum; unsupported arms carry no decoder and are listed
        // but excluded from the gate.
        let sums: Vec<Option<u64>> = arms
            .iter()
            .map(|a| a.decoder().map(|d| pool_checksum(d, &inputs)))
            .collect();
        let agreed: Vec<u64> = sums.iter().filter_map(|&s| s).collect();
        // Every arm agreeing on 0 means the input decodes to nothing: an empty or mislabelled payload
        // (a sweep pool of the wrong field), which would otherwise be timed as a skip.
        if agreed[0] == 0 {
            eprintln!("scenario {name}: every arm's checksum is 0 -- the input decodes to nothing");
            return 1;
        }
        if !agreed.iter().all(|&s| s == agreed[0]) {
            eprintln!("CHECKSUM MISMATCH in scenario {name}:");
            for (a, s) in arms.iter().zip(&sums) {
                match s {
                    Some(s) => eprintln!("  {:<18} {s:#018x}", a.name),
                    None => eprintln!(
                        "  {:<18} unsupported: {}",
                        a.name,
                        a.unsupported_reason().unwrap_or("")
                    ),
                }
            }
            return 1;
        }
        let cpp_sum = payloads::cpp_checksum(&opts.dir, &sc.source);
        if cpp_sum.is_some_and(|c| c != agreed[0]) {
            eprintln!(
                "CHECKSUM MISMATCH in scenario {name}: the arms agree on {:#018x}, the C++ arms on \
                 {:#018x}",
                agreed[0],
                cpp_sum.unwrap()
            );
            return 1;
        }
        let bytes = inputs.bytes;
        let cpp = sc
            .cpp_name
            .as_ref()
            .map_or_else(|| "null".to_string(), |n| format!("\"{n}\""));
        if opts.check_only {
            let n_unsupported = sums.iter().filter(|s| s.is_none()).count();
            println!(
                "checksum ok  {name:<26} {:#018x}{}  ({} arms{}, {} buffer(s), {:.0} bytes/call)",
                agreed[0],
                if cpp_sum.is_some() { " = C++" } else { "" },
                agreed.len(),
                if n_unsupported > 0 {
                    format!(", {n_unsupported} unsupported")
                } else {
                    String::new()
                },
                inputs.buffers.len(),
                bytes
            );
            report_mem(&sc, &inputs, &cpp, opts.json, report.decoder);
            continue;
        }
        let base = arms[0].decoder().expect("baseline arm must be supported");
        let inner = calibrate(base, &inputs, opts.inner);
        let meds = measure(&arms, &inputs, opts.rounds, inner);
        let base_gbs = bytes / meds[0].expect("baseline arm must be supported");
        for (i, a) in arms.iter().enumerate() {
            let ub = (report.ub)(name, a.name);
            match meds[i] {
                Some(med) => {
                    let gbs = bytes / med;
                    let baseline = i == 0;
                    let vs = if baseline {
                        0.0
                    } else {
                        (gbs / base_gbs - 1.0) * 100.0
                    };
                    let armcol = format!("{}{}", a.name, if ub { " ‡" } else { "" });
                    println!("{name:<26} {armcol:<18} {gbs:>9.3}  {vs:>8.1}%");
                    if opts.json {
                        println!(
                            "{{\"rec\":\"arm\",\"decoder\":\"{}\",\"scenario\":\"{name}\",\
                             \"cpp_scenario\":{cpp},\"arm\":\"{}\",\"bytes\":{bytes:.0},\
                             \"baseline\":{baseline},\"gb_s\":{gbs:.6},\"cyc_b\":null,\"ins_b\":null,\
                             \"vs_base_pct\":{vs:.3},\"ub_unaligned_fixed_slice\":{ub},\
                             \"supported\":true,\"ok\":true}}",
                            report.decoder, a.name
                        );
                    }
                }
                None => {
                    // Unsupported for this scenario (a feature gap): listed + flagged, not timed.
                    let reason = a.unsupported_reason().unwrap_or("unsupported");
                    println!("{name:<26} {:<18} {:>9}  ({reason})", a.name, "unsupported");
                    if opts.json {
                        println!(
                            "{{\"rec\":\"arm\",\"decoder\":\"{}\",\"scenario\":\"{name}\",\
                             \"cpp_scenario\":{cpp},\"arm\":\"{}\",\"bytes\":{bytes:.0},\
                             \"baseline\":false,\"gb_s\":null,\
                             \"cyc_b\":null,\"ins_b\":null,\"vs_base_pct\":null,\
                             \"supported\":false,\"unsupported_reason\":\"{reason}\",\"ok\":false}}",
                            report.decoder, a.name
                        );
                    }
                }
            }
        }
        report_mem(&sc, &inputs, &cpp, opts.json, report.decoder);
    }
    if opts.check_only {
        println!("\nall arms agree on every scenario (checksum-only run; no timing).");
    }
    0
}
