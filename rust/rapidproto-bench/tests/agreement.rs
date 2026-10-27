//! The runners' cross-arm agreement gate, without the shared payload directory (so it runs in CI):
//! every supported arm of a scenario must produce the same checksum on the same bytes. Arena arms
//! decode messages prost encodes (prost being the reference encoder); the OSM arms decode the in-repo
//! OSM fixture; streaming arms decode small hand-built inputs of each scenario's wire shape. Generic
//! over each scenario's arm list, so a newly registered arm is covered with no extra test.

use prost::Message;
use rapidproto_bench::competitors::prost::pb;
use rapidproto_bench::payloads::{self, Source};
use rapidproto_bench::Arm;

// Every supported arm's checksum on `bytes`, all equal (and returned), the unsupported ones skipped.
fn all_agree(mut arms: Vec<Arm>, bytes: &[u8]) -> u64 {
    let sums: Vec<(&str, u64)> = arms
        .iter_mut()
        .filter_map(|a| {
            let name = a.name;
            a.decoder().map(|d| (name, d.decode(bytes)))
        })
        .collect();
    assert!(
        sums.iter().all(|&(_, s)| s == sums[0].1),
        "arm checksums disagree: {sums:?}"
    );
    sums[0].1
}

// The three message types below exercise every field kind the schema has (scalars/strings/nested/
// repeated scalar+string+message/map in Dataset; dispatch + nested in RecordSet; packed varint/fixed/
// zigzag + enum + double in Big).

#[test]
fn dataset_arms_agree() {
    let ds = pb::Dataset {
        name: "acme".into(),
        version: 7,
        people: vec![
            pb::Person {
                id: 42,
                name: "ada".into(),
                email: "ada@x.io".into(),
                active: true,
                score: 3.5,
                created: 1234567890,
                address: Some(pb::Address {
                    street: "1 st".into(),
                    city: "ny".into(),
                    zip: 10001,
                }),
                tags: vec!["a".into(), "bb".into()],
                history: vec![1, -2, 3],
                attributes: vec![pb::Attribute {
                    key: "k".into(),
                    value: "v".into(),
                }],
                counters: [("x".to_string(), 1), ("y".to_string(), -3)]
                    .into_iter()
                    .collect(),
            },
            pb::Person::default(),
        ],
    };
    // The arena (cold + warm) and streaming arms are all in the list.
    let arms = rapidproto_bench::dataset_arms();
    assert!(arms.iter().any(|a| a.name == "rapidproto-stream"));
    all_agree(arms, &ds.encode_to_vec());
}

#[test]
fn recordset_arms_agree() {
    let rs = pb::RecordSet {
        records: vec![
            pb::Record {
                f1: 1,
                f2: -2,
                f3: 3,
                f4: -4,
                f5: 5,
                f6: -6,
                f7: true,
                f8: 8,
                f9: 9,
                f10: -10,
                f11: 11,
                f12: 12,
                f13: 13,
                f14: 14,
                f15: 15,
                s1: Some(pb::Sample {
                    a: 1,
                    b: 2,
                    c: 3,
                    d: -4,
                    e: true,
                    f: 6,
                }),
                s2: None,
                f18: 18,
                f19: 19,
                f20: 20,
            },
            pb::Record::default(),
        ],
    };
    let bytes = rs.encode_to_vec();
    all_agree(rapidproto_bench::recordset_arms(), &bytes);
    // The drop profile's decoder reads the three kept fields and skips the rest, s1 included. A
    // negative int32 sign-extends to u64, as the C++ arms' `static_cast<std::uint64_t>` does.
    assert_eq!(
        all_agree(rapidproto_bench::recordset_sparse_arms(), &bytes),
        1u64.wrapping_add(-10i64 as u64).wrapping_add(20)
    );
}

// Only DEFINED Kind values: pb-rs (quick-protobuf) generates closed enums whose `From<i32>` maps any
// unknown value to the default, so it cannot round-trip an out-of-range open-enum value the way the
// other arms do (the `rv-enum` sweep marks it unsupported for exactly that, see below).
#[cfg(not(debug_assertions))]
fn big() -> pb::Big {
    pb::Big {
        numbers: vec![1, -2, 3_000_000_000],
        reals: vec![1.5, -2.25, 0.0],
        zz: vec![-1, 2, -3],
        kinds: vec![pb::Kind::One as i32, 0, pb::Kind::One as i32],
    }
}

