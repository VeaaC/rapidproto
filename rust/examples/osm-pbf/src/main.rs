//! `osmstat` -- decode an OpenStreetMap .osm.pbf file with RapidProto's arena decoders and print
//! statistics (counts, tags, bounding box, top tag keys). It is a worked example of decoding real,
//! bulky protobuf efficiently with this crate.
//!
//! A .osm.pbf is a sequence of `[4-byte BE length][BlobHeader][Blob]`, and each data Blob holds an
//! independent `PrimitiveBlock`. That independence is the point: by default the blocks decode in
//! parallel across cores (rayon), each into its own [`Arena`], and their partial stats merge
//! order-independently -- `--sequential` runs the exact same per-block work on one thread and prints
//! identical output. `--baseline` additionally decodes the file with the `osmpbf` crate and prints its
//! throughput beside ours, for comparison.
//!
//! The decode flow a crate user cares about is short: give each block a fresh `Arena`, call the
//! generated `decode()`, then read fields off the borrowed view ([`walk`]). Framing and zlib inflate
//! (neither specific to RapidProto) live in [`pbf_input`]; [`process_block`] ties one block together
//! and is what both the parallel and sequential paths run.

mod baseline;
mod osm;
mod pbf_input;
mod stats;
mod walk;

use std::io;
use std::process::ExitCode;

use rapidproto::Arena;
use rayon::prelude::*;

use osm::OSMPBF::{blob::Data, Blob, BlobHeader, HeaderBlock, PrimitiveBlock};
use pbf_input::{blob_payload, next_frame, read_file, take_blob, Stopwatch};
use stats::{feature_supported, mib_per_s, Stats};
use walk::walk_block;

// Decode a Blob's payload: extract the oneof (raw bytes or zlib), then resolve to a byte slice. The
// returned slice borrows `blob_bytes` (raw) or `inflated` (zlib), so both -- plus `blob_arena` -- must
// outlive it.
fn blob_to_payload<'a>(
    blob_bytes: &'a [u8],
    blob_arena: &'a Arena<'a>,
    inflated: &'a mut Vec<u8>,
) -> Result<&'a [u8], String> {
    let blob = Blob::decode(blob_bytes, blob_arena).map_err(|e| format!("bad Blob ({e:?})"))?;
    let raw_size = blob.raw_size().unwrap_or(0) as i64;
    let (raw, deflated): (&[u8], &[u8]) = match blob.data() {
        Some(Data::Raw(v)) => (v, &[]),
        Some(Data::ZlibData(v)) => (&[], v),
        Some(_) => return Err("unsupported blob compression (only raw/zlib)".into()),
        None => return Err("Blob carries no data".into()),
    };
    blob_payload(raw, deflated, raw_size, inflated).ok_or_else(|| "blob payload failed".into())
}

// Decode one OSMData Blob (wire bytes) and walk its PrimitiveBlock into a per-block `Stats`.
// Self-contained (own arenas + inflate buffer), so it runs on any thread.
fn process_block(blob_bytes: &[u8]) -> Result<Stats, String> {
    let blob_arena = Arena::new();
    let arena = Arena::new();
    let mut inflated = Vec::new();
    let payload = blob_to_payload(blob_bytes, &blob_arena, &mut inflated)?;
    let block = PrimitiveBlock::decode(payload, &arena)
        .map_err(|e| format!("bad PrimitiveBlock ({e:?})"))?;
    let mut s = walk_block(block);
    s.payload_bytes = payload.len() as u64;
    Ok(s)
}

// An OSMHeader block: decode it and refuse any required feature we do not implement.
fn check_header(blob_bytes: &[u8]) -> Result<(), String> {
    let blob_arena = Arena::new();
    let arena = Arena::new();
    let mut inflated = Vec::new();
    let payload = blob_to_payload(blob_bytes, &blob_arena, &mut inflated)?;
    let hb =
        HeaderBlock::decode(payload, &arena).map_err(|e| format!("bad OSMHeader block ({e:?})"))?;
    for f in hb.required_features() {
        let name = String::from_utf8_lossy(f.as_bytes());
        if !feature_supported(&name) {
            return Err(format!("file requires unsupported feature '{name}'"));
        }
    }
    Ok(())
}

