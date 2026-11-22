# Rust workspace

`rust/` is the Cargo workspace for RapidProto's Rust side. It holds the two published crates, their
tests and examples, and a benchmark crate that times RapidProto against other Rust decoders. If you
only want to decode with the crates, start with the [Rust quick start](../README.md#rust) in the
root README and the crate documentation for [`rapidproto`](https://docs.rs/rapidproto) and
[`rapidproto-build`](https://docs.rs/rapidproto-build). The two examples in this directory are
written for you too.

CMake doesn't build any of this; every `cargo` command below runs from `rust/`. The Rust generator
isn't here either. It's C++, in [`src/rustgen/`](../src/rustgen/).
[CONTRIBUTING.md](../CONTRIBUTING.md#rust) has the test commands, what to regenerate when the
generator changes, and the ways a pull request that touches only C++ can still break Rust CI.

## Crates

- [`rapidproto/`](rapidproto/) is the runtime.
- [`rapidproto-build/`](rapidproto-build/) runs the generator from a build script or as the
  `rapidproto-rust` command, either
  [built natively or as prebuilt WebAssembly](https://docs.rs/rapidproto-build/latest/rapidproto_build/#how-the-generator-runs).
- [`rustgen-golden/`](rustgen-golden/) compiles the Rust generator's
  [goldens](../CONTRIBUTING.md#goldens) for the test schemas and a few others, and runs the decode,
  `Debug`, serde and streaming tests against them.
- [`rustgen-golden/macro-check/`](rustgen-golden/macro-check/) (package
  `rustgen-golden-macro-check`) calls every generated `*_stream!` macro with all fields selected,
  because the compiler checks the code a macro expands to for a field only when a call selects that
  field. With the golden crate as its only dependency, it also catches a macro that names
  `rapidproto` directly.
- [`rustgen-golden/fuzz/`](rustgen-golden/fuzz/README.md) (package `rapidproto-fuzz`) fuzzes the
  arena decoders. It has its own workspace and isn't built by commands run here.
- [`rapidproto-diff/`](rapidproto-diff/) turns a decoded message into one line of JSON for the
  [differential test](../CONTRIBUTING.md#the-differential-against-protobuf) and for
  `rustgen-golden`'s serde tests.
- [`examples/consumer/`](examples/consumer/README.md) (package `consumer-example`) generates one
  schema's decoders in a small `build.rs` and uses both decoders.
- [`examples/osm-pbf/`](examples/osm-pbf/README.md) (package `osm-pbf-example`) prints statistics
  about an OpenStreetMap PBF file, using either decoder.
- [`rapidproto-bench/`](rapidproto-bench/) is the benchmark crate. Its optional google-protobuf
  competitor lives in [`rapidproto-bench-upb/`](rapidproto-bench-upb/), outside the workspace.

## Benchmarks

`rapidproto-bench` decodes the payloads that the C++ benchmarks write. `dataset`, for one, is 2,000
`Person` messages in one `Dataset` from the [benchmark schema](../tests/bench/bench.proto). Each
scenario covers one payload or a pool of them, and every scenario but `particles` decodes the same
bytes as a row of the C++ benches. The [benchmark page](../docs/benchmarks.md#rust) sets each Rust
decoder beside prost and RapidProto's Rust numbers beside its C++ ones. Its
[reproduction notes](../docs/benchmarks.md#reproducing-the-rust-numbers) give the build features and
the C++ run behind those numbers.

The [`bench`](rapidproto-bench/src/main.rs) runner decodes the payloads of the C++
[arena bench](../tests/bench_arena.cpp), and [`stream`](rapidproto-bench/src/stream_main.rs) those
of the [streaming bench](../tests/bench_streamgen.cpp). Both check every decoder's output before
timing it. After a change to the runtime or to generated code, that check is usually all you
need, and a check-only run gives it to you without any machine setup.

### Running the bench

```sh
# from the repository root
cmake --preset clang
cmake --build build/clang --target rapidproto_arena_bench rapidproto_bench
RAPIDPROTO_BENCH_DUMP_DIR=$PWD/build/payloads build/clang/rapidproto_arena_bench
RAPIDPROTO_BENCH_DUMP_DIR=$PWD/build/payloads build/clang/rapidproto_bench

cd rust
export RAPIDPROTO_BENCH_DIR=$PWD/../build/payloads
export RUSTFLAGS="-Cllvm-args=-align-all-functions=6 -Cllvm-args=-align-loops=64"
cargo build --release -p rapidproto-bench --bins --examples
RAPIDPROTO_BENCH_CHECK_ONLY=1 target/release/bench    # checksums only, no timing
RAPIDPROTO_BENCH_CHECK_ONLY=1 target/release/stream

../tests/bench_box.sh setup                           # uses sudo
taskset -c 2 target/release/bench                     # or target/release/stream
../tests/bench_box.sh restore
```

You run the C++ half once, and again when the C++ benches, the benchmark schema or the fetched
corpus change. It needs:

- Linux, the only platform the C++ benches build on
- protobuf, for the arena bench
- the [fetched corpus](../CONTRIBUTING.md#the-real-world-schema-corpus) and the pinned OSM extract,
  for the real-world datasets `google_message1`, `google_message2`, `osm_blocks`, `osm_file` and
  `compute Instance`

Fetch those two before `cmake --preset`, which is when the build looks for them.
[Reproducing](../docs/benchmarks.md#reproducing) lists the packages, the fetch commands and what
[`bench_box.sh`](../tests/bench_box.sh) changes on the machine. With `RAPIDPROTO_BENCH_DUMP_DIR`
set, each C++ bench writes its payloads, about 2 GB for the two, and exits without timing anything.
`bench` notes and skips any real-world dataset whose payload is missing.

Keep other work off the machine between `setup` and `restore`. Build first and pin only the
binary, because `taskset -c 2 cargo run` would also confine the compile to that core. The
`RUSTFLAGS` align every function and loop to 64 bytes. Every timed build needs them, or a change
elsewhere in the code can move a hot loop to a new address and change its throughput.

A check-only run prints a `checksum ok` line per scenario. Here's part of one from `bench`:

```text
checksum ok  dataset                    0x2afc7ad07f1645ba = C++  (6 arms, 1 unsupported, 1 buffer(s), 368611 bytes/call)
...
note: scenario compute Instance SKIPPED -- not built: needs the `compute` feature (see src/main.rs)
...
checksum ok  records sparse (dropped)   0x00000000001cd3d0  (2 arms, 1 buffer(s), 1342210 bytes/call)
...

all arms agree on 123 scenario(s), 1 skipped (checksum-only run; no timing).
```

A scenario's arms are the decoders run on it. On `dataset` six arms agreed. The seventh, femtopb,
can't decode the map field in `Dataset`'s `Person` messages and is listed as unsupported, not run.

`= C++` means the sum also matched the C++ dump's checksum. The dump has checksums for the
real-world datasets and for the eight scenarios named after their payload files: `dataset`,
`big_varint`, `big_fixed`, `big_osm`, `big`, `wide`, `records` and `particles`. The C++ bench writes
the checksum for `particles` without timing that payload. `records sparse (dropped)` decodes the
`records` payload under a [decode profile](../docs/profiles.md) that keeps three of each record's
20 fields, and it has no C++ checksum to match.

When a check fails, the runner prints the sums, exits with code 1 and checks nothing after that
scenario. A single arm out of line with the rest is usually the arm to look at, though `rapidproto`
and `rapidproto-warm` run the same generated decoder and a bug in it moves both. On the eight
payload-named scenarios the other crates decode the same bytes independently. There, if the Rust
arms agree with each other but not with C++, rule out a stale dump first by writing the payloads
again from a C++ build of your checkout. On the real-world datasets every Rust arm is RapidProto's,
so the same pattern there can also be a bug in the Rust generator or runtime. A sum of zero fails
too, since it means the payload decoded to nothing. A decoder that rejects a valid payload doesn't
get that far: its arm panics, and the run ends with exit code 101.

### Arms

Each runner's arm 0 is its baseline:

| Arm | Runner | Decodes with | C++ arm |
|---|---|---|---|
| `rapidproto` (arm 0) | `bench` | The arena decoder, a new arena per decode | `arena-cold` |
| `rapidproto-warm` | `bench` | The arena decoder, one arena reset before each decode. Arm 0 on `osm_file`, where there's no `rapidproto` arm | `arena-warm` |
| `rapidproto-stream` | `bench` | The streaming decoder, on `dataset` and the real-world datasets except `compute Instance` | `streamgen` |
| `prost`, `rust-protobuf`, `quick-protobuf`, `femtopb` | `bench` | [Those crates](../docs/benchmarks.md#other-rust-decoders), on every scenario except `records sparse (dropped)` and the real-world datasets | none |
| `google-upb` | `bench` | google-protobuf, on the same scenarios, [optional](#optional-scenarios-and-settings) | `upb` |
| `generated` (arm 0) | `stream` | The generated streaming decoder | `generated` |
| `wire` | `stream` | A hand-written loop over the runtime's wire readers | `wire` |

The C++ arm is the counterpart's name, not a promise that C++ ran it on every scenario. On the C++
bench's shape rows, the varint sweeps and `records sparse (dropped)`, C++ runs only protoc and
`arena-warm`, so compare `rapidproto-warm` there. `google-upb` and the C++ `upb` arm meet only on
`dataset`.

After adding an arm, run the [agreement tests](rapidproto-bench/tests/agreement.rs) in a debug and
a release build:

```sh
cargo test -p rapidproto-bench
cargo test -p rapidproto-bench --release
```

They need no payload directory. The arms decode messages encoded by prost, hand-built streaming
inputs and a [small OSM file](../examples/osm-pbf/testdata/mini.osm.pbf), and every arm of a
scenario has to agree. `bigset_arms_agree` checks `BigSet`, the message type of the `big*`
payloads, and runs only in release, because quick-protobuf's `BigSet` decoder reads an unaligned
slice and a debug build aborts on that. For `wide` and `particles`, which no test covers, the
check-only run is the first to compare a new arm.

### Timing

The bench is built with the plain `--release` profile, without LTO. Timing goes in rounds that each
run every arm once, with the order shifted by one arm from each round to the next. An arm's result
is its median over the rounds, in decimal GB/s like the C++ harness.

The `vs base` column compares each arm with arm 0 of the same run, whereas the benchmark page's
crate table measures every crate against prost.

The checksum walk over every decoded field is part of the timed work. That walk is where femtopb
does some of its decoding, since it decodes repeated fields only when they're read.

### Other tools

Unless you're working on decoder performance, you can skip these tools. Run them between `setup`
and `restore`, as you would a timed run.

```sh
# criterion: build each package's benches, then run them pinned
cargo bench -p rapidproto-bench --no-run
cargo bench -p rapidproto --bench wire --no-run
taskset -c 2 cargo bench -p rapidproto-bench --bench decode   # or --bench stream
taskset -c 2 cargo bench -p rapidproto --bench wire

# instructions per byte
SCENARIO=records perf stat -e instructions:u target/release/examples/perf_bench
```

Criterion compares each run with the previous one, so run the benches on the old build first and
then on the new one. The `decode` and `stream` benches time RapidProto's generated decoders without
competitors. `wire` times the runtime's wire readers alone,
on its own copies of six payloads from the C++ streaming bench. Criterion reports binary units
(MiB/s, GiB/s).

The perf examples ([`perf_bench.rs`](rapidproto-bench/examples/perf_bench.rs) and
[`perf_stream.rs`](rapidproto-bench/examples/perf_stream.rs)) decode one scenario until they've
covered about 2 GB, then print the byte total to divide the instruction count by. `perf stat`
needs `kernel.perf_event_paranoid` at 2 or lower, which `setup` also sets. Code layout barely moves
instructions per byte
([measured on the C++ benches](../docs/benchmarks.md#appendix-measurement-noise)). If two builds
differ by a few percent in throughput but run the same number of instructions, suspect layout or
run noise first, and time one build against itself to see how large that noise is.

### Optional scenarios and settings

You need these mostly to reproduce the published comparison:

- The build script generates the `google_message1` and `google_message2` decoders only if it finds
  the fetched corpus. It uses `RAPIDPROTO_FETCHED_CORPUS` when that's set (a relative path is taken
  from the repository root) and `build/corpus` otherwise. Cargo doesn't rerun the build script when
  the fetched corpus arrives after the crate's first build. Run `cargo clean -p rapidproto-bench`
  after fetching it.
- `--features compute` adds `compute Instance`, a message from the fetched corpus's
  `compute.proto`. Its decoder takes several minutes to compile in a release build.
- `--features google-upb` adds the `google-upb` arm. It needs a C compiler for upb and protoc 36.2.
  Point `RAPIDPROTO_UPB_PROTOC` at that protoc or put it first on `PATH`. The top of
  `rapidproto-bench/src/main.rs` explains how to build it from source.

Both runners also read these variables:

| Variable | Effect |
|---|---|
| `RAPIDPROTO_BENCH_JSON` | Also print an NDJSON record per result. `cpp_scenario` names its C++ row, or is null if it has none; `cpp_arm` is the arm's C++ name from the table above, or null for the other crates |
| `RAPIDPROTO_BENCH_ONLY=<text>` | Run only the scenarios whose Rust or C++ name contains `<text>` |
| `ARMS=<arm>,<arm>` | Run only these arms and arm 0 |
| `ITERS=<n>` | Decode calls per sample, instead of about 10 ms of the baseline |
| `ROUNDS=<n>` | Rounds per scenario, 20 by default |
