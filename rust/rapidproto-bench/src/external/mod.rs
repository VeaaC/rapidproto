//! The C++ arena bench's real-world datasets, decoded from the bytes its dump writes under
//! `<dir>/external/`. Each scenario mirrors its C++ one: RapidProto's arms the C++ bench runs for it
//! (no competitors -- those C++ arms are protoc, upb and libosmium), the same bytes per call, and the
//! same checksum walk, ported field for field so the sums agree ACROSS languages -- the runner holds
//! them to the C++ checksum the dump records beside each input:
//!
//! - `google_message1` / `google_message2` ([`messages`]): protobuf's published benchmark payloads
//!   (`tests/bench_arm_messages.hpp`). Their schemas come from the fetched corpus, so they are built
//!   only when build.rs finds it (cfg `rp_bench_gm`).
//! - `osm_blocks` / `osm_file` ([`osm`]): a pinned OpenStreetMap extract (`tests/bench_arm_osm.hpp`) --
//!   its inflated blocks decoded per call, and the raw file (framing, zlib inflate, decode) per call.
//! - `compute Instance` ([`compute`]): one googleapis compute.proto `Instance`, the large-schema arm
//!   (`tests/bench_arm_compute.hpp`). Behind the `compute` feature.
//!
//! A scenario this build has no decoder for is still listed, its one arm unsupported with the reason,
//! so a run says why it is missing; one whose input the payload directory lacks is skipped at run
//! time.

#[cfg(feature = "compute")]
pub mod compute;
#[cfg(rp_bench_gm)]
pub mod messages;
pub mod osm;

use crate::payloads::Source;
use crate::runner::Scenario;
use crate::ArmsFn;

/// The real-world dataset scenarios, in the C++ bench's order and under its names.
pub fn scenarios() -> Vec<Scenario> {
    let scenario = |name: &str, source: Source, arms: ArmsFn| Scenario {
        name: name.to_string(),
        cpp_name: Some(name.to_string()),
        source,
        arms,
        mem: None,
    };
    #[cfg(rp_bench_gm)]
    let (gm1, gm2): (ArmsFn, ArmsFn) = (messages::gm1_arms, messages::gm2_arms);
    #[cfg(not(rp_bench_gm))]
    let (gm1, gm2): (ArmsFn, ArmsFn) = (gm_not_built, gm_not_built);
    #[cfg(feature = "compute")]
    let compute: ArmsFn = compute::arms;
    #[cfg(not(feature = "compute"))]
    let compute: ArmsFn = compute_not_built;
    vec![
        scenario("google_message1", Source::External("google_message1"), gm1),
        scenario("google_message2", Source::External("google_message2"), gm2),
        scenario("osm_blocks", Source::Batch("osm_blocks"), osm::blocks_arms),
        scenario("osm_file", Source::External("osm_file"), osm::file_arms),
        scenario(
            "compute Instance",
            Source::External("compute_instance"),
            compute,
        ),
    ]
}

#[cfg(not(rp_bench_gm))]
fn gm_not_built() -> Vec<crate::Arm> {
    vec![crate::Arm::unsupported(
        "rapidproto",
        "not built: build.rs found no corpus (python3 tests/fetch_corpus.py; then \
         `cargo clean -p rapidproto-bench`)",
    )]
}

#[cfg(not(feature = "compute"))]
fn compute_not_built() -> Vec<crate::Arm> {
    vec![crate::Arm::unsupported(
        "rapidproto",
        "not built: needs the `compute` feature (see src/main.rs)",
    )]
}