// Release-only. This message's `reals` is `repeated double`, and quick-protobuf decodes it by
// borrowing the input as an unaligned `&[f64]` -- UB that aborts under the debug `assert_unsafe_
// precondition` alignment check (see `has_unaligned_fixed_slice_ub` and the quick_protobuf module).
// A release build compiles that check out; the per-field checksums still agree on x86. CI runs it
// via a dedicated `cargo test -p rapidproto-bench --release` step.
#[cfg(not(debug_assertions))]
#[test]
fn bigset_arms_agree() {
    let bs = pb::BigSet { items: vec![big()] };
    all_agree(rapidproto_bench::bigset_arms(), &bs.encode_to_vec());
}

// A varint-sweep payload: one `Big` carrying a single packed varint field (no `reals`, so no UB).
#[test]
fn big_arms_agree() {
    let numbers = pb::Big {
        numbers: vec![0, 127, 128, 1 << 40, -1],
        ..pb::Big::default()
    };
    let zz = pb::Big {
        zz: vec![0, -1, 1 << 40, i64::MIN],
        ..pb::Big::default()
    };
    for b in [numbers, zz, big_kinds_defined()] {
        all_agree(rapidproto_bench::big_arms(), &b.encode_to_vec());
    }
}

fn big_kinds_defined() -> pb::Big {
    pb::Big {
        kinds: vec![pb::Kind::One as i32, 0],
        ..pb::Big::default()
    }
}

#[test]
fn big_enum_arms_agree_with_quick_protobuf_unsupported() {
    let arms = rapidproto_bench::big_enum_arms();
    assert!(arms
        .iter()
        .any(|a| a.name == "quick-protobuf" && a.unsupported_reason().is_some()));
    // An enum value no `Kind` names, like the sweep's varints.
    let b = pb::Big {
        kinds: vec![1, 300, 2_000_000],
        ..pb::Big::default()
    };
    all_agree(arms, &b.encode_to_vec());
}

// The OSM arms over the in-repo fixture (a zlib and a raw blob; dense nodes with DenseInfo, a plain
// node, a way, a relation): the raw-file arms agree, the block arms agree over its blocks, and the two
// scenarios sum the same -- the C++ arm's block-vs-file cross-check.
#[test]
fn osm_arms_agree() {
    use rapidproto_bench::external::osm;
    let file = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../examples/osm-pbf/testdata/mini.osm.pbf"
    ))
    .expect("read the OSM fixture");
    let file_sum = all_agree(osm::file_arms(), &file);
    let mut blocks = Vec::new();
    osm::file_sum(&file, &mut Vec::new(), |p| {
        blocks.push(p.to_vec());
        0
    });
    assert!(blocks.len() > 1, "the fixture has a zlib and a raw block");
    let blocks_sum = all_agree(osm::blocks_arms(), &payloads::batch(&blocks));
    assert_eq!(file_sum, blocks_sum);
    assert_ne!(file_sum, 0);
}

// --- streaming -----------------------------------------------------------------------------------
// A small input in each streaming scenario's wire shape (the fields of its C++ builder in
// tests/bench_streamgen.cpp, with values chosen so every signed field goes negative and every uint32
// one passes 2^31), together with the sum the C++ bench's `generated` lambda computes over it, derived
// from the ENCODED VALUES (never by parsing), so a cast both Rust arms get wrong the same way still
// fails against the C++ rule.

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
// A varint field; a negative int32/int64 encodes sign-extended to 64 bits, as protobuf does.
fn put_var(b: &mut Vec<u8>, field: u32, v: u64) {
    put_tag(b, field, 0);
    put_varint(b, v);
}
fn put_len(b: &mut Vec<u8>, field: u32, payload: &[u8]) {
    put_tag(b, field, 2);
    put_varint(b, payload.len() as u64);
    b.extend_from_slice(payload);
}
fn zigzag(v: i64) -> u64 {
    ((v << 1) ^ (v >> 63)) as u64
}

const N: i64 = 40;

// The C++ widenings: `static_cast<std::uint32_t>(int32)` zero-extends, `static_cast<std::uint64_t>`
// of a signed value sign-extends.
fn u32_of(v: i32) -> u64 {
    u64::from(v as u32)
}
fn u64_of(v: i64) -> u64 {
    v as u64
}

