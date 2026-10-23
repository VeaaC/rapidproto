//! The shared payload directory (`RAPIDPROTO_BENCH_DIR`) -- the bytes the C++ benches write in their
//! dump mode (`RAPIDPROTO_BENCH_DUMP_DIR`, see `tests/bench_payloads.hpp`), so every Rust number is
//! measured on byte-identical input to its C++ counterpart. This crate is decode-only and builds none
//! of them. Layout:
//!
//! ```text
//! <dir>/<name>.bin           an arena scenario, protoc-serialized   (rapidproto_arena_bench)
//! <dir>/stream/<stem>.bin    a streaming scenario                   (rapidproto_bench)
//! <dir>/rv/<stem>/<i>.bin    buffer i of a varint-sweep pool        (both)
//! ```
//!
//! The varint sweeps decode a POOL of distinct-seed buffers per scenario, rotated one decode per call,
//! so no arm replays one memorized width sequence (the C++ rationale, `tests/bench_varint.hpp`). Their
//! scenario names and axes are mirrored here from that header: the names must match the C++ tables.

use std::path::{Path, PathBuf};

/// One scenario's input: a single buffer, or a varint-sweep pool decoded one buffer per call.
pub struct Inputs {
    pub buffers: Vec<Vec<u8>>,
}

impl Inputs {
    /// The mean buffer size -- the bytes one decode call processes (a pool's calls rotate over all).
    pub fn avg_bytes(&self) -> f64 {
        let total: usize = self.buffers.iter().map(Vec::len).sum();
        total as f64 / self.buffers.len() as f64
    }
}

/// Where a scenario's input lives under the payload directory.
#[derive(Clone)]
pub enum Source {
    /// `<dir>/<stem>.bin` -- an arena scenario.
    Arena(&'static str),
    /// `<dir>/stream/<stem>.bin` -- a streaming scenario.
    Stream(&'static str),
    /// `<dir>/rv/<stem>/<i>.bin` -- a varint-sweep pool.
    Pool(String),
}

/// `RAPIDPROTO_BENCH_DIR`, or exit with how to produce it.
pub fn require_dir() -> PathBuf {
    match std::env::var_os("RAPIDPROTO_BENCH_DIR") {
        Some(d) => PathBuf::from(d),
        None => {
            eprintln!(
                "RAPIDPROTO_BENCH_DIR is required: the shared payload directory. Produce it with \
                 both C++ benches:\n  RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_arena_bench\n  \
                 RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_bench"
            );
            std::process::exit(2);
        }
    }
}

/// Read one payload file, failing loudly on a missing or empty one (a 0-byte file would decode as an
/// empty message and silently "measure" nothing).
pub fn read(path: &Path) -> Vec<u8> {
    let bytes = std::fs::read(path).unwrap_or_else(|e| panic!("cannot read payload {path:?}: {e}"));
    assert!(!bytes.is_empty(), "payload {path:?} is empty");
    bytes
}

/// Load a scenario's input from the payload directory `dir`.
pub fn load(dir: &Path, source: &Source) -> Inputs {
    let buffers = match source {
        Source::Arena(stem) => vec![read(&dir.join(format!("{stem}.bin")))],
        Source::Stream(stem) => vec![read(&dir.join("stream").join(format!("{stem}.bin")))],
        Source::Pool(stem) => {
            let pool_dir = dir.join("rv").join(stem);
            let pool: Vec<Vec<u8>> = (0..)
                .map(|i| pool_dir.join(format!("{i}.bin")))
                .take_while(|p| p.exists())
                .map(|p| read(&p))
                .collect();
            assert!(!pool.is_empty(), "no pool buffers under {pool_dir:?}");
            pool
        }
    };
    Inputs { buffers }
}

/// One element-width distribution of the varint sweeps: its scenario-name label, and whether its
/// values all fit an int32 (the enum sweep runs only those). Mirrors `rpbench::varint_dists()`.
pub struct VarintDist {
    pub label: &'static str,
    pub enum_narrow: bool,
}

/// The sweeps' distributions, in the C++ order: fixed width 1..10 bytes, uniform 1..10, a 90/10 skew,
/// uniform 1..2 and 1..3, and 1..3 with a 1% 10-byte outlier.
pub fn varint_dists() -> Vec<VarintDist> {
    const FIXED: [&str; 10] = [
        "fx1", "fx2", "fx3", "fx4", "fx5", "fx6", "fx7", "fx8", "fx9", "fx10",
    ];
    let mut out: Vec<VarintDist> = FIXED
        .iter()
        .enumerate()
        .map(|(i, &label)| VarintDist {
            label,
            enum_narrow: i < 3,
        })
        .collect();
    for (label, enum_narrow) in [
        ("unif", false),
        ("skew", false),
        ("mix12", true),
        ("mix13", true),
        ("mix13out", false),
    ] {
        out.push(VarintDist { label, enum_narrow });
    }
    out
}

/// The element-count axis of the `rv` sweep (`rpbench::varint_lengths()`).
pub const VARINT_LENGTHS: [u32; 6] = [10, 100, 1_000, 10_000, 100_000, 1_000_000];

/// The element count of the arena bench's per-type sweep (`rv-zz` / `rv-enum`).
pub const TYPE_SWEEP_COUNT: u32 = 1_000_000;

/// A count as its scenario-name tag: 10 -> "10", 1000 -> "1k", 1000000 -> "1M".
pub fn length_tag(count: u32) -> String {
    if count >= 1_000_000 && count.is_multiple_of(1_000_000) {
        format!("{}M", count / 1_000_000)
    } else if count >= 1_000 && count.is_multiple_of(1_000) {
        format!("{}k", count / 1_000)
    } else {
        count.to_string()
    }
}

/// `rv <dist> <count>` -- a packed int64 (`Big.numbers`) sweep scenario, named as in both C++ benches.
pub fn rv_scenario_name(dist: &VarintDist, count: u32) -> String {
    format!("rv {} {}", dist.label, length_tag(count))
}

/// `rv-<tag> <dist> <count>` -- the arena bench's per-type sweep over the same varints carried by the
/// sint64 (`zz`) or enum (`enum`) field.
pub fn rv_type_scenario_name(tag: &str, dist: &VarintDist, count: u32) -> String {
    format!("rv-{tag} {} {}", dist.label, length_tag(count))
}

/// A sweep scenario's pool: its name as a directory stem (spaces -> `-`).
pub fn pool(name: &str) -> Source {
    Source::Pool(name.replace(' ', "-"))
}
