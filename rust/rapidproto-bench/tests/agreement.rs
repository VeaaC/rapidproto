//! The runners' cross-arm agreement gate, without the shared payload directory (so it runs in CI):
//! every supported arm of a scenario must produce the same checksum on the same bytes. Arena arms
//! decode messages prost encodes (prost being the reference encoder); streaming arms decode small
//! hand-built inputs of each scenario's wire shape. Generic over each scenario's arm list, so a newly
//! registered arm is covered with no extra test.

use prost::Message;
use rapidproto_bench::competitors::prost::pb;
use rapidproto_bench::payloads::Source;
use rapidproto_bench::{Arm, ArmRun};

// Every supported arm's checksum on `bytes`, all equal (and returned), the unsupported ones skipped.
fn all_agree(arms: Vec<Arm>, bytes: &[u8]) -> u64 {
    let sums: Vec<(&str, u64)> = arms
        .iter()
        .filter_map(|a| a.decoder().map(|d| (a.name, d(bytes))))
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
                f10: 10,
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
    all_agree(rapidproto_bench::recordset_arms(), &rs.encode_to_vec());
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
        .any(|a| a.name == "quick-protobuf" && matches!(a.run, ArmRun::Unsupported(_))));
    // An enum value no `Kind` names, like the sweep's varints.
    let b = pb::Big {
        kinds: vec![1, 300, 2_000_000],
        ..pb::Big::default()
    };
    all_agree(arms, &b.encode_to_vec());
}

// --- streaming -----------------------------------------------------------------------------------
// A small input of each streaming scenario's wire shape -- the C++ builders (tests/bench_streamgen.cpp)
// at a few records -- keyed by payload stem; a sweep pool gets one packed int64 buffer of every width.

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
fn put_len(b: &mut Vec<u8>, field: u32, payload: &[u8]) {
    put_tag(b, field, 2);
    put_varint(b, payload.len() as u64);
    b.extend_from_slice(payload);
}
fn zigzag(v: i64) -> u64 {
    ((v << 1) ^ (v >> 63)) as u64
}

const N: u64 = 40;

fn sample(base: u64) -> Vec<u8> {
    let mut s = Vec::new();
    for (field, v) in [
        (1, base & 63),
        (2, (base + 1) & 63),
        (3, (base + 2) & 63),
        (4, zigzag(((base + 3) & 63) as i64 - 32)),
        (5, base & 1),
        (6, (base + 4) & 63),
    ] {
        put_tag(&mut s, field, 0);
        put_varint(&mut s, v);
    }
    s
}

fn record_set() -> Vec<u8> {
    let mut out = Vec::new();
    for i in 0..N {
        let mut r = Vec::new();
        for field in 1..=15u32 {
            put_tag(&mut r, field, 0);
            put_varint(
                &mut r,
                match field {
                    6 => zigzag(i as i64 - 20),  // f6 sint32, negative too
                    7 => i & 1,                  // f7 bool
                    1 => (i as i64 - 20) as u64, // f1 int32, negative too
                    _ => (i + u64::from(field)) & 63,
                },
            );
        }
        put_len(&mut r, 16, &sample(i + 20));
        put_len(&mut r, 17, &sample(i + 30));
        for field in 18..=20u32 {
            put_tag(&mut r, field, 0);
            put_varint(&mut r, (i + u64::from(field)) & 63);
        }
        put_len(&mut out, 1, &r);
    }
    out
}

