//! Ecosystem comparison harness: decode the shared `bench.proto` payloads with RapidProto's arena
//! decoder (arm 0, the baseline) and with each competitor crate, walking every field with one shared
//! checksum so a lazy/owned decoder is not flattered by skipping work. Competitors are added one at a
//! time; prost is the first.
//!
//! The checksum is the same walk the Rust A/B bench uses (`rustgen-golden/benches/decode.rs`): a
//! wrapping `u64` sum, mirrored field-for-field across arms so every arm in a scenario produces the
//! identical value or the run aborts. For scalars it follows the C++ ecosystem bench convention
//! (string/bytes -> byte length, bool -> 0/1, double -> IEEE bit pattern, int32/enum/uint32 -> value,
//! int64/uint64/fixed -> value); for repeated strings, repeated sub-messages and maps it contributes
//! the element COUNT (`.len()`), where the C++ bench sums each element's length -- so the sum is a
//! per-LANGUAGE agreement gate, NOT a cross-language checksum. Because these are proto3 messages
//! (zero/empty defaults contribute 0), a straight walk of every accessor equals present-fields-only.

/// prost's generated decoders for `bench.proto` (rendered by `build.rs`; package `bench` -> top-level
/// items here).
pub mod prost_bench {
    include!(concat!(env!("OUT_DIR"), "/bench.rs"));
}

/// Checksum walks over RapidProto's generated arena decoders (arm 0). These mirror the walks in
/// `rustgen-golden/benches/decode.rs` exactly, so this arm and the Rust A/B bench agree.
pub mod ours {
    use rustgen_golden::bench::arena::bench::{BigSet, Dataset, ParticleSet, RecordSet, WideSet};

