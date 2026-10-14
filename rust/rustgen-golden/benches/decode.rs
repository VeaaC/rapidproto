//! Decode benchmark for the generated Rust arena decoder, over the SHARED benchmark schema
//! `tests/bench/bench.proto` (package `bench`) that the C++ arena bench also uses. Each arm decodes a
//! different message shape into a fresh arena and checksums the tree, so a decode optimization can be
//! A/B'd against the shape it targets -- the same scenario set the C++ `bench_arena` runs:
//!
//! - `dataset`    -- realistic mixed records (scalars, strings, nested msg, repeated, map).
//! - `big_varint` -- packed int64 runs (the packed-varint path).
//! - `big_fixed`  -- packed double runs (the packed fixed-width path).
//! - `big_osm`    -- packed sint64 deltas + a packed enum (narrow-mixed packed, OSM/vector-tile shape).
//! - `big`        -- few messages, large packed arrays.
//! - `wide`       -- many messages, each with tiny repeated arrays (repeated-field churn).
//! - `records`    -- scalar-only records, 20 fields incl. 2-byte-tag nested sub-messages (dispatch-bound).
//! - `particles`  -- small records with always-present fixed-size value sub-messages.
//!
//! Input: set `RAPIDPROTO_BENCH_DIR` to a directory of shared protoc-canonical `.bin` files (produced
//! by the C++ arena bench's generator mode -- see `load_scenario` below) and BOTH this bench and the
//! C++ bench decode byte-identical bytes, so their numbers are cross-language comparable. Unset, each
//! arm falls back to a built-in hand-built payload of the same shape -- fine for Rust-vs-Rust A/B, but
//! NOT comparable to C++ (the hand-built bytes aren't protoc-canonical: they emit implicit-zero fields
//! protoc omits). Generate the shared directory once with the C++ bench:
//!
//! ```text
//! RAPIDPROTO_BENCH_DUMP_DIR=/tmp/bench ./build/<cfg>/rapidproto_arena_bench
//! RAPIDPROTO_BENCH_DIR=/tmp/bench RUSTFLAGS="-Cllvm-args=-align-all-functions=6" \
//!     taskset -c 0 cargo bench -p rustgen-golden --bench decode
//! ```
//!
//! RUN IT PINNED AND ALIGNED, or the numbers lie (same discipline as the wire bench):
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
//! arena). A warm (reset-and-reuse) arm can be added later.
#![allow(clippy::all)]

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};
use rapidproto::Arena;
use rustgen_golden::bench::arena::bench::{BigSet, Dataset, ParticleSet, RecordSet, WideSet};

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
fn put_fixed32(b: &mut Vec<u8>, field: u32, v: u32) {
    put_tag(b, field, 5);
    b.extend_from_slice(&v.to_le_bytes());
}
fn put_fixed64(b: &mut Vec<u8>, field: u32, v: u64) {
    put_tag(b, field, 1);
    b.extend_from_slice(&v.to_le_bytes());
}
fn put_double(b: &mut Vec<u8>, field: u32, v: f64) {
    put_fixed64(b, field, v.to_bits());
}
// A packed repeated run: the elements concatenated in a single length-delimited field.
fn put_packed_varints(b: &mut Vec<u8>, field: u32, vals: &[u64]) {
    let mut inner = Vec::new();
    for &v in vals {
        put_varint(&mut inner, v);
    }
    put_len(b, field, &inner);
}
fn put_packed_fixed64(b: &mut Vec<u8>, field: u32, vals: &[u64]) {
    let mut inner = Vec::new();
    for &v in vals {
        inner.extend_from_slice(&v.to_le_bytes());
    }
    put_len(b, field, &inner);
}
fn zigzag64(n: i64) -> u64 {
    ((n << 1) ^ (n >> 63)) as u64
}
fn zigzag32(n: i32) -> u64 {
    u64::from(((n << 1) ^ (n >> 31)) as u32)
}

// ── Dataset: the realistic mixed shape ───────────────────────────────────────────────────────────

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
    let history: Vec<u64> = (0..5u64)
        .map(|h| i.wrapping_mul(7).wrapping_add(h))
        .collect();
    put_packed_varints(&mut p, 9, &history); // history (repeated int32, packed)
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

// ── Wide: many messages, each with tiny repeated arrays ──────────────────────────────────────────

// bench.Wide { repeated int32 a=1,b=2,c=3,d=4 (packed); repeated string s=5,t=6 }
fn wide_item(i: u64) -> Vec<u8> {
    let mut w = Vec::new();
    put_packed_varints(&mut w, 1, &[i, i + 1, i + 2]);
    put_packed_varints(&mut w, 2, &[i * 2, i * 2 + 1, i * 2 + 2]);
    put_packed_varints(&mut w, 3, &[i * 3, i * 3 + 1, i * 3 + 2]);
    put_packed_varints(&mut w, 4, &[i * 4, i * 4 + 1, i * 4 + 2]);
    for k in 0..2u64 {
        put_str(&mut w, 5, &format!("s{}", (i + k) % 32));
    }
    for k in 0..2u64 {
        put_str(&mut w, 6, &format!("t{}", (i + k) % 32));
    }
    w
}

