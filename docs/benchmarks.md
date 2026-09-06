# Benchmarks

*The single home for the headline numbers, and how to reproduce them. Back to the
[README](../README.md); the measurement methodology (placement noise, same-binary A/B, GB/s vs ins/B)
is in [architecture.md](../architecture.md#decoder-performance).*

The numbers below come from the in-repo harness (`tests/bench.py`), decoding a realistic `Dataset`
payload — 2000 mixed records with strings, nested and repeated messages, and packed scalar arrays —
that `protoc` serializes and every decoder then parses. Built `-O3 -DNDEBUG` and measured on **g++-13**
and **clang++-20** against **protobuf 4.25.3**. Reproduce with
`python3 tests/bench.py run --build-dir build/gcc`
(see [Reproducing](#reproducing)). The multipliers are decode **throughput** (GB/s)
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
`rapidproto_arena_bench` is built only when protobuf is found — and both bench targets
exist only on Linux (`tests/bench_harness.hpp` is built on `perf_event` self-monitoring
and refuses other platforms).

**Quiesce the box first.** `tests/bench_box.sh setup` disables SMT and turbo, sets the
`performance` governor, and enables the hardware counters — saving whatever was there before, so
`tests/bench_box.sh restore` puts your machine back. None of it survives a reboot. `bench.py` checks
the same settings on every run and prints the exact fix if any is off; `tests/bench_box.sh status`
shows the current values.

`--build-dir` defaults to `build/gcc-pb25`, which no preset creates — pass `--build-dir build/gcc`
for the `gcc` preset from [CONTRIBUTING.md](../CONTRIBUTING.md), or the build dir you configured
against a specific protobuf (see [Choosing the protoc baseline](#choosing-the-protoc-baseline)).
Pinning defaults to `--core 2`; on a hybrid CPU make sure that is a performance core.

```sh
python3 tests/bench.py run --build-dir build/gcc  # build both, run pinned, write a snapshot
python3 tests/bench.py table SNAPSHOT [...]       # render one snapshot, or compare several
python3 tests/bench.py diff OLD NEW               # regression check (see the gate rule below)
python3 tests/bench.py experiment BASE [VAR]      # snapshot two git refs, then diff them
```

Four things to know before acting on a number:

- **`run` executes each bench `--repeat` times (default 5)** and keeps the median run of each *arm*
  (one decoder variant within one scenario), because
  one run is not reproducible across process launches — see the appendix. Budget roughly five times
  a single run; `experiment` builds and measures two revisions, so about ten.
- **Both snapshots must use the same `--repeat`.** The median's sampling variance falls with K, and
  at even K the harness keeps the upper of the two middle runs — so a mixed-K pair compares two
  differently-behaved estimators. `diff` refuses, as it does for a snapshot written under
  `RAPIDPROTO_BENCH_ONLY`, which covers only part of the suite.
- **Read the `noise` column.** `diff` prints each arm's own `spread_pct` and will not fail an
  arm on a delta smaller than it. An arm with large noise cannot resolve a change that size — that
  is information, not a pass.
- **Sanity-check with a self-comparison** (`experiment <rev> <rev>`) before believing a surprising
  result — it needs a clean working tree. On a quiesced box it has passed here; if it does
  not, treat
  the box, not the code, as the first suspect.

A snapshot is NDJSON tagged with the compiler, protobuf version, and git revision, so a number is
never separated from what it was measured against, and it carries every run's GB/s so it can be
re-analysed without re-measuring. GB/s is the primary signal; cyc/B and ins/B are diagnostics.

## Compile cost — what the throughput costs to build

Decode speed is half of what a code generator costs its user; the other half is **compile time,
`.text` size, and the compiler's peak RSS** for the generated decoders. Every `bench.py run` and
`experiment` embeds that sweep into the snapshot (a ~2-minute ride-along; `--no-compile` skips
it), measured by `tests/compile_bench.py`'s machinery — six schema shapes from a one-message
baseline through a 10-deep nesting chain to `descriptor.proto`, each compiled per model per
compiler as one TU with an external-linkage function per message (see that file's docstring for
the methodology and its caveats). `table` renders the columns beside the throughput tables, and
`diff`/`experiment` **gate** them at a tight threshold: unlike GB/s these numbers are
near-deterministic, with no code-placement floor to hide behind, so a codegen change that bloats
`.text` fails the same experiment that would previously have reported only its (possibly
invisible) speed effect. For magnitudes: an arena 10-message nesting chain costs ~4.7s and 174 KB
of `.text` on gcc-13 against ~1.1s and 48 KB on clang-20 — build cost is strongly
compiler-dependent, which is why the table always shows both. `tests/compile_bench.py` remains
usable standalone (same `run`/`table`/`diff` shape) for compile-only investigation.

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

## Appendix: measurement noise

Why `--repeat`, the `noise` column and the quiesce step exist. Measured on one quiesced Linux box;
treat the magnitudes as illustrative and the *method* as the transferable part. "Range" below means
(max − min) / min across an arm's runs, used only to describe the tables; the gate's own
statistic is
`spread_pct`, defined further down.

**Quiescing.** Same binary, five runs on an otherwise idle box, over the arena bench's then-259
gated arms:

| | median range | arms over 5% |
|---|---|---|
| as found (`powersave`, turbo on, SMT on) | 1.0% | 21 |
| turbo off + `performance` governor | 0.6% | 14 |
| + SMT off | 0.7% | 13 |

Frequency is the change that moves the summary. Disabling SMT does not, but it removes a
per-run coin
flip — whether the scheduler puts work on the pinned core's sibling — that makes affected arms look
bimodal when sampled a few times, so a fixed sibling state is worth having anyway. The arms-over-5%
column is a sensitivity, not a constant: repeating the bottom row with a single background process
alive moved it from 13 to 84.

**What the residual is.** Per-arm counters (`RAPIDPROTO_BENCH_EVENT`), 12 runs *per counter* — the
harness opens one extra event per process, so each row is its own set of runs — on the least stable
arm, `rv fx1 1M` / `arena-warm`:

| | run-to-run range | correlation with cycles |
|---|---|---|
| retired instructions | 0.0% | — (identical every run) |
| branch misses | 0.1% | — |
| dTLB misses | 7% | 0.28 (n=12; too few to distinguish from zero) |
| **LLC misses** | **100%** | **0.94** |

Identical work, variable stalls, and the stalls track last-level-cache misses. The LLC is shared by
every core, so ambient activity anywhere evicts our lines. The affected arms appear to be
those whose
live footprint is *comparable to* the LLC — evictable, but small enough that residency matters. Both
sweeps rotate a pool of buffers, so the live set is much larger than one buffer and is arm-specific:
`rv fx1 1M` / `arena-warm` holds 8 input buffers of ~1 MB plus ~8 MB of decoded arena, and is the
least stable arm in the suite at ~14% on a quiesced box, with 3× outliers under load. The same
scenario's `protozero` arm materializes nothing, touches only the 8 MB pool, and stays under 1%.
`rv fx10 1M` holds ~80 MB, misses regardless, and measures 0.8%. Read your own LLC with
`getconf LEVEL3_CACHE_SIZE`.

An intervention is *consistent with* that reading: streaming loads on other cores collapse the LLC
miss-rate range from 100% to 14% and the GB/s range from 14% to 5%. It is not proof — it also costs
~40% throughput, so the arm may simply be bandwidth-clamped there, and it changes core occupancy and
uncore clock at the same time. The control that would separate those (a cache-resident
spinner on the
same cores) has not been run. It is also disqualified as a remedy, since it measures a different
operating point.

**Tried and rejected as a fix:** transparent huge pages made it worse (range 14% → 23%). THP adds
compaction as its own noise source and relocates the cache mapping rather than removing variance, so
this rules THP out as a remedy without ruling physical page colouring in or out; the direct
test — an
identical pre-faulted mapping reused across runs — has not been done.

Ruled out by measurement: ASLR (`setarch -R`, no effect over 20 runs per condition), and
drift across
a session (the median of per-arm ratios against the first run stays 1.000 ±0.01 across every run of
every batch). Ruled out by construction: payload variation, since the generators are fixed-seed and
every run decodes the same bytes.

**The gate's noise statistic.** Each arm records the median of its `--repeat` runs, and `spread_pct`
— printed as the `noise` column — is that run set's interquartile range relative to the median. What
matters is how much the middle of the distribution moves, not how far the extremes reach, and an IQR
ignores one outlier in either direction. Both choices are measurable rather than assumed: re-derived
from one pair of 5-run snapshots of the same binary (485 gated arms across both benches as then
measured -- the suite has grown since; the arm counts in a live `bench.py diff` reflect it -- via the
stored `gb_s_runs`), the largest apparent change on unchanged code is **−3.7%**, where keeping each
arm's *fastest* run instead gives **−46.9%** — the fastest run records an upward fluke the next
snapshot has no reason to repeat.

**Why 5 runs.** `statistics.quantiles` interpolates, so the IQR is outlier-robust only from K=5; at
K=3 it is exactly half the full range. Below 5 an arm reports no noise and `diff` gates on the flat
threshold, saying so. Repeats also keep paying: bootstrapped from 40 runs of the worst arm, the p95
apparent delta between two snapshots of identical code falls ~14% → ~9% → ~8% → ~6% at 1, 3, 5 and 7
runs, with no knee. 5 is the cheapest K the robustness requirement allows.

**Investigating one arm.** Both variables are read by the bench binaries directly, so they work with
or without `bench.py`:

```sh
RAPIDPROTO_BENCH_ONLY="rv fx1 1M"    # only scenarios whose name contains this; skips the other
                                     # scenarios' payload builds too (under 1s for the arena bench
                                     # against tens of seconds unfiltered)
RAPIDPROTO_BENCH_EVENT=llc-miss      # one extra PMU counter, per arm, as xtra/B
                                     # (l1d-miss | llc-miss | dtlb-miss | branch-miss)
```

The counter is scoped to exactly the region the harness times. That is what makes it usable: `perf
stat` counts the whole process, where a per-arm effect is diluted away.
