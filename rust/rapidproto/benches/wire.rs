//! Micro-benchmarks for the wire-decode primitives. These MIRROR the C++ bench's `wire` arm
//! (tests/bench_streamgen.cpp) scenario-for-scenario -- same encodings, same field-loop shape, same
//! names -- so the Rust `wire` layer can be compared directly against the mature C++ one. Run with
//! `cargo bench -p rapidproto`. Measurement discipline (governor, one run at a time, prepared box)
//! is the same as the C++ bench; a verdict needs that box, not a loaded laptop.

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
// the shape the C++ `wire` arm uses (it threads a bare pointer). `read_field` handles the scenario's
// field(s), advancing `*rest` and returning a checksum contribution, or None for any other field,
// which the loop skips -- the handle-or-skip structure of the C++ arms. The checksum is black_box'd so
// the decode can't be elided. Generic (not a fn pointer) so the handler INLINES into the loop like the
// C++ arm's -- an indirect call per field would be an artifact.
fn drive<F: FnMut(u32, WireType, &mut &[u8]) -> Option<u64>>(buf: &[u8], mut read_field: F) -> u64 {
    let mut rest = buf;
    let mut sum = 0u64;
    while !rest.is_empty() {
        let mut tag = Tag {
            field: 0,
            wire: WireType::Varint,
        };
        rest = match wire::read_tag(rest, &mut tag) {
            Ok(r) => r,
            Err(_) => break,
        };
        match read_field(tag.field, tag.wire, &mut rest) {
            Some(contribution) => sum = sum.wrapping_add(contribution),
            None => {
                rest = match wire::skip(rest, tag.wire) {
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
