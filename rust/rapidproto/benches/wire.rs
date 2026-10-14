//! Micro-benchmarks for the wire-decode primitives. These MIRROR the C++ bench's `wire` arm
//! (tests/bench_streamgen.cpp) scenario-for-scenario -- same encodings, same fused field-loop driver,
//! same names -- so the Rust `wire` layer can be compared directly against the mature C++ one.
//!
//! RUN IT PINNED AND ALIGNED, or the numbers lie:
//!
//! ```text
//! RUSTFLAGS="-Cllvm-args=-align-all-functions=6" taskset -c 0 cargo bench -p rapidproto --bench wire
//! ```
//!
//! - `taskset -c 0` pins the thread: unpinned, criterion migrates across cores and run-to-run swings
//!   up to ~15% (worst on the memory-bound `string` arm) -- it is NOT the code changing. The C++ bench
//!   is pinned the same way.
//! - `-align-all-functions=6` pins 64-byte function entries (the C++ bench build does this via
//!   `-falign-functions=64`). Without it, an unrelated edit relays out the binary and shifts each
//!   arm's throughput by ~10-40% purely from code placement -- a swing on an arm whose code did not
//!   change is layout, not signal. The placement-invariant signal is instructions/byte, which the C++
//!   harness reports and criterion does not.
//!
//! Also: `performance` governor, turbo off, one run at a time, a quiesced box -- a verdict needs that
//! box, not a loaded laptop. See the project's bench-placement notes.

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};
use rapidproto::wire::{self, Tag, WireType};

const N: usize = 2_000_000; // matches the C++ bench's kN

// Encoder-side buffer builders, mirroring the C++ put_* helpers. The decoder is the SUT, not these.
fn put_varint(b: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 {
        b.push(0x80 | (v & 0x7f) as u8);
        v >>= 7;
    }
    b.push(v as u8);
}
fn put_tag(b: &mut Vec<u8>, field: u32, wire: u32) {
    put_varint(b, (u64::from(field) << 3) | u64::from(wire));
}
fn zigzag_enc(v: i64) -> u64 {
    ((v << 1) ^ (v >> 63)) as u64
}

// The Rust `wire` arm: a field loop that threads the remaining-bytes slice BY VALUE through the
// value-threaded `wire::` readers -- exactly how a generated `decode` body consumes the input, and
// the shape the C++ `wire` arm uses (it threads a bare pointer). The loop is driven by the fused
// `read_tag_or_end` (one bounds check yields end / tag / error), matching the C++ arm's driver.
// `read_field` handles the scenario's field(s), advancing `*rest` and returning a checksum
// contribution, or None for any other field, which the loop skips -- the handle-or-skip structure of
// the C++ arms. The checksum is black_box'd so the decode can't be elided. Generic (not a fn pointer)
// so the handler INLINES into the loop like the C++ arm's -- an indirect call per field is an artifact.
fn drive<F: FnMut(u32, WireType, &mut &[u8]) -> Option<u64>>(buf: &[u8], mut read_field: F) -> u64 {
    let mut rest = buf;
    let mut sum = 0u64;
    loop {
        let mut tag = Tag {
            field: 0,
            wire: WireType::Varint,
        };
        // Fused driver: one bounds check yields end / tag / error, mirroring the C++ `wire` arm.
        rest = match wire::read_tag_or_end(rest, &mut tag) {
            Ok(Some(r)) => r,
            Ok(None) => break, // clean end of input
            Err(_) => break,
        };
        match read_field(tag.field, tag.wire, &mut rest) {
            Some(contribution) => sum = sum.wrapping_add(contribution),
            None => {
                rest = match wire::skip(rest, tag) {
                    Ok(r) => r,
                    Err(_) => break,
                };
            }
        }
    }
    sum
}

fn scenario<F: Fn(u32, WireType, &mut &[u8]) -> Option<u64> + Copy>(
    c: &mut Criterion,
    name: &str,
    buf: &[u8],
    read_field: F,
) {
    let mut group = c.benchmark_group(name);
    group.throughput(Throughput::Bytes(buf.len() as u64));
    group.bench_function("wire", |b| {
        b.iter(|| black_box(drive(black_box(buf), read_field)));
    });
    group.finish();
}

