//! The measurement loop shared by the `bench` and `stream` runners. For each selected scenario:
//!
//! - skip it, with a note, when this build has no decoder for it or its real-world input is missing;
//! - load the input and require every supported arm to return the same non-zero checksum (for a
//!   real-world dataset, also the C++ checksum stored beside it);
//! - time the arms; print one row per arm (and NDJSON with `RAPIDPROTO_BENCH_JSON`) and the memory
//!   report if there is one;
//! - drop the input before the next scenario (a sweep pool is up to ~80 MB).
//!
//! Timing: `ROUNDS` samples per arm. Within a round every supported arm runs once, in an order rotated
//! per round to cancel slot bias, and an arm's cost is its median sample. A sample is whole passes over
//! the scenario's buffers ([`Decode::pass`]), as many as make the baseline's sample take ~10 ms: a
//! median is only meaningful when every sample is the same workload. Throughput is [`Inputs::bytes`]
//! over the median time per call, in decimal GB/s as the C++ benches print. There are no in-process
//! counters: read instructions/byte with `perf stat -e instructions:u` over the `perf_*` examples.
//!
//! Comparing with C++: the C++ harness times one call per sample (two clock reads and a
//! `std::function` call, ~20 ns), a pool's call being one pass over all its buffers, and adds rounds
//! until its ratios are significant; this runner times batches of at least ~10 ms over a fixed number
//! of rounds. On a small single buffer the C++ per-call overhead still weighs on its GB/s and ins/B.
//! The NDJSON `cpp_scenario` and `cpp_arm` name the C++ row a record corresponds to (null where C++
//! has none); `vs_base_pct` compares arms within this runner only.
//!
//! Run discipline, or the numbers lie: pinned (`taskset -c N`), aligned (`RUSTFLAGS=
//! "-Cllvm-args=-align-all-functions=6"`, so an unrelated edit relaying the binary does not move
//! throughput by code placement alone), `performance` governor, turbo/SMT off, one run at a time on a
//! quiesced box.
//!
//! Env: `RAPIDPROTO_BENCH_DIR` (required) the payload directory; `RAPIDPROTO_BENCH_CHECK_ONLY` run only
//! the agreement gate (no timing -- valid on any box); `RAPIDPROTO_BENCH_JSON` also emit one `rec:"arm"`
//! NDJSON line per measurement (`cyc_b`/`ins_b` are null) and one `rec:"mem"` line per memory report;
//! `RAPIDPROTO_BENCH_ONLY=<substring>` run only the scenarios whose Rust or C++ name contains it;
//! `ARMS` a comma-separated arm allowlist (arm 0, the baseline, is always kept); `ITERS` decode calls
//! per sample, rounded up to whole pool passes (default: calibrated); `ROUNDS` samples per arm
//! (default 20).

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
    /// The C++ bench's name for an arm (the NDJSON `cpp_arm`), `None` where C++ has no such arm.
    pub cpp_arm: fn(&str) -> Option<&'static str>,
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

// The baseline sample's target duration when `ITERS` is unset.
const TARGET_SAMPLE_NS: f64 = 10e6;

/// Passes over the buffers per sample: enough for `ITERS` decode calls, else for the baseline's sample
/// to take ~10 ms. Whole passes, so every sample of a pool is the same workload.
fn calibrate(base: &mut dyn Decode, inputs: &Inputs, fixed: Option<usize>) -> usize {
    let n = inputs.buffers.len();
    let calls = fixed.unwrap_or_else(|| {
        let t0 = Instant::now();
        black_box(base.pass(black_box(&inputs.buffers)));
        let per_call = t0.elapsed().as_nanos() as f64 / n as f64;
        ((TARGET_SAMPLE_NS / per_call.max(1.0)).ceil() as usize).clamp(1, 10_000_000)
    });
    calls.max(1).div_ceil(n)
}

/// Median ns per decode call for each arm (`None` for an arm unsupported for this scenario).
fn measure(arms: &mut [Arm], inputs: &Inputs, rounds: usize, passes: usize) -> Vec<Option<f64>> {
    let sup: Vec<usize> = (0..arms.len())
        .filter(|&i| arms[i].is_supported())
        .collect();
    let m = sup.len();
    let bufs = &inputs.buffers;
    let calls = passes * bufs.len();
    for &i in &sup {
        let f = arms[i].decoder().unwrap();
        for _ in 0..3 {
            black_box(f.pass(black_box(bufs)));
        }
    }
    let mut samples: Vec<Vec<f64>> = vec![Vec::with_capacity(rounds); arms.len()];
    for r in 0..rounds {
        for j in 0..m {
            let idx = sup[(j + r) % m];
            let f = arms[idx].decoder().unwrap();
            let t0 = Instant::now();
            let mut acc = 0u64;
            for _ in 0..passes {
                acc = acc.wrapping_add(f.pass(black_box(bufs)));
            }
            let ns = t0.elapsed().as_nanos() as f64 / calls as f64;
            black_box(acc);
            samples[idx].push(ns);
        }
    }
    (0..arms.len())
        .map(|i| (!samples[i].is_empty()).then(|| median(&samples[i])))
        .collect()
}