struct Args {
    path: String,
    sequential: bool,
    baseline: bool,
}

fn parse_args() -> Result<Args, String> {
    let mut path = None;
    let mut sequential = false;
    let mut baseline = false;
    for arg in std::env::args().skip(1) {
        match arg.as_str() {
            "--sequential" => sequential = true,
            "--baseline" => baseline = true,
            s if s.starts_with("--") => return Err(format!("unknown flag {s}")),
            s => {
                if path.replace(s.to_owned()).is_some() {
                    return Err("more than one input file".into());
                }
            }
        }
    }
    match path {
        Some(path) => Ok(Args {
            path,
            sequential,
            baseline,
        }),
        None => Err("no input file".into()),
    }
}

fn run() -> Result<(), String> {
    let args = parse_args()?;
    let mut clock = Stopwatch::new();
    let file = read_file(&args.path).ok_or_else(|| format!("cannot read {}", args.path))?;
    let t_read = clock.take();

    // Framing pass: decode each BlobHeader, feature-check the OSMHeader, collect the OSMData blob
    // slices (they borrow `file`). Unknown blob types are reserved for future use -- skip them.
    let mut offset = 0;
    let mut data_blobs: Vec<&[u8]> = Vec::new();
    let mut header_seen = false;
    while let Some(hbytes) = next_frame(&file, &mut offset) {
        let frame_arena = Arena::new();
        let bh = BlobHeader::decode(hbytes, &frame_arena)
            .map_err(|e| format!("bad BlobHeader ({e:?})"))?;
        let is_header = bh.r#type() == "OSMHeader";
        let is_data = bh.r#type() == "OSMData";
        let blob = take_blob(&file, &mut offset, bh.datasize())
            .ok_or_else(|| "bad Blob framing".to_owned())?;
        if is_header {
            check_header(blob)?;
            header_seen = true;
        } else if is_data {
            data_blobs.push(blob);
        }
    }
    if offset != file.len() {
        return Err("malformed framing".into()); // next_frame/take_blob already reported where
    }
    if !header_seen {
        return Err("no OSMHeader block -- not an .osm.pbf file?".into());
    }
    let t_frame = clock.take();

    // Decode + walk every data block. The blocks are independent, so parallel is the default: each
    // rayon task owns its arenas; `try_fold` accumulates within a worker and `try_reduce` merges the
    // workers. Sequential is the same `process_block` folded on one thread -- identical output.
    clock.take();
    let stats: Stats = if args.sequential {
        data_blobs
            .iter()
            .try_fold(Stats::default(), |mut acc, &b| {
                acc.merge(process_block(b)?);
                Ok::<_, String>(acc)
            })?
    } else {
        data_blobs
            .par_iter()
            .try_fold(Stats::default, |mut acc, &b| {
                acc.merge(process_block(b)?);
                Ok::<_, String>(acc)
            })
            .try_reduce(Stats::default, |mut a, b| {
                a.merge(b);
                Ok(a)
            })?
    };
    let t_process = clock.take();

    let out = io::stdout();
    stats
        .print(&mut out.lock())
        .map_err(|e| format!("write stats: {e}"))?;

    let mode = if args.sequential {
        "sequential, 1 thread".to_owned()
    } else {
        format!("parallel, {} threads", rayon::current_num_threads())
    };
    eprintln!("model            arena ({mode})");
    eprintln!(
        "read             {t_read:.3}s ({:.1} MiB/s, {} bytes)",
        mib_per_s(file.len() as u64, t_read),
        file.len()
    );
    eprintln!("frame            {t_frame:.3}s");
    eprintln!(
        "process          {t_process:.3}s ({:.1} MiB/s of payload, {} payload bytes)",
        mib_per_s(stats.payload_bytes, t_process),
        stats.payload_bytes
    );

    if args.baseline {
        baseline::run(&args.path)?;
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(msg) => {
            eprintln!("osmstat: {msg}");
            ExitCode::from(1)
        }
    }
}
