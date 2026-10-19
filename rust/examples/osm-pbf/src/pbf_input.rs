//! Plumbing shared by the osmstat paths: file reading, blob framing, zlib inflate. None of this is
//! RapidProto-specific -- it is the generic .osm.pbf container handling that gets you to the protobuf
//! bytes; `main` does the actual decode with the generated arena decoders.
//!
//! A .osm.pbf file is a sequence of `[4-byte big-endian length][BlobHeader][Blob]`: the BlobHeader
//! names the blob type and the Blob's size, the Blob wraps the payload raw or zlib-deflated.

use std::io::Read;
use std::time::Instant;

use flate2::read::ZlibDecoder;

// The spec keeps a BlobHeader under 64 KiB and a Blob's uncompressed payload under 32 MiB ("should"
// for writers, so the inflate cap allows 2x headroom). These bounds cap every allocation made from
// untrusted input.
pub const MAX_BLOB_HEADER: u32 = 64 * 1024;
pub const MAX_BLOB: i64 = 32 * 1024 * 1024;
pub const MAX_RAW_SIZE: i64 = 2 * MAX_BLOB;

/// One framing step: the BlobHeader bytes at `*offset`, advancing past them. Returns `None` at clean
/// end-of-file (`*offset == file.len()`) AND on malformed framing -- but the latter reports the error
/// and poisons `*offset` PAST the end, so after the loop a caller tells them apart with
/// `*offset == file.len()`.
pub fn next_frame<'a>(file: &'a [u8], offset: &mut usize) -> Option<&'a [u8]> {
    if *offset >= file.len() {
        return None; // clean EOF at ==; a poisoned offset (>) stays that way
    }
    if file.len() - *offset < 4 {
        eprintln!("osmstat: truncated frame length at offset {offset}");
        *offset = file.len() + 1;
        return None;
    }
    let b = |i: usize| file[*offset + i] as u32;
    let header_len = (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
    *offset += 4;
    if header_len >= MAX_BLOB_HEADER || (file.len() - *offset) < header_len as usize {
        eprintln!(
            "osmstat: bad BlobHeader length {header_len} at offset {}",
            *offset - 4
        );
        *offset = file.len() + 1;
        return None;
    }
    let header = &file[*offset..*offset + header_len as usize];
    *offset += header_len as usize;
    Some(header)
}

/// The Blob bytes after a frame's header, sized by the BlobHeader's `datasize` field.
pub fn take_blob<'a>(file: &'a [u8], offset: &mut usize, datasize: i32) -> Option<&'a [u8]> {
    if datasize < 0 || datasize as i64 > MAX_BLOB || (file.len() - *offset) < datasize as usize {
        eprintln!("osmstat: bad Blob datasize {datasize} at offset {offset}");
        *offset = file.len() + 1;
        return None;
    }
    let blob = &file[*offset..*offset + datasize as usize];
    *offset += datasize as usize;
    Some(blob)
}

/// Resolve a Blob's payload once `main` has extracted the oneof members: raw bytes pass through, zlib
/// inflates into the caller's reused buffer (sized to `raw_size`). `None` (already reported) on a bad
/// `raw_size` or a failed inflate. The returned slice borrows `raw` or `inflated`.
pub fn blob_payload<'a>(
    raw: &'a [u8],
    deflated: &[u8],
    raw_size: i64,
    inflated: &'a mut Vec<u8>,
) -> Option<&'a [u8]> {
    if deflated.is_empty() {
        return Some(raw);
    }
    if raw_size <= 0 || raw_size > MAX_RAW_SIZE {
        eprintln!("osmstat: bad raw_size {raw_size}");
        return None;
    }
    inflated.clear();
    inflated.reserve(raw_size as usize);
    // The blob's zlib stream must inflate to EXACTLY `raw_size` bytes. Cap the reader at `raw_size + 1`
    // so a hostile stream stays bounded AND an oversized one reads one byte too many -- then the exact
    // length check below rejects both too-short and too-long.
    let mut dec = ZlibDecoder::new(deflated).take(raw_size as u64 + 1);
    match dec.read_to_end(inflated) {
        Ok(_) if inflated.len() == raw_size as usize => Some(inflated),
        Ok(got) => {
            eprintln!("osmstat: zlib inflate size mismatch (got {got}, want {raw_size})");
            None
        }
        Err(e) => {
            eprintln!("osmstat: zlib inflate failed ({e})");
            None
        }
    }
}

/// Read a whole file into memory, or `None` on any I/O error (the whole payload is mapped at once --
/// the planet file is large but this keeps the example simple and the framing a plain slice walk).
pub fn read_file(path: &str) -> Option<Vec<u8>> {
    std::fs::read(path).ok()
}

/// Wall-clock timing for the stderr report.
pub struct Stopwatch {
    t0: Instant,
}

impl Stopwatch {
    pub fn new() -> Self {
        Self { t0: Instant::now() }
    }

    /// Seconds since the last `take` (or construction), and reset.
    pub fn take(&mut self) -> f64 {
        let now = Instant::now();
        let s = now.duration_since(self.t0).as_secs_f64();
        self.t0 = now;
        s
    }
}