// A Sample whose int32 `a`/`b` and sint32 `d` go negative for some bases and whose uint32 `f` is past
// 2^31 (sign-extending it would change the sum); returns its bytes and the C++ dense sum over it.
fn sample(base: i64) -> (Vec<u8>, u64) {
    let (a, b) = ((base & 63) - 32, ((base + 1) & 63) - 40);
    let (c, d) = ((base + 2) & 63, (base & 7) - 4);
    let (e, f) = (base & 1 == 1, 0x8000_0000 + ((base + 4) & 63));
    let mut s = Vec::new();
    put_var(&mut s, 1, a as u64);
    put_var(&mut s, 2, b as u64);
    put_var(&mut s, 3, c as u64);
    put_var(&mut s, 4, zigzag(d));
    put_var(&mut s, 5, u64::from(e));
    put_var(&mut s, 6, f as u64);
    let sum = [
        u64_of(a),
        u64_of(b),
        u64_of(c),
        u64_of(d),
        u64::from(e),
        f as u64,
    ]
    .into_iter()
    .fold(0u64, u64::wrapping_add);
    (s, sum)
}

// A RecordSet with negative int32 / sint32 fields and a uint32 `f5` past 2^31; returns its bytes and
// the C++ dense and sparse sums.
fn record_set() -> (Vec<u8>, u64, u64) {
    let (mut out, mut dense, mut sparse) = (Vec::new(), 0u64, 0u64);
    for i in 0..N {
        let mut r = Vec::new();
        for field in 1..=15u32 {
            let v = i - 20 + i64::from(field); // negative for the early records
            let unsigned = match field {
                5 => 0x8000_0000 | (v & 0xffff) as u64, // f5 uint32
                8 => (v & 0xffff) as u64,               // f8 uint64
                _ => 0,
            };
            match field {
                4 => put_var(&mut r, 4, v as u64), // f4 int64
                5 | 8 => put_var(&mut r, field, unsigned),
                6 => put_var(&mut r, 6, zigzag(v)), // f6 sint32
                7 => put_var(&mut r, 7, (v & 1) as u64), // f7 bool
                _ => put_var(&mut r, field, v as u64), // int32, sign-extended
            }
            dense = dense.wrapping_add(match field {
                5 | 8 => unsigned,
                7 => (v & 1) as u64,
                _ => u64_of(v),
            });
            if matches!(field, 1 | 10) {
                sparse = sparse.wrapping_add(u64_of(v));
            }
        }
        for (field, base) in [(16, i + 20), (17, i + 30)] {
            let (bytes, sum) = sample(base);
            put_len(&mut r, field, &bytes);
            dense = dense.wrapping_add(sum);
        }
        for field in 18..=20u32 {
            let v = i - 30 + i64::from(field);
            put_var(&mut r, field, v as u64);
            dense = dense.wrapping_add(u64_of(v));
            if field == 20 {
                sparse = sparse.wrapping_add(u64_of(v));
            }
        }
        put_len(&mut out, 1, &r);
    }
    (out, dense, sparse)
}

// One packed int64 field 1 carrying a value of every varint width, 1..10 bytes (10: a negative int64).
fn pool_input() -> (Vec<u8>, u64) {
    let values: Vec<i64> = (1..=9u32)
        .map(|w| 1i64 << (7 * (w - 1)))
        .chain([-5])
        .collect();
    let mut run = Vec::new();
    for &v in &values {
        put_varint(&mut run, v as u64);
    }
    let mut b = Vec::new();
    put_len(&mut b, 1, &run);
    (
        b,
        values.iter().fold(0u64, |s, &v| s.wrapping_add(u64_of(v))),
    )
}

