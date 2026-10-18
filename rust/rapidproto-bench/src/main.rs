//! Ecosystem comparison runner. Decodes the shared `bench.proto` payloads (the same
//! `<RAPIDPROTO_BENCH_DIR>/<name>.bin` files the C++ arena bench and the Rust A/B bench use) with
//! RapidProto's arena decoder (baseline) and each competitor, enforces that every arm agrees on the
//! per-field checksum, then times them and reports throughput relative to the baseline.
//!
//! Cold path only: a fresh `Arena` per decode for our arm (and a fresh owned allocation for prost) --
//! the realistic decode-into-a-new-arena shape, matching the C++ arena-cold arm and `decode.rs`. A
//! warm (reset-and-reuse) arm can be added later.
//!
//! BUILD PROFILE: plain `--release` (no LTO), matching the RapidProto-vs-C++ bench methodology and
//! what most users actually ship. Under the default profile a decoder's hot path stays inlined across
//! crate / codegen-unit boundaries only if it is ANNOTATED for it: RapidProto's wire readers are
//! `#[inline(always)]`, so they inline here; prost's decode is not force-inlined the same way, so it
//! gains from LTO + `codegen-units=1`. That is a real property of how each decoder is written -- a
//! competitor optimizing poorly under the default profile is the competitor's trade-off, not a
//! benchmark handicap -- so `--release` is a fair comparison. For transparency, LTO + `codegen-units=1`
//! narrows prost's gap by up to ~1.5x on the compute-bound scenarios (the allocation/validation-bound
//! shapes like `dataset` barely move -- that part is a model difference, not an optimization gap); add
//! `--config 'profile.release.lto="fat"' --config 'profile.release.codegen-units=1'` to see it.
//!
//! ```text
//! # produce the shared payloads once (protoc-canonical, from the C++ bench):
//! RAPIDPROTO_BENCH_DUMP_DIR=/tmp/bench ./build/<cfg>/rapidproto_arena_bench
//! # run pinned + aligned (see the discipline note below), one run at a time, on a quiesced box:
//! RAPIDPROTO_BENCH_DIR=/tmp/bench RUSTFLAGS="-Cllvm-args=-align-all-functions=6" \
//!     taskset -c 0 cargo run -p bench-ecosystem --release
//! ```
//!
//! Env: `RAPIDPROTO_BENCH_DIR` (required) the payload dir; `RAPIDPROTO_BENCH_CHECK_ONLY` run only the
//! cross-arm checksum agreement (no timing -- valid on any box); `RAPIDPROTO_BENCH_JSON` also emit one
//! `rec:"arm"` ndjson line per measurement (the format the C++ bench + bench.py use; `cyc_b`/`ins_b`
//! are null here -- Rust has no in-process counters, read instructions/byte with an external
//! `perf stat -e instructions:u`); `ARMS` a comma-separated arm allowlist, e.g. `rapidproto,prost`
//! (arm 0 is always kept; unset runs every arm); `ITERS` inner decodes per sample (default 50);
//! `ROUNDS` samples per arm (default 20).

use std::hint::black_box;
use std::path::Path;
use std::time::Instant;

use rapidproto_bench::{has_unaligned_fixed_slice_ub, scenario_specs, Arm};

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

// Median ns/op per arm. Arms run back-to-back within each round, their order rotated per round to
// cancel slot bias (the C++ harness does the same); `inner` decodes amortize the timer per sample.
fn measure(arms: &[Arm], bytes: &[u8], rounds: usize, inner: usize) -> Vec<f64> {
    let n = arms.len();
    for a in arms {
        for _ in 0..3 {
            black_box((a.run)(black_box(bytes)));
        }
    }
    let mut samples: Vec<Vec<f64>> = vec![Vec::with_capacity(rounds); n];
    for r in 0..rounds {
        for i in 0..n {
            let idx = (i + r) % n;
            let t0 = Instant::now();
            let mut acc = 0u64;
            for _ in 0..inner {
                acc = acc.wrapping_add((arms[idx].run)(black_box(bytes)));
            }
            let ns = t0.elapsed().as_nanos() as f64 / inner as f64;
            black_box(acc);
            samples[idx].push(ns);
        }
    }
    samples.iter().map(|s| median(s)).collect()
}