// An optional name as a JSON value (the names need no escaping).
fn json_str(s: Option<&str>) -> String {
    s.map_or_else(|| "null".to_string(), |s| format!("\"{s}\""))
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
/// `SCENARIO` with arm `ARM` (default `default_arm`) `ITERS` times, rounded up to whole passes over a
/// pool, then report the bytes decoded -- instructions over those bytes is the instructions/byte to set
/// beside the C++ bench's `ins/B`. `ITERS` defaults to ~2 GB of input, so loading the payload stays a
/// small share of the count.
pub fn perf(scenarios: Vec<Scenario>, default_arm: &str) {
    let name = std::env::var("SCENARIO").expect("set SCENARIO to a scenario name (e.g. records)");
    let arm_name = std::env::var("ARM").unwrap_or_else(|_| default_arm.to_string());
    let dir = payloads::require_dir();
    let sc = scenarios
        .into_iter()
        .find(|s| s.name == name)
        .unwrap_or_else(|| panic!("unknown SCENARIO {name:?}"));
    let inputs = payloads::load(&dir, &sc.source);
    let mut arms = (sc.arms)();
    let arm = arms
        .iter_mut()
        .find(|a| a.name == arm_name)
        .unwrap_or_else(|| panic!("scenario {name:?} has no arm {arm_name:?}"));
    let f = arm
        .decoder()
        .unwrap_or_else(|| panic!("arm {arm_name:?} is unsupported for {name:?}"));
    let bytes = inputs.bytes;
    let bufs = &inputs.buffers;
    let passes = env_usize("ITERS")
        .unwrap_or(((2e9 / bytes).ceil() as usize).max(1))
        .div_ceil(bufs.len());
    let iters = passes * bufs.len();
    let mut checksum = 0u64;
    for _ in 0..passes {
        checksum = checksum.wrapping_add(f.pass(black_box(bufs)));
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
    let (mut ran, mut skipped) = (0usize, 0usize);
    for sc in scenarios {
        if let Some(only) = opts.only.as_deref() {
            let cpp_match = sc.cpp_name.as_deref().is_some_and(|c| c.contains(only));
            if !sc.name.contains(only) && !cpp_match {
                continue;
            }
        }
        let name = sc.name.as_str();
        let arms = (sc.arms)();
        // A scenario this build cannot decode at all (its decoders were not generated) is listed with
        // the reason, not dropped.
        if let [first, ..] = arms.as_slice() {
            if !arms.iter().any(Arm::is_supported) {
                let reason = first.unsupported_reason().unwrap_or("");
                eprintln!("note: scenario {name} SKIPPED -- {reason}");
                skipped += 1;
                continue;
            }
        }
        if sc.source.is_external() && !payloads::present(&opts.dir, &sc.source) {
            eprintln!(
                "note: scenario {name} SKIPPED -- its input is not in the payload directory (dump it \
                 from a C++ arena bench built with the dataset fetched; see payloads.rs)"
            );
            skipped += 1;
            continue;
        }
        ran += 1;
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
            .iter_mut()
            .map(|a| a.decoder().map(|d| d.pass(&inputs.buffers)))
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
        let cpp = json_str(sc.cpp_name.as_deref());
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
        let passes = calibrate(base, &inputs, opts.inner);
        let meds = measure(&mut arms, &inputs, opts.rounds, passes);
        let base_gbs = bytes / meds[0].expect("baseline arm must be supported");
        for (i, a) in arms.iter().enumerate() {
            let ub = (report.ub)(name, a.name);
            let cpp_arm = json_str((report.cpp_arm)(a.name));
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
                             \"cpp_scenario\":{cpp},\"arm\":\"{}\",\"cpp_arm\":{cpp_arm},\
                             \"bytes\":{bytes:.0},\
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
                             \"cpp_scenario\":{cpp},\"arm\":\"{}\",\"cpp_arm\":{cpp_arm},\
                             \"bytes\":{bytes:.0},\"baseline\":false,\"gb_s\":null,\
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
    if ran == 0 {
        eprintln!("no scenario ran ({skipped} skipped)");
        return 1;
    }
    if opts.check_only {
        println!(
            "\nall arms agree on {ran} scenario(s), {skipped} skipped (checksum-only run; no timing)."
        );
    }
    0
}
