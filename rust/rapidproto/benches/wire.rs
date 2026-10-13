//! Micro-benchmarks for the wire-decode primitives, to track and tune their cost against the C++
//! `wire::` layer (compared on the same encodings; see docs/benchmarks once the numbers land). Run
//! with `cargo bench -p rapidproto`. Measurement discipline (governor, one run at a time) is the
//! same as the C++ bench -- a verdict needs the prepared bench box, not a loaded laptop.

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};
use rapidproto::wire::Cursor;

fn encode_varint(buf: &mut Vec<u8>, mut value: u64) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            buf.push(byte);
            return;
        }
        buf.push(byte | 0x80);
    }
}

fn repeated_varints(count: usize, value: u64) -> Vec<u8> {
    let mut buf = Vec::new();
    for _ in 0..count {
        encode_varint(&mut buf, value);
    }
    buf
}

// Decode every varint in `buf` to steady-state (amortized per-byte cost, the comparison signal).
fn drain_varints(buf: &[u8]) -> u64 {
    let mut cursor = Cursor::new(buf);
    let mut acc = 0u64;
    while !cursor.is_empty() {
        acc = acc.wrapping_add(cursor.read_varint().expect("valid varint buffer"));
    }
    acc
}

fn bench_varint(c: &mut Criterion) {
    let mut group = c.benchmark_group("read_varint");
    for (label, value) in [
        ("w1", 0x7f_u64),
        ("w2", 0x3fff),
        ("w5", 0xffff_ffff),
        ("w10", u64::MAX),
    ] {
        let buf = repeated_varints(4096, value);
        group.throughput(Throughput::Bytes(buf.len() as u64));
        group.bench_function(label, |b| {
            b.iter(|| black_box(drain_varints(black_box(&buf))))
        });
    }
    group.finish();
}

// A realistic mixed message, decoded with the field loop: tag + per-wire-type read/skip. Fields:
// 1 varint, 2 fixed64, 3 length-delimited (16 bytes), 5 fixed32 -- repeated to fill the buffer.
fn mixed_message(records: usize) -> Vec<u8> {
    let mut buf = Vec::new();
    for _ in 0..records {
        encode_varint(&mut buf, 1 << 3); // field 1, varint (wire type 0)
        encode_varint(&mut buf, 300);
        encode_varint(&mut buf, (2 << 3) | 1); // field 2, i64
        buf.extend_from_slice(&0x0102_0304_0506_0708_u64.to_le_bytes());
        encode_varint(&mut buf, (3 << 3) | 2); // field 3, len
        encode_varint(&mut buf, 16);
        buf.extend_from_slice(&[0xab; 16]);
        encode_varint(&mut buf, (5 << 3) | 5); // field 5, i32
        buf.extend_from_slice(&0x1122_3344_u32.to_le_bytes());
    }
    buf
}

fn bench_field_scan(c: &mut Criterion) {
    let buf = mixed_message(512);
    let mut group = c.benchmark_group("field_scan");
    group.throughput(Throughput::Bytes(buf.len() as u64));
    group.bench_function("tag_and_skip", |b| {
        b.iter(|| {
            let mut cursor = Cursor::new(black_box(&buf));
            while let Ok(Some(tag)) = cursor.read_tag_or_end() {
                cursor.skip(tag.wire).expect("valid message");
            }
            black_box(cursor.offset())
        });
    });
    group.finish();
}

fn bench_payloads(c: &mut Criterion) {
    // 256 length-delimited 32-byte payloads, read without copying.
    let mut lends = Vec::new();
    for _ in 0..256 {
        encode_varint(&mut lends, 32);
        lends.extend_from_slice(&[0x5a; 32]);
    }
    // 1024 fixed64 values.
    let fixed = vec![0x7fu8; 1024 * 8];

    let mut group = c.benchmark_group("payloads");
    group.throughput(Throughput::Bytes(lends.len() as u64));
    group.bench_function("length_delimited", |b| {
        b.iter(|| {
            let mut cursor = Cursor::new(black_box(&lends));
            let mut total = 0usize;
            while !cursor.is_empty() {
                total += cursor
                    .read_length_delimited()
                    .expect("valid payloads")
                    .len();
            }
            black_box(total)
        });
    });
    group.throughput(Throughput::Bytes(fixed.len() as u64));
    group.bench_function("fixed64", |b| {
        b.iter(|| {
            let mut cursor = Cursor::new(black_box(&fixed));
            let mut acc = 0u64;
            while !cursor.is_empty() {
                acc ^= cursor.read_fixed64().expect("valid fixed buffer");
            }
            black_box(acc)
        });
    });
    group.finish();
}

criterion_group!(benches, bench_varint, bench_field_scan, bench_payloads);
criterion_main!(benches);