    pub fn dataset(ds: &Dataset) -> u64 {
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

    pub fn bigset(bs: &BigSet) -> u64 {
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

    pub fn wideset(ws: &WideSet) -> u64 {
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

    pub fn recordset(rs: &RecordSet) -> u64 {
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

    pub fn particleset(ps: &ParticleSet) -> u64 {
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
}

/// The identical walks over prost's owned structs (public fields instead of accessors). Field-for-
/// field the same arithmetic as [`ours`], so the two arms produce the same sum.
pub mod prost_ck {
    use crate::prost_bench::{BigSet, Dataset, ParticleSet, RecordSet, WideSet};

    pub fn dataset(ds: &Dataset) -> u64 {
        let mut sum = ds.name.len() as u64 ^ (ds.version as u64);
        for p in &ds.people {
            sum = sum
                .wrapping_add(p.id as u64)
                .wrapping_add(p.name.len() as u64)
                .wrapping_add(p.email.len() as u64)
                .wrapping_add(u64::from(p.active))
                .wrapping_add(p.score.to_bits())
                .wrapping_add(p.created)
                .wrapping_add(p.tags.len() as u64)
                .wrapping_add(p.attributes.len() as u64)
                .wrapping_add(p.counters.len() as u64);
            if let Some(a) = &p.address {
                sum = sum.wrapping_add(u64::from(a.zip));
            }
            for &h in &p.history {
                sum = sum.wrapping_add(h as u64);
            }
        }
        sum
    }

    pub fn bigset(bs: &BigSet) -> u64 {
        let mut sum = 0u64;
        for it in &bs.items {
            for &n in &it.numbers {
                sum = sum.wrapping_add(n as u64);
            }
            for &r in &it.reals {
                sum = sum.wrapping_add(r.to_bits());
            }
            for &z in &it.zz {
                sum = sum.wrapping_add(z as u64);
            }
            for &k in &it.kinds {
                sum = sum.wrapping_add(k as u64);
            }
        }
        sum
    }

    pub fn wideset(ws: &WideSet) -> u64 {
        let mut sum = 0u64;
        for it in &ws.items {
            for &v in &it.a {
                sum = sum.wrapping_add(v as u64);
            }
            for &v in &it.b {
                sum = sum.wrapping_add(v as u64);
            }
            for &v in &it.c {
                sum = sum.wrapping_add(v as u64);
            }
            for &v in &it.d {
                sum = sum.wrapping_add(v as u64);
            }
            for x in &it.s {
                sum = sum.wrapping_add(x.len() as u64);
            }
            for x in &it.t {
                sum = sum.wrapping_add(x.len() as u64);
            }
        }
        sum
    }

    pub fn recordset(rs: &RecordSet) -> u64 {
        let mut sum = 0u64;
        for r in &rs.records {
            sum = sum
                .wrapping_add(r.f1 as u64)
                .wrapping_add(r.f2 as u64)
                .wrapping_add(r.f3 as u64)
                .wrapping_add(r.f4 as u64)
                .wrapping_add(u64::from(r.f5))
                .wrapping_add(r.f6 as u64)
                .wrapping_add(u64::from(r.f7))
                .wrapping_add(r.f8)
                .wrapping_add(r.f9 as u64)
                .wrapping_add(r.f10 as u64)
                .wrapping_add(r.f11 as u64)
                .wrapping_add(r.f12 as u64)
                .wrapping_add(r.f13 as u64)
                .wrapping_add(r.f14 as u64)
                .wrapping_add(r.f15 as u64)
                .wrapping_add(r.f18 as u64)
                .wrapping_add(r.f19 as u64)
                .wrapping_add(r.f20 as u64);
            for s in [r.s1.as_ref(), r.s2.as_ref()].into_iter().flatten() {
                sum = sum
                    .wrapping_add(s.a as u64)
                    .wrapping_add(s.b as u64)
                    .wrapping_add(s.c as u64)
                    .wrapping_add(s.d as u64)
                    .wrapping_add(u64::from(s.e))
                    .wrapping_add(u64::from(s.f));
            }
        }
        sum
    }

    pub fn particleset(ps: &ParticleSet) -> u64 {
        let mut sum = 0u64;
        for p in &ps.items {
            sum = sum
                .wrapping_add(p.id as u64)
                .wrapping_add(u64::from(p.color));
            if let Some(v) = &p.position {
                sum = sum
                    .wrapping_add(v.x.to_bits())
                    .wrapping_add(v.y.to_bits())
                    .wrapping_add(v.z.to_bits());
            }
            if let Some(v) = &p.velocity {
                sum = sum
                    .wrapping_add(v.x.to_bits())
                    .wrapping_add(v.y.to_bits())
                    .wrapping_add(v.z.to_bits());
            }
        }
        sum
    }
}

/// Decode `input` and return the fairness checksum (one decoder's whole job, boxed so arms of
/// different decoders share a type).
pub type Decode = Box<dyn Fn(&[u8]) -> u64>;

/// Builds the arms for a scenario (arm 0 = baseline), so several scenarios can share one builder.
pub type ArmsFn = fn() -> Vec<Arm>;

/// One decoder under test.
pub struct Arm {
    pub name: &'static str,
    pub run: Decode,
}

/// The arms for a scenario, arm 0 first (the baseline). Keyed by the message type the scenario's
/// payload holds (several scenarios share a type over different payloads).
pub mod arms {
    use super::{ours, prost_bench, prost_ck, Arm};
    use prost::Message;
    use rapidproto::Arena;
    use rustgen_golden::bench::arena::bench as rp;

    macro_rules! pair {
        ($ty:ident, $ck:ident) => {
            vec![
                Arm {
                    name: "rapidproto",
                    run: Box::new(|b| {
                        let arena = Arena::new();
                        ours::$ck(rp::$ty::decode(b, &arena).expect("rapidproto decode"))
                    }),
                },
                Arm {
                    name: "prost",
                    run: Box::new(|b| {
                        prost_ck::$ck(&prost_bench::$ty::decode(b).expect("prost decode"))
                    }),
                },
            ]
        };
    }

    pub fn dataset() -> Vec<Arm> {
        pair!(Dataset, dataset)
    }
    pub fn bigset() -> Vec<Arm> {
        pair!(BigSet, bigset)
    }
    pub fn wideset() -> Vec<Arm> {
        pair!(WideSet, wideset)
    }
    pub fn recordset() -> Vec<Arm> {
        pair!(RecordSet, recordset)
    }
    pub fn particleset() -> Vec<Arm> {
        pair!(ParticleSet, particleset)
    }
}

/// The 8 shared scenarios, in the C++ bench's order: `(scenario/file name, arms-for-its-type)`.
pub fn scenario_specs() -> Vec<(&'static str, ArmsFn)> {
    vec![
        ("dataset", arms::dataset),
        ("big_varint", arms::bigset),
        ("big_fixed", arms::bigset),
        ("big_osm", arms::bigset),
        ("big", arms::bigset),
        ("wide", arms::wideset),
        ("records", arms::recordset),
        ("particles", arms::particleset),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use prost::Message;

    // Encode a populated message with prost, then decode the SAME bytes with both arms and assert the
    // fairness checksums agree -- the cross-arm correctness gate, without needing the external payload
    // directory, so it runs in CI. Covers the field kinds the schema has (scalars/strings/nested/
    // repeated scalar+string+message/map in Dataset; dispatch + nested in RecordSet; packed varint/
    // fixed/zigzag + enum + double in BigSet).
    fn roundtrip<P: Message, F: Fn(&[u8]) -> u64, G: Fn(&[u8]) -> u64>(msg: &P, ours: F, prost: G) {
        let bytes = msg.encode_to_vec();
        assert_eq!(ours(&bytes), prost(&bytes), "arm checksums disagree");
    }

    #[test]
    fn dataset_arms_agree() {
        use prost_bench::*;
        let ds = Dataset {
            name: "acme".into(),
            version: 7,
            people: vec![
                Person {
                    id: 42,
                    name: "ada".into(),
                    email: "ada@x.io".into(),
                    active: true,
                    score: 3.5,
                    created: 1234567890,
                    address: Some(Address {
                        street: "1 st".into(),
                        city: "ny".into(),
                        zip: 10001,
                    }),
                    tags: vec!["a".into(), "bb".into()],
                    history: vec![1, -2, 3],
                    attributes: vec![Attribute {
                        key: "k".into(),
                        value: "v".into(),
                    }],
                    counters: [("x".to_string(), 1), ("y".to_string(), -3)]
                        .into_iter()
                        .collect(),
                },
                Person::default(),
            ],
        };
        roundtrip(
            &ds,
            |b| arms::dataset()[0].run.as_ref()(b),
            |b| arms::dataset()[1].run.as_ref()(b),
        );
    }

    #[test]
    fn recordset_arms_agree() {
        use prost_bench::*;
        let rs = RecordSet {
            records: vec![
                Record {
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
                    s1: Some(Sample {
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
                Record::default(),
            ],
        };
        roundtrip(
            &rs,
            |b| arms::recordset()[0].run.as_ref()(b),
            |b| arms::recordset()[1].run.as_ref()(b),
        );
    }

    #[test]
    fn bigset_arms_agree() {
        use prost_bench::*;
        let bs = BigSet {
            items: vec![Big {
                numbers: vec![1, -2, 3_000_000_000],
                reals: vec![1.5, -2.25, 0.0],
                zz: vec![-1, 2, -3],
                kinds: vec![Kind::One as i32, 0, 42],
            }],
        };
        roundtrip(
            &bs,
            |b| arms::bigset()[0].run.as_ref()(b),
            |b| arms::bigset()[1].run.as_ref()(b),
        );
    }
}
