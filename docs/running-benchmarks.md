# Running the benchmarks

[`tests/bench.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/bench.py) builds the two C++
benchmark programs, runs them and writes the results to a snapshot, which it can show on its own or
compare with another. Use it to reproduce the published numbers or to see whether a change to
RapidProto made a decoder slower. What the benchmarks need, and how to prepare the machine, is under
[Reproducing](benchmarks.md#reproducing). [Benchmark method](benchmark-method.md) explains what the
programs measure. The Rust benchmark has its own instructions in
[rust/README.md](https://github.com/VeaaC/rapidproto/blob/main/rust/README.md#running-the-bench).

## Building protobuf

The `protoc` arms are built against whichever protobuf `find_package(Protobuf)` finds. It prefers
the package file that protobuf's own CMake build installs, which also links the Abseil libraries
newer releases need, and falls back on CMake's FindProtobuf module for older installations such as a
distribution's 3.x package. `protoc` comes from the same installation as the library, and a
snapshot's header records the library's version as `protobuf_version`.

These commands, run from the repository root, build protobuf v35.1 and configure a g++-13
benchmark build against it:

```sh
git clone --branch v35.1 https://github.com/protocolbuffers/protobuf build/deps/protobuf-src
cmake -S build/deps/protobuf-src -B build/deps/protobuf-build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_FORCE_FETCH_DEPENDENCIES=ON \
    -DCMAKE_INSTALL_PREFIX=$HOME/protobuf-35.1
cmake --build build/deps/protobuf-build --parallel
cmake --install build/deps/protobuf-build
cmake --preset gcc -B build/gcc-pb35 -DCMAKE_PREFIX_PATH=$HOME/protobuf-35.1
```

The protobuf sources and build go under `build/deps/`, which git ignores, so `bench.py experiment`
won't count them as untracked files. `protobuf_FORCE_FETCH_DEPENDENCIES=ON` makes protobuf's CMake
download the Abseil release it pins and ignore any Abseil already installed. The library behind the
published numbers was built with these flags. `CMAKE_POSITION_INDEPENDENT_CODE=ON` compiles it with
`-fPIC`, which limits what GCC inlines inside it, so leave it in to time the same code.

## Running and comparing

```sh
python3 tests/bench.py run                             # five runs of both programs, one snapshot
python3 tests/bench.py table A.ndjson [B.ndjson ...]   # show one snapshot, or compare several
python3 tests/bench.py diff OLD.ndjson NEW.ndjson      # the regression check, exit 1 on failure
python3 tests/bench.py experiment main HEAD            # build, run and diff two commits
```

`run` and `experiment` build in `build/gcc-pb35` unless `--build-dir` names another directory. `run`
runs each program five times, or as often as `--repeat` says, and writes
`bench_snapshots/<compiler>-<rev>.ndjson` or the file that `--out` names. Git ignores
`bench_snapshots/`. Besides each arm's median run, a snapshot keeps every run's GB/s as `gb_s_runs`
and the arm's spread as `spread_pct`
([Measurement noise](benchmark-method.md#measurement-noise) defines it).

`bench.py` starts each program under `taskset` on core 2. `--core` names another core, and
`--core none` leaves `taskset` out. The program then pins itself to the allowed core with the
highest advertised maximum frequency. Under `taskset` that's the one core, and without it the
fastest core of the machine.

Given one snapshot, `table` shows each arm's GB/s, cyc/B, ins/B and `vs base`, followed by a memory
table. Given several, it puts their GB/s and ins/B in adjacent columns and names the best snapshot
on each row, and for a pair it also gives the change.

[Timing](benchmark-method.md#timing) says what cyc/B and ins/B count. A change of clock speed moves
cyc/B less than GB/s, except on an arm that waits on memory. A stall on memory takes the same time
at any clock, which at a higher clock is more cycles. ins/B is steady enough (the
[July 2026 study](benchmark-method.md#machine-settings) saw no change over 12 runs) to show a change
too small to see through the timing noise. Both columns need `kernel.perf_event_paranoid` at 2 or
lower; above that the programs leave them out and `table` prints `n/a`.

`vs base` is each arm's percentage gain over arm 0, from the same per-round ratios that decide when
sampling stops. We don't publish it. Its cycle counts leave out time spent in the kernel, and its
arm 0 is `protoc` even where the published tables divide by `protoc-warm`.

Besides the scenarios that [Payloads](benchmark-method.md#payloads) lists, the streaming program
runs `two-tier` and `two-tier-mbtag`. Each times one decoder compiled in a file of its own, arm 0
`micro (own TU)`, against the same decoder compiled inside a large file, `large TU`. No published
table uses them.

`run --compile` and `experiment --compile` add the
[compile-cost sweep](benchmark-method.md#the-compile-cost-sweep) to the snapshot, which takes under
two minutes. Both rebuild `rapidprotoc` first, so `experiment` measures each commit's own generator.

## The regression check

`bench.py diff` fails an arm whose GB/s dropped by more than both the threshold and the arm's own
noise. The threshold is 10% unless `--threshold` sets another, and that default sits above the 6.5%
by which relinking alone moved an arm under [Code placement](benchmark-method.md#code-placement).
The noise is the larger `spread_pct` of the two snapshots. A drop past the threshold that stays
within the noise is listed on its own and passes. A failing arm looks like this, with invented
figures:

```text
regressions (GB/s down, beyond 10.0% and the arm's own noise)
  decoder scenario                arm            old GB/s new GB/s    delta   noise    ins/B
  arena   Dataset                 arena-warm         1.36     1.16   -15.0%    0.1%    +0.0%
```

An arm whose own code didn't change can fail it too, as `arena-warm` on `rv fx1 100k` did with
g++-13 ([The remaining noise](benchmark-method.md#the-remaining-noise)).

`diff` leaves the packed-varint pools of 10 and 100 values (arrays of 10 bytes to 1 KB) out of its
verdict and reports only how many it left out; `table` shows them. Three arms on 10-value pools are
among the [seven that spread more than 5%](benchmark-method.md#measurement-noise) in the published
runs.

Below five runs `bench.py` records a spread of 0.0. That 0.0 stands for a spread it couldn't
measure, because the quartiles of so few values would still depend on the lowest and the highest.
`diff` then relies on its threshold alone.

Snapshots made with different `--repeat` counts or with `RAPIDPROTO_BENCH_ONLY` set are refused, as
is a new snapshot that lacks one of the old one's arms. Where both snapshots carry the compile-cost
sweep, `diff` lets `.text` grow by 5%, peak memory by 10% and seconds by 20% before an entry fails.

Before you believe a surprising result, compare a commit with itself
(`python3 tests/bench.py experiment <rev> <rev>`) to see how far identical code moves on your
machine. `experiment` won't start while `git status` lists any change, untracked files included, or
with `RAPIDPROTO_BENCH_ONLY` set.

## Environment variables

The programs read these variables with or without `bench.py`:

| Variable | Effect |
|---|---|
| `RAPIDPROTO_BENCH_JSON=1` | print one NDJSON record per arm; `bench.py` sets it |
| `RAPIDPROTO_BENCH_ONLY=<text>` | run only scenarios whose name contains `<text>` |
| `RAPIDPROTO_BENCH_EVENT=<event>` | count one more hardware event per arm over its timed calls only, printed as `xtra/B`: `l1d-miss`, `llc-miss`, `dtlb-miss` or `branch-miss` |
| `RAPIDPROTO_BENCH_CPU=<n>` | pin to core `<n>` if the process may run there; under `bench.py`'s `taskset`, only that core qualifies |
| `RAPIDPROTO_BENCH_NO_PIN` | don't pin |
| `RAPIDPROTO_BENCH_DUMP_DIR=<dir>` | write every payload to `<dir>`, about 2 GB for both programs, and exit |
| `RAPIDPROTO_BENCH_DIR=<dir>` | read the payloads from `<dir>` instead of building them |

## The placement calibration

The figures under [Code placement](benchmark-method.md#code-placement) come from relinking one
build's object files in shuffled orders. To repeat that, you need lld and a build directory made
with CMake's Makefile generator, the only generator that writes `link.txt`. For each seed, add
`-fuse-ld=lld -Wl,--shuffle-sections=.text*=<seed>` and a fresh `-o` output name to the link command
in `link.txt` and run it. Time each relinked binary with `RAPIDPROTO_BENCH_JSON=1` and
`RAPIDPROTO_BENCH_ONLY=<pattern>` on a [quiesced machine](benchmark-method.md#runs). An arm's
placement range is (max − min) / median of its GB/s across the seeds.
