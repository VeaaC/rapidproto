//! The C++ arena bench's real-world datasets, decoded from the bytes its dump writes under
//! `<dir>/external/`. Each scenario mirrors its C++ one: RapidProto's arms the C++ bench runs for it
//! (no competitors -- those C++ arms are protoc, upb and libosmium), the same bytes per call, and the
//! same checksum walk, ported field for field so the sums agree ACROSS languages, not just across arms:
//!
//! - `google_message1` / `google_message2` ([`messages`]): protobuf's published benchmark payloads
//!   (`tests/bench_arm_messages.hpp`). Built when the corpus is present (cfg `rp_bench_gm`, build.rs).
//! - `osm_blocks` / `osm_file` ([`osm`]): a pinned OpenStreetMap extract (`tests/bench_arm_osm.hpp`) --
//!   its inflated blocks decoded per call, and the raw file (framing, zlib inflate, decode) per call.
//! - `compute Instance` ([`compute`]): one googleapis compute.proto `Instance`, the large-schema arm
//!   (`tests/bench_arm_compute.hpp`). Behind the `compute` feature.
//!
//! The decoders come from build.rs (rapidproto-build); a scenario whose input the payload directory
//! lacks is skipped at run time.

#[cfg(feature = "compute")]
pub mod compute;
#[cfg(rp_bench_gm)]
pub mod messages;
pub mod osm;

use crate::payloads::Source;
use crate::runner::Scenario;
use crate::ArmsFn;

/// The real-world dataset scenarios this build has, in the C++ bench's order and under its names.
pub fn scenarios() -> Vec<Scenario> {
    let scenario = |name: &str, source: Source, arms: ArmsFn| Scenario {
        name: name.to_string(),
        cpp_name: Some(name.to_string()),
        source,
        arms,
        mem: None,
    };
    let mut out = Vec::new();
    #[cfg(rp_bench_gm)]
    out.extend([
        scenario(
            "google_message1",
            Source::External("google_message1"),
            messages::gm1_arms,
        ),
        scenario(
            "google_message2",
            Source::External("google_message2"),
            messages::gm2_arms,
        ),
    ]);
    out.extend([
        scenario("osm_blocks", Source::Batch("osm_blocks"), osm::blocks_arms),
        scenario("osm_file", Source::External("osm_file"), osm::file_arms),
    ]);
    #[cfg(feature = "compute")]
    out.push(scenario(
        "compute Instance",
        Source::External("compute_instance"),
        compute::arms,
    ));
    out
}
