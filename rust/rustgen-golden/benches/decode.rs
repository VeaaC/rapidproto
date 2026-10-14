//! Decode benchmark for the generated Rust arena decoder, over the SHARED benchmark schema
//! `tests/bench/bench.proto` (package `bench`) that the C++ arena bench also uses. It decodes a
//! realistic `bench.Dataset` (people with scalars, strings, a nested message, repeated scalars/strings/
//! sub-messages, and a map) into a fresh arena and checksums the tree, so the perf pass can A/B each
//! optimization against a stable, representative workload.
//!
//! Input: set `RAPIDPROTO_BENCH_DATASET` to a shared protoc-canonical `.bin` (produced by the C++
//! arena bench's generator mode -- see `load_payload` below) and BOTH this bench and the C++ bench
//! decode byte-identical bytes, so their numbers are cross-language comparable. Unset, this bench
//! falls back to a built-in hand-built payload -- fine for Rust-vs-Rust A/B, but NOT comparable to C++
//! (the hand-built bytes aren't protoc-canonical: they emit implicit-zero fields protoc omits).
//!
//! RUN IT PINNED AND ALIGNED, or the numbers lie (same discipline as the wire bench):
//!
//! ```text
//! RUSTFLAGS="-Cllvm-args=-align-all-functions=6" taskset -c 0 cargo bench -p rustgen-golden --bench decode
//! ```
//!
//! - `taskset -c 0` pins the thread (unpinned, criterion migrates cores -> run-to-run swings).
//! - `-align-all-functions=6` pins 64-byte function entries, so an unrelated edit relaying the binary
//!   does not shift throughput by code placement alone. The placement-invariant signal is
//!   instructions/byte (`perf stat`), which criterion does not report -- read it alongside the time.
//! - `performance` governor, turbo off, one run at a time, a quiesced box. See the bench-placement notes.
//! - UNITS when comparing to the C++ arena bench: criterion prints BINARY MiB/s (1024-based); the C++
//!   harness prints DECIMAL GB/s (1000-based). The input is byte-identical (that is the point), so
//!   convert before comparing throughputs -- or compare the placement-invariant instructions/byte.
//!
//! This is the "cold" shape: a fresh `Arena` per decode (the realistic path -- you decode into a new
//! arena). A warm (reset-and-reuse) arm and packed/scalar-dispatch arms can be added later.
#![allow(clippy::all)]

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};
use rapidproto::Arena;
use rustgen_golden::bench::arena::bench::Dataset;

// --- wire encoders (the decoder is the system under test, not these) ---
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
fn put_var_field(b: &mut Vec<u8>, field: u32, v: u64) {
    put_tag(b, field, 0);
    put_varint(b, v);
}
fn put_len(b: &mut Vec<u8>, field: u32, payload: &[u8]) {
    put_tag(b, field, 2);
    put_varint(b, payload.len() as u64);
    b.extend_from_slice(payload);
}
fn put_str(b: &mut Vec<u8>, field: u32, s: &str) {
    put_len(b, field, s.as_bytes());
}
fn put_fixed64(b: &mut Vec<u8>, field: u32, v: u64) {
    put_tag(b, field, 1);
    b.extend_from_slice(&v.to_le_bytes());
}
fn put_double(b: &mut Vec<u8>, field: u32, v: f64) {
    put_fixed64(b, field, v.to_bits());
}

// bench.Attribute { key = 1, value = 2 }
fn attribute(key: &str, value: &str) -> Vec<u8> {
    let mut a = Vec::new();
    put_str(&mut a, 1, key);
    put_str(&mut a, 2, value);
    a
}

// bench.Address { street = 1, city = 2, zip = 3 }
fn address(i: u64) -> Vec<u8> {
    let mut a = Vec::new();
    put_str(&mut a, 1, &format!("{} Main St", 100 + i));
    put_str(&mut a, 2, "Springfield");
    put_var_field(&mut a, 3, 90000 + i);
    a
}

// One map<string,int32> entry as a length-delimited sub-message (key field 1, value field 2).
fn counter_entry(key: &str, value: u64) -> Vec<u8> {
    let mut e = Vec::new();
    put_str(&mut e, 1, key);
    put_var_field(&mut e, 2, value);
    e
}