fn env_usize(key: &str, default: usize) -> usize {
    std::env::var(key)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

// Keep the arms named in `filter` (plus arm 0, the baseline, always -- ratios need a reference); with
// no filter, keep every arm. Lets a run pick competitors at runtime without recompiling.
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

fn main() {
    let dir = match std::env::var_os("RAPIDPROTO_BENCH_DIR") {
        Some(d) => d,
        None => {
            eprintln!(
                "RAPIDPROTO_BENCH_DIR is required: a directory of shared <name>.bin payloads.\n\
                 Produce it with the C++ bench: RAPIDPROTO_BENCH_DUMP_DIR=<dir> \
                 ./build/<cfg>/rapidproto_arena_bench"
            );
            std::process::exit(2);
        }
    };
    let dir = Path::new(&dir);
    let check_only = std::env::var_os("RAPIDPROTO_BENCH_CHECK_ONLY").is_some();
    let json = std::env::var_os("RAPIDPROTO_BENCH_JSON").is_some();
    let inner = env_usize("ITERS", 50);
    let rounds = env_usize("ROUNDS", 20);
    // Optional runtime arm selection: `ARMS=rapidproto,prost` keeps just those (arm 0 is always kept);
    // unset runs every arm.
    let arm_filter: Option<Vec<String>> = std::env::var("ARMS").ok().map(|v| {
        v.split(',')
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect()
    });

    // Load every payload and gate on cross-arm checksum agreement before timing anything.
    let mut loaded: Vec<(&'static str, Vec<u8>, Vec<Arm>)> = Vec::new();
    for (name, build) in scenario_specs() {
        let path = dir.join(format!("{name}.bin"));
        let bytes =
            std::fs::read(&path).unwrap_or_else(|e| panic!("cannot read payload {path:?}: {e}"));
        assert!(!bytes.is_empty(), "payload {path:?} is empty");
        let mut arms = select_arms(build(), arm_filter.as_deref());
        // An arm whose decode of this scenario reads a packed fixed-width float as an unaligned slice
        // (quick-protobuf on the Big scenarios) only behaves in a release build; in a debug build the
        // alignment UB-check aborts the process. Drop it there so `cargo run`/`cargo test` still work --
        // it is measured in release only, and flagged there (see below).
        if cfg!(debug_assertions) {
            arms.retain(|a| !has_unaligned_fixed_slice_ub(name, a.name));
        }
        let sums: Vec<u64> = arms.iter().map(|a| (a.run)(&bytes)).collect();
        if !sums.iter().all(|&s| s == sums[0]) {
            eprintln!("CHECKSUM MISMATCH in scenario {name}:");
            for (a, s) in arms.iter().zip(&sums) {
                eprintln!("  {:<12} {s:#018x}", a.name);
            }
            std::process::exit(1);
        }
        println!(
            "checksum ok  {name:<11} {:#018x}  ({} arms, {} bytes)",
            sums[0],
            arms.len(),
            bytes.len()
        );
        loaded.push((name, bytes, arms));
    }

    if check_only {
        println!("\nall arms agree on every scenario (checksum-only run; no timing).");
        return;
    }

    eprintln!(
        "\nNOTE: throughput is meaningful only pinned (taskset -c N), aligned \
         (-Cllvm-args=-align-all-functions=6), governor=performance, turbo/SMT off, one run at a \
         time. ins/B via an external `perf stat -e instructions:u`. --release is the default profile \
         most users ship; a decoder not annotated for inlining (unlike our #[inline(always)] wire \
         readers) gains from LTO+cgu=1 -- see the module header to reproduce a competitor's best \
         build.\n"
    );
    println!(
        "\n‡ = this measurement relied on UNDEFINED BEHAVIOUR: the arm read a packed fixed-width float \
         field (`Big.reals`, `repeated double`) as an unaligned borrowed slice. Correct on x86 here, \
         but a misaligned reference -- flag it in any table built from these numbers. (`ub_unaligned_\
         fixed_slice` in the ndjson.)\n"
    );
    println!(
        "\n{:<11} {:<16} {:>9}  {:>9}",
        "scenario", "arm", "GB/s", "vs base"
    );
    for (name, bytes, arms) in &loaded {
        let meds = measure(arms, bytes, rounds, inner);
        let base_gbs = bytes.len() as f64 / meds[0];
        for (i, a) in arms.iter().enumerate() {
            let gbs = bytes.len() as f64 / meds[i];
            let vs = (gbs / base_gbs - 1.0) * 100.0;
            let baseline = i == 0;
            let ub = has_unaligned_fixed_slice_ub(name, a.name);
            let armcol = format!("{}{}", a.name, if ub { " ‡" } else { "" });
            println!(
                "{name:<11} {armcol:<16} {gbs:>9.3}  {:>8.1}%",
                if baseline { 0.0 } else { vs }
            );
            if json {
                println!(
                    "{{\"rec\":\"arm\",\"decoder\":\"rust-ecosystem\",\"scenario\":\"{name}\",\
                     \"arm\":\"{}\",\"bytes\":{},\"baseline\":{baseline},\"gb_s\":{gbs:.6},\
                     \"cyc_b\":null,\"ins_b\":null,\"vs_base_pct\":{:.3},\
                     \"ub_unaligned_fixed_slice\":{ub},\"ok\":true}}",
                    a.name,
                    bytes.len(),
                    if baseline { 0.0 } else { vs }
                );
            }
        }
    }
}