// bench.WideSet { repeated Wide items = 1 }
fn wideset(items: u64) -> Vec<u8> {
    let mut d = Vec::new();
    for i in 0..items {
        put_len(&mut d, 1, &wide_item(i));
    }
    d
}

// ── Big: packed numeric runs (varint / fixed / zigzag+enum) ──────────────────────────────────────

// bench.Big items wrapped into a BigSet { repeated Big items = 1 }; `build` fills one Big message.
fn bigset(items: u64, build: impl Fn(u64) -> Vec<u8>) -> Vec<u8> {
    let mut d = Vec::new();
    for i in 0..items {
        put_len(&mut d, 1, &build(i));
    }
    d
}

// bench.Big { repeated int64 numbers = 1 } (packed varint).
fn big_varint_item(i: u64, per: u64) -> Vec<u8> {
    let nums: Vec<u64> = (0..per).map(|k| i * per + k).collect();
    let mut b = Vec::new();
    put_packed_varints(&mut b, 1, &nums);
    b
}

// bench.Big { repeated double reals = 2 } (packed fixed-width).
fn big_fixed_item(per: u64) -> Vec<u8> {
    let reals: Vec<u64> = (0..per).map(|k| ((k as f64) * 1.5).to_bits()).collect();
    let mut b = Vec::new();
    put_packed_fixed64(&mut b, 2, &reals);
    b
}

// bench.Big { numbers=1 + reals=2 } (both packed arrays).
fn big_both_item(i: u64, per: u64) -> Vec<u8> {
    let nums: Vec<u64> = (0..per).map(|k| i * per + k).collect();
    let reals: Vec<u64> = (0..per).map(|k| ((k as f64) * 1.5).to_bits()).collect();
    let mut b = Vec::new();
    put_packed_varints(&mut b, 1, &nums);
    put_packed_fixed64(&mut b, 2, &reals);
    b
}

// bench.BigSet of OSM-shaped Big { repeated sint64 zz = 3 (packed, zigzag deltas) + repeated Kind
// kinds = 4 (packed enum) }. Deterministic xorshift so the bytes are reproducible, mirroring the C++
// make_big_osm: mostly small deltas (1-2 byte zigzag), 1-in-16 a larger jump (~3-byte), plus a narrow
// enum run -- the narrow-mixed regime the packed kernels target.
fn big_osm(items: u64, per: u64) -> Vec<u8> {
    let mut rng: u64 = 0x9E37_79B9_7F4A_7C15;
    let mut next = || {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        rng
    };
    let mut set = Vec::new();
    for _ in 0..items {
        let mut zz: Vec<u64> = Vec::with_capacity(per as usize);
        let mut kinds: Vec<u64> = Vec::with_capacity(per as usize);
        for _ in 0..per {
            let delta: i64 = if next() % 16 == 0 {
                (next() % 200_000) as i64 - 100_000
            } else {
                (next() % 256) as i64 - 128
            };
            zz.push(zigzag64(delta));
            kinds.push(u64::from(next() & 1 != 0));
        }
        let mut b = Vec::new();
        put_packed_varints(&mut b, 3, &zz);
        put_packed_varints(&mut b, 4, &kinds);
        put_len(&mut set, 1, &b);
    }
    set
}

// ── Record: scalar-only, dispatch-bound (20 fields incl. 2-byte-tag nested sub-messages) ──────────

// bench.Sample { int32 a=1, b=2; int64 c=3; sint32 d=4; bool e=5; uint32 f=6 }.
fn sample(base: i64) -> Vec<u8> {
    let mut s = Vec::new();
    put_var_field(&mut s, 1, (base & 63) as u64);
    put_var_field(&mut s, 2, ((base + 1) & 63) as u64);
    put_var_field(&mut s, 3, ((base + 2) & 63) as u64);
    put_var_field(&mut s, 4, zigzag32(((base + 3) & 63) as i32));
    put_var_field(&mut s, 5, u64::from(base & 1 != 0));
    put_var_field(&mut s, 6, ((base + 4) & 63) as u64);
    s
}

