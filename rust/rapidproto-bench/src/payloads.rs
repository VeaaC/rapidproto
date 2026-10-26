//! The shared payload directory (`RAPIDPROTO_BENCH_DIR`) -- the bytes the C++ benches write in their
//! dump mode (`RAPIDPROTO_BENCH_DUMP_DIR`, see `tests/bench_payloads.hpp`), so every Rust number is
//! measured on byte-identical input to its C++ counterpart. The runners build none of them. Layout:
//!
//! ```text
//! <dir>/<name>.bin           an arena scenario, protoc-serialized   (rapidproto_arena_bench)
//! <dir>/stream/<stem>.bin    a streaming scenario                   (rapidproto_bench)
//! <dir>/rv/<stem>.pool       a varint-sweep pool                    (both)
//! <dir>/external/<stem>.bin  a real-world dataset's input           (rapidproto_arena_bench)
//! <dir>/external/<stem>.pool a real-world dataset's pool            (rapidproto_arena_bench)
//! <dir>/external/<stem>.sum  the C++ arms' checksum over that input (rapidproto_arena_bench)
//! ```
//!
//! A `.pool` file holds a pool's buffers in order, each a 4-byte little-endian length and its bytes:
//! the [`Source::Batch`] layout.
//!
//! The varint sweeps decode a POOL of distinct-seed buffers per scenario, in whole passes, so no arm
//! replays one memorized width sequence (the C++ rationale, `tests/bench_varint.hpp`). Their
//! scenario names and axes are mirrored here from that header: the names must match the C++ tables.
//!
//! A real-world dataset's input exists only when the C++ bench was built with that dataset fetched
//! (`tests/fetch_corpus.py`; OSM's extract also `tests/fetch_osm_dataset.py`), so its scenarios are
//! skipped without it ([`Source::is_external`]); every other payload is required. The Rust walks over those
//! inputs are ported from the C++ ones field for field, so the runner holds them to the dumped C++
//! checksum ([`cpp_checksum`]).

