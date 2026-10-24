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

`rapidproto-bench` runs the C++ benchmark scenarios in Rust, on the same input files:

- `bench`: the arena and streaming decoders against prost, rust-protobuf, quick-protobuf and femtopb
  (the C++ `rapidproto_arena_bench`).
- `stream`: the streaming decoder against a hand-written `rapidproto::wire` loop (the C++
  `rapidproto_bench`).
- `benches/` (criterion): one build of a decoder against another.
- `examples/perf_*`: instruction counts under `perf stat`.

The C++ benches write the inputs (~1.9 GB); point the Rust ones at the same directory:

```sh
RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_arena_bench
RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_bench
export RAPIDPROTO_BENCH_DIR=<dir>
```

The real-world datasets need the corpus fetched on the C++ side
([Reproducing](../docs/benchmarks.md#reproducing)); without it they are skipped.

`RAPIDPROTO_BENCH_CHECK_ONLY=1` checks correctness without timing: all arms must agree on a
checksum, and on the real-world datasets it must also match C++. This works on any machine.

For timing, quiesce the machine (`tests/bench_box.sh setup`; `restore` undoes it) and run nothing
else. Pin to one core, and align functions so unrelated code changes don't shift the results:

```sh
RUSTFLAGS="-Cllvm-args=-align-all-functions=6" \
    taskset -c 2 cargo run -p rapidproto-bench --release --bin bench
```

Use plain `--release`, without LTO: that is how users build.

Throughput is what counts. Instructions per byte don't depend on code layout, which makes them
useful for small differences between builds, but more instructions can still be faster. Measure a
build against itself to see the noise before trusting a small difference. Criterion reports MiB/s,
everything else GB/s.

Every arm reads what it decodes into a checksum, so a lazy decoder pays for the work and nothing is
optimized away.

`RAPIDPROTO_BENCH_JSON=1` adds NDJSON records; `cpp_scenario` and `cpp_arm` identify the matching
C++ result. Don't compare the `rv <dist> 10` and `100` rows with C++: its per-call timing overhead
dominates there. Published numbers go in [docs/benchmarks.md](../docs/benchmarks.md).

Optional scenarios and arms:

- `google_message1`/`2` need the corpus when this crate is built (`build/corpus`, or
  `RAPIDPROTO_FETCHED_CORPUS`).
- `--features compute` adds compute.proto. Build it with
  `--config 'profile.release.build-override.opt-level=1'`, or code generation takes over ten minutes.
- `--features google-upb` adds Google's upb crate. It needs `protoc` 36.2 (`RAPIDPROTO_UPB_PROTOC`).

The remaining options are documented in `rapidproto-bench/src/runner.rs`.