fn bench_wire(c: &mut Criterion) {
    // varint-1byte: field 1 varint, one-byte values.
    let mut buf = Vec::new();
    for i in 0..N {
        put_tag(&mut buf, 1, 0);
        put_varint(&mut buf, (i as u64) & 0x7f);
    }
    scenario(c, "varint-1byte", &buf, |f, w, rest| {
        if f == 1 && w == WireType::Varint {
            let mut v = 0;
            *rest = wire::read_varint(rest, &mut v).unwrap();
            Some(v)
        } else {
            None
        }
    });

    // varint-multibyte: field 1 varint, five-byte values.
    let mut buf = Vec::new();
    for i in 0..N {
        put_tag(&mut buf, 1, 0);
        put_varint(&mut buf, 0xF000_0000_u64 + ((i as u64) & 0xffff));
    }
    scenario(c, "varint-multibyte", &buf, |f, w, rest| {
        if f == 1 && w == WireType::Varint {
            let mut v = 0;
            *rest = wire::read_varint(rest, &mut v).unwrap();
            Some(v)
        } else {
            None
        }
    });

    // tag-2byte: field 18 varint (a 2-byte tag, 0x90 0x01), one-byte values -- exercises the fused
    // 2-byte tag path (fields 16..2047). Mirrors the C++ bench's >15-field (field 18) scenario.
    let mut buf = Vec::new();
    for i in 0..N {
        put_tag(&mut buf, 18, 0);
        put_varint(&mut buf, (i as u64) & 0x7f);
    }
    scenario(c, "tag-2byte", &buf, |f, w, rest| {
        if f == 18 && w == WireType::Varint {
            let mut v = 0;
            *rest = wire::read_varint(rest, &mut v).unwrap();
            Some(v)
        } else {
            None
        }
    });

    // fixed64: field 15 i64, double bits.
    let mut buf = Vec::new();
    for i in 0..N {
        put_tag(&mut buf, 15, 1);
        buf.extend_from_slice(&(i as f64 * -2.25).to_bits().to_le_bytes());
    }
    scenario(c, "fixed64", &buf, |f, w, rest| {
        if f == 15 && w == WireType::I64 {
            let mut v = 0;
            *rest = wire::read_fixed64(rest, &mut v).unwrap();
            Some(v)
        } else {
            None
        }
    });

    // string: field 12 length-delimited, 8-byte payloads.
    let mut buf = Vec::new();
    for _ in 0..N {
        put_tag(&mut buf, 12, 2);
        put_varint(&mut buf, 8);
        buf.extend_from_slice(b"abcdefgh");
    }
    scenario(c, "string", &buf, |f, w, rest| {
        if f == 12 && w == WireType::Len {
            let mut payload: &[u8] = &[];
            *rest = wire::read_length_delimited(rest, &mut payload).unwrap();
            Some(payload.len() as u64)
        } else {
            None
        }
    });

    // mixed: fields 1 (varint), 6 (zigzag varint), 15 (fixed64), 12 (length-delimited).
    let mut buf = Vec::new();
    for i in 0..N {
        put_tag(&mut buf, 1, 0);
        put_varint(&mut buf, i as u64);
        put_tag(&mut buf, 6, 0);
        put_varint(
            &mut buf,
            zigzag_enc(i as i64 * if i & 1 != 0 { -7 } else { 7 }),
        );
        put_tag(&mut buf, 15, 1);
        buf.extend_from_slice(&(i as f64 * 0.5).to_bits().to_le_bytes());
        put_tag(&mut buf, 12, 2);
        put_varint(&mut buf, 4);
        buf.extend_from_slice(b"wxyz");
    }
    scenario(c, "mixed", &buf, |f, w, rest| match (f, w) {
        (1, WireType::Varint) => {
            let mut v = 0;
            *rest = wire::read_varint(rest, &mut v).unwrap();
            Some(v)
        }
        (6, WireType::Varint) => {
            let mut v = 0;
            *rest = wire::read_varint(rest, &mut v).unwrap();
            Some(wire::zigzag64(v) as u64)
        }
        (15, WireType::I64) => {
            let mut v = 0;
            *rest = wire::read_fixed64(rest, &mut v).unwrap();
            Some(v)
        }
        (12, WireType::Len) => {
            let mut payload: &[u8] = &[];
            *rest = wire::read_length_delimited(rest, &mut payload).unwrap();
            Some(payload.len() as u64)
        }
        _ => None,
    });
}

criterion_group!(benches, bench_wire);
criterion_main!(benches);