// bench.Record: f1..f15 scalars, then Sample s1=16/s2=17 (2-byte tags), then f18..f20.
fn record(i: i64) -> Vec<u8> {
    let mut r = Vec::new();
    put_var_field(&mut r, 1, (i & 63) as u64);
    put_var_field(&mut r, 2, ((i + 1) & 63) as u64);
    put_var_field(&mut r, 3, ((i + 2) & 63) as u64);
    put_var_field(&mut r, 4, ((i + 3) & 63) as u64); // int64
    put_var_field(&mut r, 5, ((i + 4) & 63) as u64); // uint32
    put_var_field(&mut r, 6, zigzag32(((i + 5) & 63) as i32)); // sint32
    put_var_field(&mut r, 7, u64::from(i & 1 != 0)); // bool
    put_var_field(&mut r, 8, ((i + 6) & 63) as u64); // uint64
    put_var_field(&mut r, 9, ((i + 7) & 63) as u64);
    put_var_field(&mut r, 10, ((i + 8) & 63) as u64);
    put_var_field(&mut r, 11, ((i + 9) & 63) as u64);
    put_var_field(&mut r, 12, ((i + 10) & 63) as u64);
    put_var_field(&mut r, 13, ((i + 11) & 63) as u64);
    put_var_field(&mut r, 14, ((i + 12) & 63) as u64);
    put_var_field(&mut r, 15, ((i + 13) & 63) as u64);
    put_len(&mut r, 16, &sample(i + 20));
    put_len(&mut r, 17, &sample(i + 30));
    put_var_field(&mut r, 18, ((i + 14) & 63) as u64);
    put_var_field(&mut r, 19, ((i + 15) & 63) as u64);
    put_var_field(&mut r, 20, ((i + 16) & 63) as u64);
    r
}

// bench.RecordSet { repeated Record records = 1 }
fn recordset(count: i64) -> Vec<u8> {
    let mut d = Vec::new();
    for i in 0..count {
        put_len(&mut d, 1, &record(i));
    }
    d
}

// ── Particle: small records with always-present fixed-size value sub-messages ─────────────────────

// bench.Vec3 { double x=1, y=2, z=3 }
fn vec3(x: f64, y: f64, z: f64) -> Vec<u8> {
    let mut v = Vec::new();
    put_double(&mut v, 1, x);
    put_double(&mut v, 2, y);
    put_double(&mut v, 3, z);
    v
}

// bench.Particle { int64 id=1; Vec3 position=2, velocity=3; fixed32 color=4 }
fn particle(i: u64) -> Vec<u8> {
    let f = i as f64;
    let mut p = Vec::new();
    put_var_field(&mut p, 1, i);
    put_len(&mut p, 2, &vec3(f, f * 2.0, f * 3.0));
    put_len(&mut p, 3, &vec3(f * 0.1, f * 0.2, f * 0.3));
    put_fixed32(&mut p, 4, i as u32);
    p
}

// bench.ParticleSet { repeated Particle items = 1 }
fn particleset(items: u64) -> Vec<u8> {
    let mut d = Vec::new();
    for i in 0..items {
        put_len(&mut d, 1, &particle(i));
    }
    d
}

// ── checksums: touch every decoded field so the optimizer cannot elide the decode ─────────────────