// bench.Person: a realistic mix of every field kind.
fn person(i: u64) -> Vec<u8> {
    let mut p = Vec::new();
    put_var_field(&mut p, 1, i); // id (int64)
    put_str(&mut p, 2, &format!("Person {i}")); // name
    put_str(&mut p, 3, &format!("person{i}@example.com")); // email
    put_var_field(&mut p, 4, u64::from(i % 2 == 0)); // active (bool)
    put_double(&mut p, 5, (i as f64) * 1.5); // score
    put_fixed64(&mut p, 6, 1_700_000_000 + i); // created (fixed64)
    put_len(&mut p, 7, &address(i)); // address (nested message)
    for t in 0..3u64 {
        put_str(&mut p, 8, &format!("tag{}", i * 3 + t)); // tags (repeated string)
    }
    let mut history = Vec::new(); // history (repeated int32, packed)
    for h in 0..5u64 {
        put_varint(&mut history, i.wrapping_mul(7).wrapping_add(h));
    }
    put_len(&mut p, 9, &history);
    for a in 0..2u64 {
        put_len(
            &mut p,
            10,
            &attribute(&format!("attr{a}"), &format!("v{}", i + a)),
        ); // repeated sub-msg
    }
    put_len(&mut p, 11, &counter_entry("hits", i)); // counters (map<string,int32>)
    put_len(&mut p, 11, &counter_entry("misses", i / 2));
    p
}

// bench.Dataset { name = 1, version = 2, repeated Person people = 3 }
fn dataset(people: u64) -> Vec<u8> {
    let mut d = Vec::new();
    put_str(&mut d, 1, "benchmark dataset");
    put_var_field(&mut d, 2, 7);
    for i in 0..people {
        put_len(&mut d, 3, &person(i));
    }
    d
}

// Touch every decoded field so the optimizer cannot elide the decode.
fn checksum(ds: &Dataset) -> u64 {
    let mut sum = ds.name().as_bytes().len() as u64 ^ (ds.version() as u64);
    for p in ds.people() {
        sum = sum
            .wrapping_add(p.id() as u64)
            .wrapping_add(p.name().as_bytes().len() as u64)
            .wrapping_add(p.email().as_bytes().len() as u64)
            .wrapping_add(u64::from(p.active()))
            .wrapping_add(p.score().to_bits())
            .wrapping_add(p.created())
            .wrapping_add(p.tags().len() as u64)
            .wrapping_add(p.attributes().len() as u64)
            .wrapping_add(p.counters().len() as u64);
        if let Some(a) = p.address() {
            sum = sum.wrapping_add(u64::from(a.zip()));
        }
        for &h in p.history() {
            sum = sum.wrapping_add(h as u64);
        }
    }
    sum
}

// The payload: the SHARED protoc-canonical fixture when `RAPIDPROTO_BENCH_DATASET` points at it (so
// this bench and the C++ arena bench decode byte-identical input, cross-language comparable), else the
// built-in hand-built Dataset for a standalone Rust-vs-Rust A/B. Build the shared fixture with the C++
// bench's generator mode:
//   RAPIDPROTO_BENCH_DUMP_DATASET=/tmp/dataset.bin ./build/<cfg>/rapidproto_arena_bench
// then run both benches with RAPIDPROTO_BENCH_DATASET=/tmp/dataset.bin.
fn load_payload(people: u64) -> Vec<u8> {
    match std::env::var_os("RAPIDPROTO_BENCH_DATASET") {
        Some(path) => {
            let bytes = std::fs::read(&path)
                .unwrap_or_else(|e| panic!("cannot read RAPIDPROTO_BENCH_DATASET {path:?}: {e}"));
            // A 0-byte file would decode as an empty Dataset and silently "measure" nothing; fail loud.
            assert!(
                !bytes.is_empty(),
                "RAPIDPROTO_BENCH_DATASET {path:?} is empty"
            );
            bytes
        }
        None => dataset(people),
    }
}

fn decode_bench(c: &mut Criterion) {
    const PEOPLE: u64 = 2_000; // matches the C++ arena bench's record count
                               // Read the WHOLE payload into memory once here, before the timed loop: `std::fs::read` returns an
                               // owned `Vec` with every byte resident, so the measured region is pure decode -- no file I/O, no
                               // page faults on first touch -- and it is reused across all iterations.
    let payload = load_payload(PEOPLE);

    let mut group = c.benchmark_group("decode");
    group.throughput(Throughput::Bytes(payload.len() as u64));
    group.bench_function("dataset_cold", |b| {
        b.iter(|| {
            let arena = Arena::new();
            let ds = Dataset::decode(black_box(payload.as_slice()), &arena).unwrap();
            black_box(checksum(ds))
        });
    });
    group.finish();
}

criterion_group!(benches, decode_bench);
criterion_main!(benches);
