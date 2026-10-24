# RapidProto — Rust support (work in progress)

This directory is the in-tree Cargo workspace for RapidProto's native Rust decoders, developed on
the long-running `rust` branch. It is **not yet released** and not built by the C++ CMake project.

- `rapidproto/` — the decode-only runtime (arena, borrowed views, wire primitives, errors).
- `rapidproto-build/` — the build-time driver that generates Rust decoders (via the WASM-embedded
  C++ generator).
- `rapidproto-bench/` — the decode benchmarks (below).

The public interface is still taking shape and is not yet stable. The scratch spikes at the repo root
(`rust-poc/`, `rust-m0/`) are exploratory and are **not** part of this workspace.

## Benchmarks

`rapidproto-bench` mirrors the C++ benches scenario for scenario, on the same input bytes:

| Target | Compares | Mirrors |
|---|---|---|
| `bench` binary | the arena decoder (cold, warm) and the streaming decoder against prost, rust-protobuf, quick-protobuf, femtopb | `rapidproto_arena_bench` |
| `stream` binary | the streaming decoder against a hand-written `rapidproto::wire` loop | `rapidproto_bench` |
| `benches/decode.rs`, `benches/stream.rs` | one decoder build against another (criterion A/B) | |
| `examples/perf_bench.rs`, `examples/perf_stream.rs` | instruction counts under `perf stat` | the C++ `ins/B` column |

**Inputs.** The Rust side builds no payloads. Both C++ benches dump theirs into one directory
(~1.9 GB, keep it), and every Rust target reads it:

```sh
RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_arena_bench
RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_bench
export RAPIDPROTO_BENCH_DIR=<dir>
```

The real-world datasets (google_message1/2, OSM, compute.proto) are dumped only when the C++ bench
was built with them fetched ([Reproducing](../docs/benchmarks.md#reproducing)); without them those
scenarios are skipped with a note.

**Check before timing.** `RAPIDPROTO_BENCH_CHECK_ONLY=1` runs only the agreement gate: every arm
of a scenario must return the same checksum, and on the real-world datasets that checksum must equal
the one the C++ dump recorded. It is valid on any machine; a timed run gates the same way first.

**Timing discipline.** A number is only worth reading from a run that is:

- on a quiesced box: `tests/bench_box.sh setup` (SMT and turbo off, `performance` governor;
  `restore` undoes it), with nothing else running — one background process is enough to move
  results;
- pinned: `taskset -c <core>`, a performance core on a hybrid CPU;
- aligned: `RUSTFLAGS="-Cllvm-args=-align-all-functions=6"`, so an unrelated edit that moves code
  does not move throughput by placement alone;
- a plain `--release` build. That is the baseline for every arm; a crate that only does well under LTO
  is measured as users build it.

```sh
RUSTFLAGS="-Cllvm-args=-align-all-functions=6" \
    taskset -c 2 cargo run -p rapidproto-bench --release --bin bench
```

**Reading the results.**

- Throughput (GB/s) is the objective. Instructions per byte (`perf stat -e instructions:u` over the
  `perf_*` examples) is immune to code placement, so it is the signal to compare two builds when
  their GB/s delta is small. It is not the verdict: an inlining change can add instructions and still
  win.
- The arena arms read every decoded field into their checksum, and the streaming arms exactly what
  their C++ counterparts read. This is deliberate: it denies a lazy decoder the work it skips, and
  keeps the decode from being optimized away.
- Before trusting a small A/B delta, measure the same build against itself; the difference is the
  noise floor.
- Criterion prints binary MiB/s; the runners and the C++ benches print decimal GB/s.

**Comparing with C++.** `RAPIDPROTO_BENCH_JSON=1` adds NDJSON records whose `cpp_scenario` and
`cpp_arm` name the C++ row they correspond to. On the varint sweeps' smallest rows (`rv <dist> 10`,
`100`) the C++ harness's per-call timing overhead dominates, so a ratio there compares the harnesses.
Published numbers belong in [docs/benchmarks.md](../docs/benchmarks.md), not here.

**Optional arms.**

- `google_message1`/`2` need the fetched corpus when `rapidproto-bench` is built (`build/corpus`, or
  `RAPIDPROTO_FETCHED_CORPUS`).
- `--features compute` adds the compute.proto scenario. Build it with
  `--config 'profile.release.build-override.opt-level=1'`, or generating its decoder takes over ten
  minutes.
- `--features google-upb` adds Google's upb-backed crate. It needs `protoc` 36.2 at build time
  (`RAPIDPROTO_UPB_PROTOC`; see `rapidproto-bench/src/main.rs`).

The runner's options (scenario and arm filters, rounds, NDJSON fields) are documented in
`rapidproto-bench/src/runner.rs`.
