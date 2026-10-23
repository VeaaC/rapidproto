//! Rust decode benchmarks, each mirroring a C++ bench scenario for scenario over the same bytes (the
//! shared payload directory, see [`payloads`]):
//!
//! - the arena decoder vs the Rust ecosystem (the `bench` runner, `benches/decode.rs`,
//!   `examples/perf_bench.rs`) -- the scenarios of the C++ `rapidproto_arena_bench`;
//! - the streaming decoder vs a hand-written wire loop (the `stream` runner, `benches/stream.rs`,
//!   `examples/perf_stream.rs`) -- the scenarios of the C++ `rapidproto_bench`; see [`stream`].
//!
//! ARENA ARMS: RapidProto's arena decoder cold (arm 0, the baseline: a fresh `Arena` per decode) and
//! warm (one arena, reset per decode -- the C++ `arena-warm` arm), its streaming decoder where the C++
//! bench runs one (`dataset`), then each competitor crate, walking every field with one shared
//! checksum so a lazy/owned decoder is not flattered by skipping work. Competitors are added one at a
//! time (one module each under `competitors/`, one line in the `arms!` macro below): prost,
//! rust-protobuf, quick-protobuf, femtopb (and google-upb behind its feature). The real-world datasets
//! run RapidProto's arms only, the ones the C++ bench runs for them ([`external`]).
//!
//! The checksum is a wrapping `u64` sum, mirrored field-for-field across arms so every arm in a
//! scenario produces the identical value or the run aborts. For scalars it follows the C++ ecosystem
//! bench convention (string/bytes -> byte length, bool -> 0/1, double -> IEEE bit pattern,
//! int32/enum/uint32 -> value, int64/uint64/fixed -> value); for repeated strings, repeated
//! sub-messages and maps it contributes the element COUNT (`.len()`), where the C++ bench sums each
//! element's length -- so the sum is a per-LANGUAGE agreement gate, NOT a cross-language checksum.
//! Because these are proto3 messages (zero/empty defaults contribute 0), a straight walk of every
//! accessor equals present-fields-only.

// RapidProto's cold + warm arena arms for one message type, `$ck` walking the decoded tree. The warm
// arm's arena lives in the closure (a `RefCell`, since an arm is a `Fn`) and is reset before every
// decode. Defined ahead of the modules so they can use it.
macro_rules! arena_arms {
    ($ty:ty, $ck:path) => {{
        let warm = ::std::cell::RefCell::new(::rapidproto::Arena::new());
        vec![
            $crate::Arm::new("rapidproto", |b| {
                let arena = ::rapidproto::Arena::new();
                $ck(<$ty>::decode(b, &arena).expect("rapidproto decode"))
            }),
            $crate::Arm::new("rapidproto-warm", move |b| {
                let mut arena = warm.borrow_mut();
                arena.reset();
                $ck(<$ty>::decode(b, &arena).expect("rapidproto decode"))
            }),
        ]
    }};
}

pub mod checksum;
pub mod competitors;
pub mod external;
mod ours;
pub mod payloads;
pub mod runner;
pub mod stream;

use payloads::Source;
use runner::Scenario;

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

// The arms for a scenario: ours first (arm 0, the cold baseline, then the others `ours` runs for that
// message type), then each competitor. To add a competitor: write its module under `competitors/` and
// append one `competitors::<name>::$ty()` call here -- it then joins every scenario of its message type
// AND the agreement test, no other edits. An arm that cannot support a scenario returns
// `Arm::unsupported(..)` for it (e.g. femtopb for `dataset`), so it stays in the list and is flagged in
// the table rather than dropped. The google-upb capstone is appended only under its off-by-default
// feature (it carries a protoc/C-toolchain build dep the other arms avoid).
macro_rules! arms {
    ($ty:ident) => {{
        let mut v = ours::$ty();
        v.extend([
            competitors::prost::$ty(),
            competitors::rust_protobuf::$ty(),
            competitors::quick_protobuf::$ty(),
            competitors::femtopb::$ty(),
        ]);
        #[cfg(feature = "google-upb")]
        v.push(competitors::google_upb::$ty());
        v
    }};
}

