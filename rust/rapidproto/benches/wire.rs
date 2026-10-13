//! Micro-benchmarks for the wire-decode primitives. These MIRROR the C++ bench's `wire` arm
//! (tests/bench_streamgen.cpp) scenario-for-scenario -- same encodings, same field-loop shape, same
//! names -- so the Rust `wire` layer can be compared directly against the mature C++ one. Run with
//! `cargo bench -p rapidproto`. Measurement discipline (governor, one run at a time, prepared box)
//! is the same as the C++ bench; a verdict needs that box, not a loaded laptop.

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};
use rapidproto::wire::{Cursor, WireType};

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

// The Rust `wire` arm: a field loop over the Cursor, mirroring the C++ `wire` arm. `read_field`
// handles the scenario's field(s) (returning a checksum contribution) and returns None for any other
// field, which the loop skips -- exactly the handle-or-skip structure of the C++ arms. The returned
// checksum is black_box'd so the decode can't be optimized away. Generic (not a fn pointer) so the
// handler INLINES into the loop like the C++ arm's -- an indirect call per field would be an artifact.
fn drive<F: FnMut(u32, WireType, &mut Cursor) -> Option<u64>>(
    buf: &[u8],
    mut read_field: F,
) -> u64 {
    let mut cursor = Cursor::new(buf);
    let mut sum = 0u64;
    while let Ok(Some(tag)) = cursor.read_tag_or_end() {
        match read_field(tag.field, tag.wire, &mut cursor) {
            Some(contribution) => sum = sum.wrapping_add(contribution),
            None => {
                if cursor.skip(tag.wire).is_err() {
                    break;
                }
            }
        }
    }
    sum
}

fn scenario<F: Fn(u32, WireType, &mut Cursor) -> Option<u64> + Copy>(
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
    scenario(c, "varint-1byte", &buf, |f, w, cur| {
        (f == 1 && w == WireType::Varint).then(|| cur.read_varint().unwrap())
    });

    // varint-multibyte: field 1 varint, five-byte values.
    let mut buf = Vec::new();
    for i in 0..N {
        put_tag(&mut buf, 1, 0);
        put_varint(&mut buf, 0xF000_0000_u64 + ((i as u64) & 0xffff));
    }
    scenario(c, "varint-multibyte", &buf, |f, w, cur| {
        (f == 1 && w == WireType::Varint).then(|| cur.read_varint().unwrap())
    });

    // fixed64: field 15 i64, double bits.
    let mut buf = Vec::new();
    for i in 0..N {
        put_tag(&mut buf, 15, 1);
        buf.extend_from_slice(&(i as f64 * -2.25).to_bits().to_le_bytes());
    }
    scenario(c, "fixed64", &buf, |f, w, cur| {
        (f == 15 && w == WireType::I64).then(|| cur.read_fixed64().unwrap())
    });

    // string: field 12 length-delimited, 8-byte payloads.
    let mut buf = Vec::new();
    for _ in 0..N {
        put_tag(&mut buf, 12, 2);
        put_varint(&mut buf, 8);
        buf.extend_from_slice(b"abcdefgh");
    }
    scenario(c, "string", &buf, |f, w, cur| {
        (f == 12 && w == WireType::Len).then(|| cur.read_length_delimited().unwrap().len() as u64)
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
    scenario(c, "mixed", &buf, |f, w, cur| match (f, w) {
        (1, WireType::Varint) => Some(cur.read_varint().unwrap()),
        (6, WireType::Varint) => {
            Some(rapidproto::wire::zigzag64(cur.read_varint().unwrap()) as u64)
        }
        (15, WireType::I64) => Some(cur.read_fixed64().unwrap()),
        (12, WireType::Len) => Some(cur.read_length_delimited().unwrap().len() as u64),
        _ => None,
    });
}

criterion_group!(benches, bench_wire);
criterion_main!(benches);
