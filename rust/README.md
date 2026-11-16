# RapidProto for Rust

This is the Cargo workspace of RapidProto's Rust decoders. The C++ CMake project does not build it.
To use the decoders, start with the [quick start](../README.md#rust) and the
[`rapidproto` documentation](https://docs.rs/rapidproto).

- `rapidproto/` is the runtime crate that the generated code uses.
- `rapidproto-build/` generates the decoders in a build script. It runs the C++ generator, compiled
  natively or as WebAssembly.
- `rustgen-golden/` compiles and tests the generated code for the test schemas, and fuzzes it.
- `rapidproto-diff/` formats decoded messages for the differential test against `protoc`.
- `examples/` holds a small consumer and an OpenStreetMap reader.
- `rapidproto-bench/` and `rapidproto-bench-upb/` are the benchmarks.

## Benchmarks

`rapidproto-bench` measures the Rust decoders the way the C++ benches measure the C++ ones, so the
two can be compared row for row.

### How it works

- The C++ benches produce every input; the Rust side only reads them. Both languages decode the
  same bytes.
- Each scenario mirrors a C++ one, and RapidProto's arms match the C++ bench's. The NDJSON output
  names the matching C++ row (`cpp_scenario`, `cpp_arm`).
- Nothing is timed until it is correct: a scenario's arms must agree on a checksum, and on the
  eight arena shapes and the real-world datasets that checksum must equal the C++ one.
- Every arm reads what it decodes into the checksum, so a lazy decoder pays for the work it defers
  and nothing is optimized away.
- Arms take turns within each round, in rotating order, and an arm's result is its median.
- Everything builds with plain `--release`, as users build it; no LTO.

### Running

The C++ benches write the inputs (~1.9 GB). Point the Rust ones at the same directory:

```sh
RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_arena_bench
RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_bench
export RAPIDPROTO_BENCH_DIR=<dir>
```

The real-world datasets need the corpus fetched on the C++ side
([Reproducing](../docs/benchmarks.md#reproducing)); without it they are skipped.

`RAPIDPROTO_BENCH_CHECK_ONLY=1` runs the correctness check alone, which is valid on any machine. For
timing, quiesce the machine (`tests/bench_box.sh setup`; `restore` undoes it) and run nothing else.
Pin to one core, and align functions and loops so unrelated code changes don't shift the results:

```sh
RUSTFLAGS="-Cllvm-args=-align-all-functions=6 -Cllvm-args=-align-loops=64" \
    taskset -c 2 cargo run -p rapidproto-bench --release --bin bench   # or --bin stream
```

`bench` compares the arena and streaming decoders with prost, rust-protobuf, quick-protobuf and
femtopb; `stream` compares the streaming decoder with a hand-written wire loop. `cargo bench`
compares two builds of a decoder.

### Reading the numbers

Throughput is what counts. Instructions per byte (the `perf_*` examples under `perf stat`) don't
depend on code layout, which makes them useful for small differences between builds, but more
instructions can still be faster. Measure a build against itself to see the noise before trusting a
small difference. Criterion reports MiB/s, everything else GB/s.

Published numbers go in [docs/benchmarks.md](../docs/benchmarks.md).

### Optional scenarios and arms

- `google_message1`/`2` need the corpus when this crate is built (`build/corpus`, or
  `RAPIDPROTO_FETCHED_CORPUS`).
- `--features compute` adds compute.proto, whose large decoder takes about a minute to compile.
- `--features google-upb` adds Google's upb crate. It needs `protoc` 36.2 (`RAPIDPROTO_UPB_PROTOC`).

The remaining options are documented in `rapidproto-bench/src/runner.rs`.
