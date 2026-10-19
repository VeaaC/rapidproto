//! Ecosystem comparison harness: decode the shared `bench.proto` payloads with RapidProto's arena
//! decoder (arm 0, the baseline) and with each competitor crate, walking every field with one shared
//! checksum so a lazy/owned decoder is not flattered by skipping work. Competitors are added one at a
//! time (one module each under `competitors/`, one line in the `arms!` macro below): prost,
//! rust-protobuf, quick-protobuf, then femtopb.
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

/// How an arm handles a given scenario.
pub enum ArmRun {
    /// Decodes the payload and returns the fairness checksum.
    Supported(Decode),
    /// The arm cannot decode this scenario's schema (a feature gap, e.g. femtopb has no `map` support
    /// so it cannot decode `dataset`). The string is the reason, shown in the table. An unsupported
    /// arm is still LISTED and flagged in the output, never silently dropped, but it is excluded from
    /// the agreement gate and from timing.
    Unsupported(&'static str),
}

/// One decoder under test.
pub struct Arm {
    pub name: &'static str,
    pub run: ArmRun,
}

impl Arm {
    /// A supported arm: `f` decodes the payload and returns its fairness checksum.
    pub fn new(name: &'static str, f: impl Fn(&[u8]) -> u64 + 'static) -> Self {
        Self {
            name,
            run: ArmRun::Supported(Box::new(f)),
        }
    }

    /// An arm that cannot decode this scenario; `reason` is shown in the table.
    pub fn unsupported(name: &'static str, reason: &'static str) -> Self {
        Self {
            name,
            run: ArmRun::Unsupported(reason),
        }
    }

    /// The decoder, or `None` if this arm is unsupported for its scenario.
    pub fn decoder(&self) -> Option<&Decode> {
        match &self.run {
            ArmRun::Supported(d) => Some(d),
            ArmRun::Unsupported(_) => None,
        }
    }

    /// The unsupported reason, or `None` if this arm is supported.
    pub fn unsupported_reason(&self) -> Option<&'static str> {
        match &self.run {
            ArmRun::Unsupported(r) => Some(r),
            ArmRun::Supported(_) => None,
        }
    }
}

// The arms for a scenario, arm 0 (baseline) first, then each competitor. To add a competitor: write
// its module under `competitors/` and append one `competitors::<name>::$ty()` call here -- it then
// joins every scenario of its message type AND the agreement test, no other edits. An arm that cannot
// support a scenario returns `Arm::unsupported(..)` for it (e.g. femtopb for `dataset`), so it stays
// in the list and is flagged in the table rather than dropped. The google-upb capstone is appended only
// under its off-by-default feature (it carries a protoc/C-toolchain build dep the other arms avoid).
macro_rules! arms {
    ($ty:ident) => {{
        #[allow(unused_mut)]
        let mut v = vec![
            ours::$ty(),
            competitors::prost::$ty(),
            competitors::rust_protobuf::$ty(),
            competitors::quick_protobuf::$ty(),
            competitors::femtopb::$ty(),
        ];
        #[cfg(feature = "google-upb")]
        v.push(competitors::google_upb::$ty());
        v
    }};
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

/// Whether `arm`'s decode of `scenario` reads a packed fixed-width float field as an UNALIGNED
/// borrowed slice -- undefined behaviour. Only quick-protobuf's zero-copy `read_packed_fixed` does
/// this (see [`competitors::quick_protobuf`]), and only on the `Big`-shaped scenarios whose `reals`
/// field is `repeated double`: it hands back a `&[f64]` pointing straight into the (align-1) input.
/// Correct on x86 in a release build where the check is compiled out, but a misaligned reference --
/// it aborts under debug UB-checks / Miri, and would misread on a big-endian host. (Our own arena
/// decoder instead memcpy's packed fixed data into an aligned arena array, so it is sound.) The runner
/// uses this to drop the arm from these scenarios in a debug build and to flag the measurement in the
/// release table / ndjson, so the generated benchmark tables can mark it.
pub fn has_unaligned_fixed_slice_ub(scenario: &str, arm: &str) -> bool {
    arm == "quick-protobuf" && matches!(scenario, "big" | "big_varint" | "big_fixed" | "big_osm")
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

    // Encode a populated message with prost, then run every SUPPORTED arm over the same bytes and
    // assert they all agree -- the cross-arm correctness gate, without the external payload directory,
    // so it runs in CI. Unsupported arms (e.g. femtopb on `dataset`) carry no decoder and are skipped.
    // Generic over the scenario's arm list, so a newly-registered competitor is covered with no extra
    // test. The three types below exercise every field kind the schema has (scalars/strings/nested/
    // repeated scalar+string+message/map in Dataset; dispatch + nested in RecordSet; packed varint/
    // fixed/zigzag + enum + double in BigSet). prost is the reference encoder.
    fn all_agree(arms: Vec<Arm>, bytes: &[u8]) {
        let sums: Vec<u64> = arms
            .iter()
            .filter_map(|a| a.decoder().map(|d| d(bytes)))
            .collect();
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

    // Release-only. This scenario's `Big.reals` is `repeated double`, and quick-protobuf decodes it by
    // borrowing the input as an unaligned `&[f64]` -- UB that aborts under the debug `assert_unsafe_
    // precondition` alignment check (see `has_unaligned_fixed_slice_ub` and the quick_protobuf module).
    // A release build compiles that check out; the per-field checksums still agree on x86. CI runs it
    // via a dedicated `cargo test -p rapidproto-bench --release` step.
    #[cfg(not(debug_assertions))]
    #[test]
    fn bigset_arms_agree() {
        let bs = pb::BigSet {
            items: vec![pb::Big {
                numbers: vec![1, -2, 3_000_000_000],
                reals: vec![1.5, -2.25, 0.0],
                zz: vec![-1, 2, -3],
                // Only DEFINED Kind values here: pb-rs (quick-protobuf) generates closed enums whose
                // `From<i32>` maps any unknown value to the default, so it cannot round-trip an
                // out-of-range open-enum value the way the other arms do. The shared payloads likewise
                // carry only 0/1, so this stays faithful to what the bench actually measures.
                kinds: vec![pb::Kind::One as i32, 0, pb::Kind::One as i32],
            }],
        };
        all_agree(bigset_arms(), &bs.encode_to_vec());
    }
}