// The input of the scenario `name` (payload `stem`) and the C++ generated arm's sum over it.
fn stream_case(name: &str, stem: &str) -> (Vec<u8>, u64) {
    let (mut b, mut sum) = (Vec::new(), 0u64);
    let mut add = |v: u64| sum = sum.wrapping_add(v);
    match stem {
        "varint-1byte" | "skip-heavy" | "sparse-skip" | "varint-multibyte" => {
            for i in 0..N {
                let v = match stem {
                    "varint-multibyte" => (0xF000_0000 + i) as i32, // > i32::MAX: wraps negative
                    _ => (i - 20) as i32,
                };
                put_var(&mut b, 1, i64::from(v) as u64);
                add(u32_of(v));
                match stem {
                    "skip-heavy" => put_var(&mut b, 11, (i & 1) as u64),
                    "sparse-skip" => {
                        put_var(&mut b, 2, (i * 3) as u64);
                        put_tag(&mut b, 8, 1);
                        b.extend_from_slice(&(i as u64).to_le_bytes());
                        put_len(&mut b, 12, b"abcd");
                    }
                    _ => {}
                }
            }
        }
        "zigzag-sint64" => {
            for i in 0..N {
                let v = i * if i & 1 == 1 { -1003 } else { 1003 };
                put_var(&mut b, 6, zigzag(v));
                add(u64_of(v));
            }
        }
        "fixed32-float" => {
            for i in 0..N {
                let bits = (i as f32 * -1.5).to_bits();
                put_tag(&mut b, 14, 5);
                b.extend_from_slice(&bits.to_le_bytes());
                add(u64::from(bits));
            }
        }
        "fixed64-double" => {
            for i in 0..N {
                let bits = (i as f64 * -2.25).to_bits();
                put_tag(&mut b, 15, 1);
                b.extend_from_slice(&bits.to_le_bytes());
                add(bits);
            }
        }
        "len-string" => {
            for i in 0..N {
                let s = &b"abcdefgh"[..(i % 9) as usize];
                put_len(&mut b, 12, s);
                add(s.len() as u64);
            }
        }
        "mixed" => {
            for i in 0..N {
                put_var(&mut b, 1, (i - 20) as u64);
                add(u32_of((i - 20) as i32));
                put_var(&mut b, 6, zigzag(i * -7));
                add(u64_of(i * -7));
                let bits = (i as f64 * 0.5).to_bits();
                put_tag(&mut b, 15, 1);
                b.extend_from_slice(&bits.to_le_bytes());
                add(bits);
                put_len(&mut b, 12, b"wxyz");
                add(4);
            }
        }
        "multibyte-tag" => {
            for i in 0..N {
                put_var(&mut b, 18, (i - 20) as u64);
                add(u32_of((i - 20) as i32));
            }
        }
        "packed-int32" => {
            let mut run = Vec::new();
            for i in 0..N {
                put_varint(&mut run, (i - 20) as u64);
                add(u32_of((i - 20) as i32));
            }
            put_len(&mut b, 17, &run);
        }
        "nested-msg" => {
            for i in 0..N {
                let mut nested = Vec::new();
                put_var(&mut nested, 1, (i - 20) as u64);
                put_len(&mut b, 5, &nested);
                add(u32_of((i - 20) as i32));
            }
        }
        "groups" => {
            for i in 0..N {
                put_tag(&mut b, 1, 3);
                put_var(&mut b, 2, (i - 20) as u64);
                put_tag(&mut b, 1, 4);
                add(u32_of((i - 20) as i32));
            }
        }
        "records" => {
            let (bytes, dense, sparse) = record_set();
            return match name {
                "records dense (stream)" => (bytes, dense),
                "records sparse (stream)" => (bytes, sparse),
                other => panic!("no expected sum for {other:?}"),
            };
        }
        other => panic!("no input for stream payload {other:?}"),
    }
    (b, sum)
}

#[test]
fn every_stream_scenario_sums_what_cpp_sums() {
    let scenarios = rapidproto_bench::stream::scenarios();
    assert_eq!(
        scenarios.len(),
        105,
        "the C++ streaming bench's 107 scenarios, less its two two-tier ones"
    );
    for sc in scenarios {
        let (input, expected) = match &sc.source {
            Source::Stream(stem) => stream_case(&sc.name, stem),
            Source::Pool(_) => pool_input(),
            Source::Arena(_) | Source::External(_) | Source::Batch(_) => {
                unreachable!("a streaming scenario reads stream payloads")
            }
        };
        let arms = (sc.arms)();
        let names: Vec<&str> = arms.iter().map(|a| a.name).collect();
        assert_eq!(names, ["generated", "wire"], "{}", sc.name);
        assert_eq!(all_agree(arms, &input), expected, "{}", sc.name);
        assert_ne!(expected, 0, "{} sums nothing", sc.name);
    }
}

#[test]
fn every_arena_scenario_is_listed_once() {
    let specs = rapidproto_bench::scenario_specs();
    // The 8 shapes and `records sparse (dropped)`, the 5 real-world datasets (listed whether or not
    // this build decodes them), the 90-scenario `rv` sweep, and the 1M per-type sweep (15 `rv-zz`,
    // 5 `rv-enum`).
    assert_eq!(specs.len(), 8 + 1 + 5 + 90 + 15 + 5);
    let mut names: Vec<&str> = specs.iter().map(|s| s.name.as_str()).collect();
    names.sort_unstable();
    names.dedup();
    assert_eq!(names.len(), specs.len());
}