fn stream_input(stem: &str) -> Vec<u8> {
    let mut b = Vec::new();
    for i in 0..N {
        match stem {
            "varint-1byte" => {
                put_tag(&mut b, 1, 0);
                put_varint(&mut b, i & 0x7f);
            }
            "varint-multibyte" => {
                put_tag(&mut b, 1, 0);
                put_varint(&mut b, 0xF000_0000 + i);
            }
            "zigzag-sint64" => {
                put_tag(&mut b, 6, 0);
                put_varint(
                    &mut b,
                    zigzag(i as i64 * if i & 1 == 1 { -1003 } else { 1003 }),
                );
            }
            "fixed32-float" => {
                put_tag(&mut b, 14, 5);
                b.extend_from_slice(&(i as f32 * 1.5).to_bits().to_le_bytes());
            }
            "fixed64-double" => {
                put_tag(&mut b, 15, 1);
                b.extend_from_slice(&(i as f64 * -2.25).to_bits().to_le_bytes());
            }
            "len-string" => put_len(&mut b, 12, b"abcdefgh"),
            "skip-heavy" => {
                put_tag(&mut b, 1, 0);
                put_varint(&mut b, i);
                put_tag(&mut b, 11, 0);
                put_varint(&mut b, i & 1);
            }
            "mixed" | "sparse-skip" => {
                put_tag(&mut b, 1, 0);
                put_varint(&mut b, i);
                put_tag(&mut b, if stem == "mixed" { 6 } else { 2 }, 0);
                put_varint(&mut b, zigzag(i as i64 * -7));
                put_tag(&mut b, if stem == "mixed" { 15 } else { 8 }, 1);
                b.extend_from_slice(&(i as f64 * 0.5).to_bits().to_le_bytes());
                put_len(&mut b, 12, b"wxyz");
            }
            "multibyte-tag" | "two-tier-mbtag" => {
                put_tag(&mut b, 18, 0);
                put_varint(&mut b, i & 0x7f);
            }
            "nested-msg" => {
                let mut nested = Vec::new();
                put_tag(&mut nested, 1, 0);
                put_varint(&mut nested, i);
                put_len(&mut b, 5, &nested);
            }
            "groups" => {
                put_tag(&mut b, 1, 3);
                put_tag(&mut b, 2, 0);
                put_varint(&mut b, i);
                put_tag(&mut b, 1, 4);
            }
            "packed-int32" | "two-tier" | "records" => break,
            other => panic!("no input for stream payload {other:?}"),
        }
    }
    match stem {
        "packed-int32" => {
            let mut run = Vec::new();
            for i in 0..N {
                put_varint(&mut run, (i * 9000) & 0x3_FFFF);
            }
            put_len(&mut b, 17, &run);
        }
        "two-tier" => {
            put_tag(&mut b, 1, 0);
            put_varint(&mut b, 1_234_567);
            put_tag(&mut b, 4, 0);
            put_varint(&mut b, 0xDEAD_BEEF);
            put_tag(&mut b, 5, 0);
            put_varint(&mut b, zigzag(-42));
            let mut run = Vec::new();
            for i in 0..N {
                put_varint(&mut run, (i * 9000) & 0x3_FFFF);
            }
            put_len(&mut b, 17, &run);
            put_len(&mut b, 12, b"the quick brown fox");
        }
        "records" => b = record_set(),
        _ => {}
    }
    b
}

// One packed int64 field 1 carrying a value of every varint width, 1..10 bytes (10: a negative int64).
fn pool_input() -> Vec<u8> {
    let mut run = Vec::new();
    for width in 1..=9u32 {
        put_varint(&mut run, 1u64 << (7 * (width - 1)));
    }
    put_varint(&mut run, (-5i64) as u64);
    let mut b = Vec::new();
    put_len(&mut b, 1, &run);
    b
}

#[test]
fn every_stream_scenario_has_agreeing_arms() {
    let scenarios = rapidproto_bench::stream::scenarios();
    assert_eq!(
        scenarios.len(),
        107,
        "the C++ streaming bench's scenario count"
    );
    for sc in scenarios {
        let input = match &sc.source {
            Source::Stream(stem) => stream_input(stem),
            Source::Pool(_) => pool_input(),
            Source::Arena(_) => unreachable!("a streaming scenario reads stream payloads"),
        };
        let arms = (sc.arms)();
        let names: Vec<&str> = arms.iter().map(|a| a.name).collect();
        assert_eq!(names, ["generated", "wire"], "{}", sc.name);
        // Non-vacuous: every scenario's input has fields the arms sum.
        assert_ne!(all_agree(arms, &input), 0, "{} summed nothing", sc.name);
    }
}

#[test]
fn every_arena_scenario_is_listed_once() {
    let specs = rapidproto_bench::scenario_specs();
    // The 8 shapes, the 90-scenario `rv` sweep, and the 1M per-type sweep (15 `rv-zz`, 5 `rv-enum`).
    assert_eq!(specs.len(), 8 + 90 + 15 + 5);
    let mut names: Vec<&str> = specs.iter().map(|s| s.name.as_str()).collect();
    names.sort_unstable();
    names.dedup();
    assert_eq!(names.len(), specs.len());
}