pub fn dataset_arms() -> Vec<Arm> {
    arms!(dataset)
}
pub fn bigset_arms() -> Vec<Arm> {
    arms!(bigset)
}
pub fn big_arms() -> Vec<Arm> {
    arms!(big)
}
/// The `rv-enum` sweep's arms: [`big_arms`], but quick-protobuf's generated enums are closed -- a value
/// that is not a `Kind` (the sweep's varints are not) decodes as the default -- so it is unsupported.
pub fn big_enum_arms() -> Vec<Arm> {
    big_arms()
        .into_iter()
        .map(|a| match a.name {
            "quick-protobuf" => {
                Arm::unsupported(a.name, "closed enums: unknown values decode as 0")
            }
            _ => a,
        })
        .collect()
}
pub fn wideset_arms() -> Vec<Arm> {
    arms!(wideset)
}
pub fn recordset_arms() -> Vec<Arm> {
    arms!(recordset)
}
pub fn particleset_arms() -> Vec<Arm> {
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
/// release table / ndjson, so the generated benchmark tables can mark it. The varint sweeps carry no
/// `reals`, so they never take that path.
pub fn has_unaligned_fixed_slice_ub(scenario: &str, arm: &str) -> bool {
    arm == "quick-protobuf" && matches!(scenario, "big" | "big_varint" | "big_fixed" | "big_osm")
}

/// The arena scenarios, in the C++ bench's order: `dataset` (with the arena memory report), the
/// real-world datasets ([`external`]), the other 7 shared shapes (named by their payload file), then
/// the repeated-varint sweeps -- `rv <dist> <count>` (a single `Big` carrying packed int64 `numbers`,
/// field 1) and, at 1M elements, the same varints as packed sint64 (`rv-zz`, field 3) and enum
/// (`rv-enum`, narrow widths only, field 4).
pub fn scenario_specs() -> Vec<Scenario> {
    // A shape is named by its payload file; its C++ row has a descriptive name (`particles` has no
    // row: the C++ bench decodes it only in its chunk-cap sweep).
    let shape = |name: &'static str, cpp: Option<&str>, arms: ArmsFn| Scenario {
        name: name.to_string(),
        cpp_name: cpp.map(str::to_string),
        source: Source::Arena(name),
        arms,
        mem: None,
    };
    let mut out = vec![Scenario {
        mem: Some(ours::dataset_memory),
        ..shape("dataset", Some("Dataset"), dataset_arms)
    }];
    out.extend(external::scenarios());
    out.extend([
        shape("big_varint", Some("packed int64(varint)"), bigset_arms),
        shape("big_fixed", Some("packed double(fixed)"), bigset_arms),
        shape("big_osm", Some("osm: sint64 deltas + enum"), bigset_arms),
        shape("big", Some("few msgs, big arrays"), bigset_arms),
        shape("wide", Some("many msgs, tiny arrays"), wideset_arms),
        shape(
            "records",
            Some("scalar records (dispatch-bound)"),
            recordset_arms,
        ),
        shape("particles", None, particleset_arms),
    ]);
    let sweep = |name: String, arms: ArmsFn| Scenario {
        source: payloads::pool(&name),
        cpp_name: Some(name.clone()),
        name,
        arms,
        mem: None,
    };
    for dist in payloads::varint_dists() {
        for count in payloads::VARINT_LENGTHS {
            out.push(sweep(payloads::rv_scenario_name(&dist, count), big_arms));
        }
    }
    for dist in payloads::varint_dists() {
        let count = payloads::TYPE_SWEEP_COUNT;
        out.push(sweep(
            payloads::rv_type_scenario_name("zz", &dist, count),
            big_arms,
        ));
        if dist.enum_narrow {
            out.push(sweep(
                payloads::rv_type_scenario_name("enum", &dist, count),
                big_enum_arms,
            ));
        }
    }
    out
}
