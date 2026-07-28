# Benchmarks

*The single home for the headline numbers, and how to reproduce them. Back to the
[README](../README.md); the measurement methodology (placement noise, same-binary A/B, GB/s vs ins/B)
is in [architecture.md](../architecture.md#decoder-performance).*

The numbers below come from the in-repo harness (`tests/bench.py`), decoding a realistic `Dataset`
payload — 2000 mixed records with strings, nested and repeated messages, and packed scalar arrays —
that `protoc` serializes and every decoder then parses. Built `-O3 -DNDEBUG` and measured on **g++-13**
and **clang++-20** against **protobuf 4.25.3**. Reproduce with `python3 tests/bench.py run` (see
[Reproducing](#reproducing)). The multipliers are decode **throughput** (GB/s)
ratios — the number that matters, since a schema-specialized decoder wins as much from branch prediction,
pipelining, and superscalar execution as from fewer instructions. Every decoder is measured in the same
run under identical conditions, so the ratios are stable; treat them as a box-specific rule of thumb, not
a cross-machine constant.

## Arena vs `protoc` + `google::protobuf::Arena`

Both materialize a full object tree, so this is a
like-for-like comparison of decode speed and peak arena memory. RapidProto **borrows** strings, bytes,
and `raw` payloads as views into the input instead of copying them, so the arena holds only tree
structure — which is where most of the memory win comes from:

| Metric | RapidProto arena | protoc + Arena |
|---|---|---|
| Decode throughput | **~5× faster** | baseline |
| Peak memory, payload (arena `bytes_used` vs protoc `SpaceUsed`) | **0.49×** | 1× |
| Peak memory, total held (arena `bytes_reserved` vs protoc `SpaceAllocated`) | **0.56×** | 1× |

That's the g++-13 figure; clang++-20 measures ~6×. The decoded tree borrows the input, so it stays valid
only while both the input and the `Arena` outlive it (or use
[`decode_owned`](arena.md#self-contained-decode-decode_owned) for a self-contained handle).

## Streaming vs `protozero`

Both are zero-materialization pull parsers. On the realistic `Dataset` the
streaming decoder is **~2× faster than protozero** — and **~13× faster than `protoc` + `Arena`**, since
it materializes nothing. Across the per-field microbenchmarks it's faster on most shapes (repeated
fields, nested messages, skip-heavy records), about even on single fixed-width scalars, and slower only
on large **packed** arrays, which it decodes one element per callback — decode those with the arena model
(below).

## Arena vs streaming (the two RapidProto models)

The streaming decoder is **~2.7× faster** than the
arena decoder on the `Dataset`, since it builds no object tree. Use the arena model when you want a
navigable, random-access object; stream when you only extract or forward fields.

**Large packed scalar arrays: prefer the arena model.** The arena decoder decodes a packed varint array
a word at a time straight into its array, while the streaming decoder decodes it one element per callback
(a streaming callback takes a single value and can't batch its store). So if your hot path is dominated
by large packed scalar fields, the **arena** decoder is the faster choice.

Speedups vary with payload shape, and part of the arena/protoc gap is a feature gap — protoc validates
UTF-8 on every proto3 string and RapidProto does not. The harness ships every scenario (and a memory
report), so measure your own payloads rather than trusting one ratio as universal.

## Reproducing

The benches need `libprotobuf-dev` (+ a matching `protoc`) and `protozero` installed;
`rapidproto_arena_bench` is built only when protobuf is found. `tests/bench.py` drives everything:

```sh
python3 tests/bench.py run                      # build both benches, run them pinned, write a snapshot
python3 tests/bench.py table SNAPSHOT [...]     # render one snapshot, or compare several
python3 tests/bench.py diff OLD NEW             # GB/s regression check (exit 1 past the threshold)
python3 tests/bench.py experiment BASE [VAR]    # build+snapshot two git refs, then diff them
```

Two environment variables help when investigating one arm rather than gating a change — both are
read by the bench binaries directly, so they work with or without `bench.py`:

```sh
RAPIDPROTO_BENCH_ONLY="rv fx1 1M"    # run only scenarios whose name contains this (~7s, not ~100s)
RAPIDPROTO_BENCH_EVENT=llc-miss      # add one PMU counter, reported per arm as xtra/B
                                     # (l1d-miss | llc-miss | dtlb-miss | branch-miss)
```

The counter is measured over exactly the region the harness times, which is what makes it usable:
`perf stat` counts the whole process, so a per-arm effect is diluted away (process-level cycles moved
1.6% across runs whose *arm* throughput moved 19%).

A snapshot is NDJSON tagged with the compiler, protobuf version, and git revision, so a number is
never separated from what it was measured against. GB/s is the primary signal; cyc/B and ins/B are
kept as diagnostics.

### Quiescing the box (do this first)

These settings are **not persistent** — re-apply after every reboot. `bench.py` checks them and prints
the exact commands if any is wrong, so you do not have to remember; run it and read the warning.

```sh
sudo sh -c 'echo off > /sys/devices/system/cpu/smt/control'          # biggest single effect
sudo sh -c 'echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo'  # fixed clock
sudo sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > $g; done'
sudo sysctl -w kernel.perf_event_paranoid=1                          # enables cyc/B and ins/B
```

Why each matters, measured on this bench with the *identical binary* run five times (spread = per-arm
max-vs-min over the runs):

| | median spread | arms over 5% |
|---|---|---|
| as-found (`powersave`, turbo on, SMT on) | 1.0% | 21 of 259 |
| + turbo off, `performance` governor | 0.6% | 14 |
| + SMT off | 0.7% | 13 |

**SMT is the one that matters most.** The pinned core's sibling may or may not receive unrelated work
during a run, which is a per-run coin flip: holding the sibling deliberately busy (a constant state)
cut arms-over-5% from 14 to 2. Leaving SMT on is what made several arms look *bimodal* — they are not,
the samples were just drawn from two different machine states.

### Where the residual noise comes from

Even on a quiesced box a few arms still move ±9% run to run. Diagnosed with per-arm counters
(`RAPIDPROTO_BENCH_EVENT=llc-miss`, measured over exactly the timed region — `perf stat` dilutes this
beyond recognition because it counts the whole process):

| | run-to-run spread |
|---|---|
| retired instructions | **0.0%** — identical work every run |
| branch misses | 0.1% |
| dTLB misses | 7% (uncorrelated, r = 0.28) |
| **LLC misses** | **100%**, and correlated with cycles at **r = 0.94** |

The chain is: identical instructions → variable L3 residency → variable misses → variable cycles.
Confirmed by making L3 pressure *constant* (streaming loads on other cores): miss rate rises 4.5× as
expected, but its spread collapses from 100% to 14% and GB/s spread from 14% to 5%.

The L3 is 24 MiB shared by every core, so ambient activity anywhere evicts our lines. The noisy arms
are the ones whose working set sits in the **partially resident** band — big enough to be evictable,
small enough that residency matters. Arms well below it (everything stays hot) and well above it
(everything misses anyway) are both quiet: `rv fx10 1M` at 10 MB measures 0.8%, while `rv fx1 1M` at
1 MB measures 11.8%.

Two hypotheses were tested and **refuted**, recorded so they are not re-tried: physical page colouring
(transparent huge pages made spread *worse*, 14% → 23%, not better) and branch-predictor aliasing
(branch misses are flat at 0.1%). ASLR was also re-checked and makes no difference.

Deliberately loading the machine would stabilise the numbers, but it costs ~40% throughput and
measures something else. The supported answer is: quiesce, repeat, and gate per arm — below.

### Why `--repeat` exists

One run of an arm is not a measurement. Even on a quiesced box a few arms have a run-level spread near
±9%, so at `--repeat 1` two snapshots of **identical code** differ by up to ~14% (p95) — above any
threshold worth gating on, which is exactly how false "regressions" arise. p95 of that identical-code
delta falls to 9.2% at 3 runs, 7.7% at 5, 6.1% at 7; `bench.py run` defaults to 5.

Each arm keeps its **best** run (interference only ever subtracts throughput, so the fastest run is the
least-disturbed estimate), and the spread across runs is recorded per arm. `diff` then gates each arm
against **its own** measured spread as well as the flat threshold, and prints that spread in a `noise`
column. An arm that moved less than its own demonstrated noise is reported but never failed on — and an
arm whose noise is large genuinely cannot resolve a change that size, which the column makes visible
rather than hiding.

Cross-build GB/s additionally carries code-placement noise — read
[architecture.md](../architecture.md#decoder-performance) before comparing numbers across builds or
machines.

## Choosing the protoc baseline

The arena bench's `protoc` arm is whatever `find_package(Protobuf)` resolves, and libprotobuf's own
decoder has sped up markedly across releases (3.21 → 25.3 measures ~10–40% fewer cycles/byte on these
shapes), so an old baseline flatters the arena. To benchmark against a specific version, build it (plus
**Abseil**, required by protobuf 22+) into a local prefix — nothing is committed to the tree — and
point CMake at it:

```sh
git clone --depth 1 --recurse-submodules -b v25.3 https://github.com/protocolbuffers/protobuf
cmake -S protobuf -B protobuf/_b -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF \
      -Dprotobuf_ABSL_PROVIDER=module -DCMAKE_INSTALL_PREFIX="$PWD/pb-25"
cmake --build protobuf/_b -j && cmake --install protobuf/_b
cmake --preset gcc -DCMAKE_PREFIX_PATH="$PWD/pb-25"    # find_package(Protobuf CONFIG) picks it up
```

The bench CMake prefers the protobuf **CONFIG** package (whose `protobuf::libprotobuf` target carries
the Abseil link deps 22+ needs) and falls back to the **FindProtobuf module** for a system 3.x install;
`protoc` and `libprotobuf` come matched from the same prefix.
