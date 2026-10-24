//! The arena runner: the C++ arena bench's (`rapidproto_arena_bench`) scenarios over its dumped
//! payloads, with RapidProto's arms and each competitor's (see the crate docs). The loop, options and
//! output are the shared runner's (`runner.rs`).
//!
//! The real-world datasets (`src/external/`) need more than the payload directory: `google_message1`/`2`
//! need the corpus when this crate is built (build.rs), and `compute Instance` the `compute` feature.
//! Generating the compute decoder takes over ten minutes in cargo's default, unoptimized build script,
//! so build it with `--features compute --config 'profile.release.build-override.opt-level=1'`
//! (level 1: 2 and above currently overflow the generator's stack).
//!
//! BUILD PROFILE: plain `--release` (no LTO), matching the RapidProto-vs-C++ bench methodology and
//! what most users actually ship. Under the default profile a decoder's hot path stays inlined across
//! crate / codegen-unit boundaries only if it is ANNOTATED for it: RapidProto's wire readers are
//! `#[inline(always)]`, so they inline here; prost's decode is not force-inlined the same way, so it
//! gains from LTO + `codegen-units=1`. That is a real property of how each decoder is written -- a
//! competitor optimizing poorly under the default profile is the competitor's trade-off, not a
//! benchmark handicap -- so `--release` is a fair comparison. For transparency, LTO + `codegen-units=1`
//! narrows prost's gap by up to ~1.5x on the compute-bound scenarios (the allocation/validation-bound
//! shapes like `dataset` barely move -- that part is a model difference, not an optimization gap); add
//! `--config 'profile.release.lto="fat"' --config 'profile.release.codegen-units=1'` to see it.
//!
//! ```text
//! # produce the shared payloads once (the C++ arena bench's, ~1.7 GB; keep them on disk):
//! RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_arena_bench
//! # run pinned + aligned (the discipline note is runner.rs's), one run at a time, on a quiesced box:
//! RAPIDPROTO_BENCH_DIR=<dir> RUSTFLAGS="-Cllvm-args=-align-all-functions=6" \
//!     taskset -c 0 cargo run -p rapidproto-bench --release --bin bench
//! ```
//!
//! GOOGLE-UPB CAPSTONE (feature `google-upb`, OFF by default): adds Google's official upb-backed
//! `protobuf` crate -- the only true arena-vs-arena peer (borrowed `*View<'_>` over a upb C arena). It
//! is the one arm that needs `protoc`: Google's codegen drives an external one whose version must
//! EXACTLY match the crate (currently 36.2). So it lives in
//! the separate `rapidproto-bench-upb` crate (its `::protobuf::` is v4; the rust-protobuf arm's is v3 --
//! incompatible, hence the split) and is gated behind the feature. Build a matching protoc from source
//! (portable; the prebuilt release binary also works) and point the bench at it:
//! ```text
//! git clone --depth 1 --recurse-submodules --shallow-submodules -b v36.2 \
//!     https://github.com/protocolbuffers/protobuf
//! cmake -S protobuf -B protobuf/_b -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF
//! cmake --build protobuf/_b --target protoc -j   # -> protobuf/_b/protoc-36.2.0
//! RAPIDPROTO_UPB_PROTOC="$PWD/protobuf/_b/protoc-36.2.0" RAPIDPROTO_BENCH_DIR=<payloads> \
//!     taskset -c 0 cargo run -p rapidproto-bench --release --bin bench --features google-upb
//! ```

use rapidproto_bench::runner::{self, Report};
use rapidproto_bench::{has_unaligned_fixed_slice_ub, scenario_specs};

fn main() {
    let report = Report {
        decoder: "rust-ecosystem",
        ub: has_unaligned_fixed_slice_ub,
        legend: "‡ = this measurement relied on UNDEFINED BEHAVIOUR: the arm read a packed \
                 fixed-width float field (`Big.reals`, `repeated double`) as an unaligned borrowed \
                 slice. Correct on x86 here, but a misaligned reference -- flag it in any table built \
                 from these numbers. (`ub_unaligned_fixed_slice` in the ndjson.)",
        // RapidProto's arms are the C++ bench's under Rust names; google-upb runs the upb kernel the
        // C++ `upb` arm does. The other competitors are Rust-only.
        cpp_arm: |arm| match arm {
            "rapidproto" => Some("arena-cold"),
            "rapidproto-warm" => Some("arena-warm"),
            "rapidproto-stream" => Some("streamgen"),
            "google-upb" => Some("upb"),
            _ => None,
        },
    };
    std::process::exit(runner::run(scenario_specs(), &report));
}