use std::io::{BufReader, Cursor, Read, Seek};
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
    /// `<dir>/rv/<stem>.pool` -- a varint-sweep pool.
    Pool(String),
    /// `<dir>/external/<stem>.bin` -- a real-world dataset's input.
    External(&'static str),
    /// `<dir>/external/<stem>.pool` -- a real-world dataset's pool decoded WHOLE per call (as the C++
    /// `osm_blocks` arms loop over every block): loaded as ONE buffer, the file itself, whose
    /// length-prefixed frames a decoder walks with [`frames`].
    Batch(&'static str),
}

impl Source {
    /// The input's file.
    fn path(&self, dir: &Path) -> PathBuf {
        match self {
            Source::Arena(stem) => dir.join(format!("{stem}.bin")),
            Source::Stream(stem) => dir.join("stream").join(format!("{stem}.bin")),
            Source::Pool(stem) => dir.join("rv").join(format!("{stem}.pool")),
            Source::External(stem) => dir.join("external").join(format!("{stem}.bin")),
            Source::Batch(stem) => dir.join("external").join(format!("{stem}.pool")),
        }
    }

    fn is_pool(&self) -> bool {
        matches!(self, Source::Pool(_) | Source::Batch(_))
    }

    /// A real-world dataset's input: the one kind a payload directory may lack (a dump from a C++
    /// build without that dataset writes none), so the runner skips it rather than fail.
    pub fn is_external(&self) -> bool {
        matches!(self, Source::External(_) | Source::Batch(_))
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

/// Whether `source`'s input is in the payload directory `dir`.
pub fn present(dir: &Path, source: &Source) -> bool {
    source.path(dir).is_file()
}

/// The C++ arms' checksum over a real-world dataset's input (`<dir>/external/<stem>.sum`, which the dump
/// writes beside every such input); `None` for every other source, whose checksums follow this crate's
/// own convention.
pub fn cpp_checksum(dir: &Path, source: &Source) -> Option<u64> {
    let (Source::External(stem) | Source::Batch(stem)) = source else {
        return None;
    };
    let path = dir.join("external").join(format!("{stem}.sum"));
    assert!(
        path.is_file(),
        "{path:?} is missing: the payload directory predates the recorded C++ checksums -- re-dump \
         it with the C++ arena bench"
    );
    let text = String::from_utf8(read(&path)).unwrap_or_default();
    Some(
        text.trim()
            .parse()
            .unwrap_or_else(|_| panic!("bad checksum in {path:?}")),
    )
}

/// Load a scenario's input from the payload directory `dir`.
pub fn load(dir: &Path, source: &Source) -> Inputs {
    let path = source.path(dir);
    let file = read(&path);
    if !source.is_pool() {
        let bytes = file.len() as f64;
        return Inputs {
            buffers: vec![file],
            bytes,
        };
    }
    let (n, total) = pool_size(&path, Cursor::new(&file), file.len() as u64);
    match source {
        Source::Batch(_) => Inputs {
            buffers: vec![file],
            bytes: total as f64,
        },
        _ => Inputs {
            buffers: frames(&file).map(<[u8]>::to_vec).collect(),
            bytes: total as f64 / n as f64,
        },
    }
}

/// `buffers` in the `.pool` layout: each prefixed with its length (u32, little-endian).
pub fn batch(buffers: &[Vec<u8>]) -> Vec<u8> {
    let mut out = Vec::with_capacity(buffers.iter().map(|b| 4 + b.len()).sum());
    for b in buffers {
        let len = u32::try_from(b.len()).expect("a pool buffer over 4 GiB");
        out.extend_from_slice(&len.to_le_bytes());
        out.extend_from_slice(b);
    }
    out
}

/// The buffers of a `.pool` file's bytes (a [`Source::Batch`] input), in order. Panics on a malformed
/// pool.
pub fn frames(mut batch: &[u8]) -> impl Iterator<Item = &[u8]> {
    std::iter::from_fn(move || {
        if batch.is_empty() {
            return None;
        }
        let (len, rest) = batch
            .split_first_chunk::<4>()
            .expect("not a pool: a truncated buffer length");
        let (frame, rest) = rest
            .split_at_checked(u32::from_le_bytes(*len) as usize)
            .expect("not a pool: a buffer past the end");
        batch = rest;
        Some(frame)
    })
}

/// The buffer count and total buffer size of the `.pool` file `path`, whose `size` bytes `pool` reads:
/// only the length prefixes are read. Panics, naming `path`, on a malformed or empty pool or an empty
/// buffer (which would decode as an empty message and silently "measure" nothing).
fn pool_size(path: &Path, mut pool: impl Read + Seek, size: u64) -> (usize, u64) {
    let (mut n, mut at) = (0, 0);
    while at < size {
        let mut len = [0u8; 4];
        pool.read_exact(&mut len)
            .unwrap_or_else(|e| panic!("{path:?} is not a pool: {e}"));
        let len = u32::from_le_bytes(len);
        assert!(len > 0, "{path:?} has an empty buffer");
        pool.seek_relative(i64::from(len))
            .unwrap_or_else(|e| panic!("cannot read pool {path:?}: {e}"));
        at += 4 + u64::from(len);
        n += 1;
    }
    assert!(at == size, "{path:?} is not a pool: a buffer past the end");
    assert!(n > 0, "{path:?} is empty");
    (n, size - 4 * n as u64)
}

/// [`Inputs::bytes`] without loading the input -- for a caller that must state a throughput before it
/// loads it (criterion).
pub fn bytes_per_call(dir: &Path, source: &Source) -> f64 {
    let path = source.path(dir);
    if !source.is_pool() {
        return std::fs::metadata(&path)
            .unwrap_or_else(|e| panic!("cannot stat payload {path:?}: {e}"))
            .len() as f64;
    }
    let file =
        std::fs::File::open(&path).unwrap_or_else(|e| panic!("cannot read payload {path:?}: {e}"));
    let size = file
        .metadata()
        .unwrap_or_else(|e| panic!("cannot stat payload {path:?}: {e}"))
        .len();
    let (n, total) = pool_size(&path, BufReader::new(file), size);
    match source {
        Source::Batch(_) => total as f64,
        _ => total as f64 / n as f64,
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

/// A sweep scenario's pool: its name as a file stem (spaces -> `-`).
pub fn pool(name: &str) -> Source {
    Source::Pool(name.replace(' ', "-"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn size(pool: &[u8]) -> (usize, u64) {
        pool_size(Path::new("test.pool"), Cursor::new(pool), pool.len() as u64)
    }

    #[test]
    fn reads_what_batch_writes() {
        let buffers = vec![vec![1u8, 2, 3], vec![4], vec![5, 6]];
        let pool = batch(&buffers);
        assert_eq!(size(&pool), (3, 6));
        assert!(frames(&pool).eq(buffers.iter().map(Vec::as_slice)));
    }

    #[test]
    #[should_panic(expected = "a buffer past the end")]
    fn rejects_a_truncated_buffer() {
        let mut pool = batch(&[vec![1, 2, 3]]);
        pool.pop();
        size(&pool);
    }

    #[test]
    #[should_panic(expected = "is not a pool")]
    fn rejects_a_truncated_length() {
        let mut pool = batch(&[vec![1]]);
        pool.extend_from_slice(&[2, 0]);
        size(&pool);
    }

    #[test]
    #[should_panic(expected = "has an empty buffer")]
    fn rejects_an_empty_buffer() {
        size(&batch(&[vec![1], vec![]]));
    }

    #[test]
    #[should_panic(expected = "is empty")]
    fn rejects_an_empty_pool() {
        size(&[]);
    }
}