fn checksum_dataset(ds: &Dataset) -> u64 {
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

fn checksum_wideset(ws: &WideSet) -> u64 {
    let mut sum = 0u64;
    for it in ws.items() {
        for &v in it.a() {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in it.b() {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in it.c() {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in it.d() {
            sum = sum.wrapping_add(v as u64);
        }
        for x in it.s() {
            sum = sum.wrapping_add(x.len() as u64);
        }
        for x in it.t() {
            sum = sum.wrapping_add(x.len() as u64);
        }
    }
    sum
}

fn checksum_bigset(bs: &BigSet) -> u64 {
    let mut sum = 0u64;
    for it in bs.items() {
        for &n in it.numbers() {
            sum = sum.wrapping_add(n as u64);
        }
        for &r in it.reals() {
            sum = sum.wrapping_add(r.to_bits());
        }
        for &z in it.zz() {
            sum = sum.wrapping_add(z as u64);
        }
        for &k in it.kinds() {
            sum = sum.wrapping_add(k.0 as u64);
        }
    }
    sum
}

fn checksum_recordset(rs: &RecordSet) -> u64 {
    let mut sum = 0u64;
    for r in rs.records() {
        sum = sum
            .wrapping_add(r.f1() as u64)
            .wrapping_add(r.f2() as u64)
            .wrapping_add(r.f3() as u64)
            .wrapping_add(r.f4() as u64)
            .wrapping_add(u64::from(r.f5()))
            .wrapping_add(r.f6() as u64)
            .wrapping_add(u64::from(r.f7()))
            .wrapping_add(r.f8())
            .wrapping_add(r.f9() as u64)
            .wrapping_add(r.f10() as u64)
            .wrapping_add(r.f11() as u64)
            .wrapping_add(r.f12() as u64)
            .wrapping_add(r.f13() as u64)
            .wrapping_add(r.f14() as u64)
            .wrapping_add(r.f15() as u64)
            .wrapping_add(r.f18() as u64)
            .wrapping_add(r.f19() as u64)
            .wrapping_add(r.f20() as u64);
        for s in [r.s1(), r.s2()].into_iter().flatten() {
            sum = sum
                .wrapping_add(s.a() as u64)
                .wrapping_add(s.b() as u64)
                .wrapping_add(s.c() as u64)
                .wrapping_add(s.d() as u64)
                .wrapping_add(u64::from(s.e()))
                .wrapping_add(u64::from(s.f()));
        }
    }
    sum
}

fn checksum_particleset(ps: &ParticleSet) -> u64 {
    let mut sum = 0u64;
    for p in ps.items() {
        sum = sum
            .wrapping_add(p.id() as u64)
            .wrapping_add(u64::from(p.color()));
        if let Some(v) = p.position() {
            sum = sum
                .wrapping_add(v.x().to_bits())
                .wrapping_add(v.y().to_bits())
                .wrapping_add(v.z().to_bits());
        }
        if let Some(v) = p.velocity() {
            sum = sum
                .wrapping_add(v.x().to_bits())
                .wrapping_add(v.y().to_bits())
                .wrapping_add(v.z().to_bits());
        }
    }
    sum
}

// The payload for `name`: the shared protoc-canonical file `<RAPIDPROTO_BENCH_DIR>/<name>.bin` when the
// directory is set (so this bench and the C++ arena bench decode byte-identical input), else the
// built-in hand-built payload for a standalone Rust-vs-Rust A/B. Build the shared directory with the
// C++ bench's generator mode: `RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_arena_bench`.
fn load_scenario(name: &str, fallback: impl FnOnce() -> Vec<u8>) -> Vec<u8> {
    match std::env::var_os("RAPIDPROTO_BENCH_DIR") {
        Some(dir) => {
            let path = std::path::Path::new(&dir).join(format!("{name}.bin"));
            let bytes = std::fs::read(&path)
                .unwrap_or_else(|e| panic!("cannot read RAPIDPROTO_BENCH_DIR file {path:?}: {e}"));
            // A 0-byte file would decode as an empty message and silently "measure" nothing; fail loud.
            assert!(!bytes.is_empty(), "scenario file {path:?} is empty");
            bytes
        }
        None => fallback(),
    }
}

// Read the WHOLE payload into memory once, before the timed loop: `std::fs::read` / the builder return
// an owned `Vec` with every byte resident, so the measured region is pure decode -- no file I/O, no
// first-touch page faults -- reused across all iterations.
macro_rules! scenario {
    ($group:ident, $name:literal, $payload:expr, $ty:ty, $ck:path) => {{
        let payload: Vec<u8> = $payload;
        $group.throughput(Throughput::Bytes(payload.len() as u64));
        $group.bench_function($name, |b| {
            b.iter(|| {
                let arena = Arena::new();
                let m = <$ty>::decode(black_box(payload.as_slice()), &arena).unwrap();
                black_box($ck(m))
            });
        });
    }};
}

fn decode_bench(c: &mut Criterion) {
    let mut group = c.benchmark_group("decode");
    // Record counts match the C++ arena bench's arms, so the shared files (and the fallbacks) are the
    // same shape and size on both sides.
    scenario!(
        group,
        "dataset",
        load_scenario("dataset", || dataset(2000)),
        Dataset,
        checksum_dataset
    );
    scenario!(
        group,
        "big_varint",
        load_scenario("big_varint", || bigset(200, |i| big_varint_item(i, 1000))),
        BigSet,
        checksum_bigset
    );
    scenario!(
        group,
        "big_fixed",
        load_scenario("big_fixed", || bigset(200, |_| big_fixed_item(1000))),
        BigSet,
        checksum_bigset
    );
    scenario!(
        group,
        "big_osm",
        load_scenario("big_osm", || big_osm(200, 1000)),
        BigSet,
        checksum_bigset
    );
    scenario!(
        group,
        "big",
        load_scenario("big", || bigset(30, |i| big_both_item(i, 10000))),
        BigSet,
        checksum_bigset
    );
    scenario!(
        group,
        "wide",
        load_scenario("wide", || wideset(50000)),
        WideSet,
        checksum_wideset
    );
    scenario!(
        group,
        "records",
        load_scenario("records", || recordset(20000)),
        RecordSet,
        checksum_recordset
    );
    scenario!(
        group,
        "particles",
        load_scenario("particles", || particleset(50000)),
        ParticleSet,
        checksum_particleset
    );
    group.finish();
}

criterion_group!(benches, decode_bench);
criterion_main!(benches);
