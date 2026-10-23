//! The shared payload directory (`RAPIDPROTO_BENCH_DIR`) -- the bytes the C++ benches write in their
//! dump mode (`RAPIDPROTO_BENCH_DUMP_DIR`, see `tests/bench_payloads.hpp`), so every Rust number is
//! measured on byte-identical input to its C++ counterpart. The runners build none of them. Layout:
//!
//! ```text
//! <dir>/<name>.bin           an arena scenario, protoc-serialized   (rapidproto_arena_bench)
//! <dir>/stream/<stem>.bin    a streaming scenario                   (rapidproto_bench)
//! <dir>/rv/<stem>/<i>.bin    buffer i of a varint-sweep pool        (both)
//! <dir>/rv/<stem>/pool.txt   the pool's buffer count, written last  (both)
//! <dir>/external/<stem>.bin  a real-world dataset's input           (rapidproto_arena_bench, corpus)
//! <dir>/external/<stem>/     a real-world dataset's pool            (rapidproto_arena_bench, corpus)
//! ```
//!
//! The varint sweeps decode a POOL of distinct-seed buffers per scenario, rotated one decode per call,
//! so no arm replays one memorized width sequence (the C++ rationale, `tests/bench_varint.hpp`). Their
//! scenario names and axes are mirrored here from that header: the names must match the C++ tables.
//!
//! The real-world datasets' inputs exist only when the C++ bench was built with the fetched corpus, so
//! their scenarios are skipped without them ([`available`]); every other payload is required.

use std::path::{Path, PathBuf};

/// One scenario's input: a single buffer (a batch is one, too), or a varint-sweep pool decoded one
/// buffer per call.
pub struct Inputs {
    pub buffers: Vec<Vec<u8>>,
    /// The bytes one decode call processes: the mean buffer size (a pool's calls rotate over all), or
    /// a batch's buffers without their framing.
    pub bytes: f64,
}

/// Where a scenario's input lives under the payload directory.
pub enum Source {
    /// `<dir>/<stem>.bin` -- an arena scenario.
    Arena(&'static str),
    /// `<dir>/stream/<stem>.bin` -- a streaming scenario.
    Stream(&'static str),
    /// `<dir>/rv/<stem>/<i>.bin` -- a varint-sweep pool.
    Pool(String),
    /// `<dir>/external/<stem>.bin` -- a real-world dataset's input.
    External(&'static str),
    /// `<dir>/external/<stem>/<i>.bin` -- a real-world dataset's pool decoded WHOLE per call (as the C++
    /// `osm_blocks` arms loop over every block): loaded as ONE buffer of length-prefixed frames, which
    /// a decoder walks with [`frames`].
    Batch(&'static str),
}

impl Source {
    /// The input's file, or its pool's directory.
    fn path(&self, dir: &Path) -> PathBuf {
        match self {
            Source::Arena(stem) => dir.join(format!("{stem}.bin")),
            Source::Stream(stem) => dir.join("stream").join(format!("{stem}.bin")),
            Source::Pool(stem) => dir.join("rv").join(stem),
            Source::External(stem) => dir.join("external").join(format!("{stem}.bin")),
            Source::Batch(stem) => dir.join("external").join(stem),
        }
    }

    fn is_pool(&self) -> bool {
        matches!(self, Source::Pool(_) | Source::Batch(_))
    }
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

/// Whether `source`'s input is in the payload directory `dir`. Only a real-world dataset's may be
/// missing (a dump from a C++ build without the corpus writes none), so this is `true` for every other
/// source -- loading one that is missing fails loudly instead.
pub fn available(dir: &Path, source: &Source) -> bool {
    match source {
        Source::External(_) => source.path(dir).is_file(),
        Source::Batch(_) => source.path(dir).join("pool.txt").is_file(),
        Source::Arena(_) | Source::Stream(_) | Source::Pool(_) => true,
    }
}

/// Load a scenario's input from the payload directory `dir`.
pub fn load(dir: &Path, source: &Source) -> Inputs {
    let path = source.path(dir);
    if !source.is_pool() {
        let buffer = read(&path);
        let bytes = buffer.len() as f64;
        return Inputs {
            buffers: vec![buffer],
            bytes,
        };
    }
    // Exactly the buffers the pool's `pool.txt` counts: a dump writes it last, so a pool an interrupted
    // dump left behind fails here instead of loading short.
    let pool: Vec<Vec<u8>> = (0..pool_len(&path))
        .map(|i| read(&path.join(format!("{i}.bin"))))
        .collect();
    let total = pool.iter().map(Vec::len).sum::<usize>() as f64;
    match source {
        Source::Batch(_) => Inputs {
            buffers: vec![batch(&pool)],
            bytes: total,
        },
        _ => Inputs {
            bytes: total / pool.len() as f64,
            buffers: pool,
        },
    }
}

/// `buffers` as one [`Source::Batch`] input: each prefixed with its length (u32, little-endian).
pub fn batch(buffers: &[Vec<u8>]) -> Vec<u8> {
    let mut out = Vec::with_capacity(buffers.iter().map(|b| 4 + b.len()).sum());
    for b in buffers {
        let len = u32::try_from(b.len()).expect("a batch buffer over 4 GiB");
        out.extend_from_slice(&len.to_le_bytes());
        out.extend_from_slice(b);
    }
    out
}

/// The buffers of a [`Source::Batch`] input, in order.
pub fn frames(mut batch: &[u8]) -> impl Iterator<Item = &[u8]> {
    std::iter::from_fn(move || {
        let (len, rest) = batch.split_first_chunk::<4>()?;
        let (frame, rest) = rest.split_at(u32::from_le_bytes(*len) as usize);
        batch = rest;
        Some(frame)
    })
}

fn pool_len(pool_dir: &Path) -> usize {
    let manifest = pool_dir.join("pool.txt");
    let text = String::from_utf8(read(&manifest)).unwrap_or_default();
    match text.trim().parse() {
        Ok(n) if n > 0 => n,
        _ => panic!("bad pool count in {manifest:?}"),
    }
}

/// [`Inputs::bytes`] from the file sizes alone -- for a caller that must state a throughput before it
/// loads the input (criterion).
pub fn bytes_per_call(dir: &Path, source: &Source) -> f64 {
    let len = |p: PathBuf| {
        std::fs::metadata(&p)
            .unwrap_or_else(|e| panic!("cannot stat payload {p:?}: {e}"))
            .len() as f64
    };
    let path = source.path(dir);
    if !source.is_pool() {
        return len(path);
    }
    let n = pool_len(&path);
    let total: f64 = (0..n).map(|i| len(path.join(format!("{i}.bin")))).sum();
    match source {
        Source::Batch(_) => total,
        _ => total / n as f64,
    }
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
