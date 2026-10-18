//! Ecosystem comparison harness: decode the shared `bench.proto` payloads with RapidProto's arena
//! decoder (arm 0, the baseline) and with each competitor crate, walking every field with one shared
//! checksum so a lazy/owned decoder is not flattered by skipping work. Competitors are added one at a
//! time (one module each under `competitors/`, one line in the `arms!` macro below); prost is the
//! first.
//!
//! The checksum is the same walk the Rust A/B bench uses (`rustgen-golden/benches/decode.rs`): a
//! wrapping `u64` sum, mirrored field-for-field across arms so every arm in a scenario produces the
//! identical value or the run aborts. For scalars it follows the C++ ecosystem bench convention
//! (string/bytes -> byte length, bool -> 0/1, double -> IEEE bit pattern, int32/enum/uint32 -> value,
//! int64/uint64/fixed -> value); for repeated strings, repeated sub-messages and maps it contributes
//! the element COUNT (`.len()`), where the C++ bench sums each element's length -- so the sum is a
//! per-LANGUAGE agreement gate, NOT a cross-language checksum. Because these are proto3 messages
//! (zero/empty defaults contribute 0), a straight walk of every accessor equals present-fields-only.

pub mod checksum;
mod competitors;
mod ours;

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

// The arms for a scenario, arm 0 (baseline) first, then each competitor. To add a competitor: write
// its module under `competitors/` and append one `competitors::<name>::$ty()` call here -- it then
// joins every scenario of its message type AND the agreement test, no other edits.
macro_rules! arms {
    ($ty:ident) => {
        vec![ours::$ty(), competitors::prost::$ty()]
    };
}

fn dataset_arms() -> Vec<Arm> {
    arms!(dataset)
}
fn bigset_arms() -> Vec<Arm> {
    arms!(bigset)
}
fn wideset_arms() -> Vec<Arm> {
    arms!(wideset)
}
fn recordset_arms() -> Vec<Arm> {
    arms!(recordset)
}
fn particleset_arms() -> Vec<Arm> {
    arms!(particleset)
}

/// The 8 shared scenarios, in the C++ bench's order: `(scenario/file name, arms-for-its-type)`.
pub fn scenario_specs() -> Vec<(&'static str, ArmsFn)> {
    vec![
        ("dataset", dataset_arms),
        ("big_varint", bigset_arms),
        ("big_fixed", bigset_arms),
        ("big_osm", bigset_arms),
        ("big", bigset_arms),
        ("wide", wideset_arms),
        ("records", recordset_arms),
        ("particles", particleset_arms),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use competitors::prost::pb;
    use prost::Message;

    // Encode a populated message with prost, then run EVERY arm over the same bytes and assert they
    // all agree -- the cross-arm correctness gate, without the external payload directory, so it runs
    // in CI. Generic over the scenario's arm list, so a newly-registered competitor is covered with no
    // extra test. The three types below exercise every field kind the schema has (scalars/strings/
    // nested/repeated scalar+string+message/map in Dataset; dispatch + nested in RecordSet; packed
    // varint/fixed/zigzag + enum + double in BigSet). prost is the reference encoder.
    fn all_agree(arms: Vec<Arm>, bytes: &[u8]) {
        let sums: Vec<u64> = arms.iter().map(|a| (a.run)(bytes)).collect();
        assert!(
            sums.iter().all(|&s| s == sums[0]),
            "arm checksums disagree: {sums:?}"
        );
    }

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
        all_agree(dataset_arms(), &ds.encode_to_vec());
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
        all_agree(recordset_arms(), &rs.encode_to_vec());
    }

    #[test]
    fn bigset_arms_agree() {
        let bs = pb::BigSet {
            items: vec![pb::Big {
                numbers: vec![1, -2, 3_000_000_000],
                reals: vec![1.5, -2.25, 0.0],
                zz: vec![-1, 2, -3],
                kinds: vec![pb::Kind::One as i32, 0, 42],
            }],
        };
        all_agree(bigset_arms(), &bs.encode_to_vec());
    }
}
