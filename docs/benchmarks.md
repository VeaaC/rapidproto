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
`rapidproto_arena_bench` is built only when protobuf is found.

**Quiesce the box first.** `tests/bench_box.sh setup` disables SMT and turbo, sets the
`performance` governor, and enables the hardware counters — saving whatever was there before, so
`tests/bench_box.sh restore` puts your machine back. None of it survives a reboot. `bench.py` checks
the same settings on every run and prints the exact fix if any is off, so you do not have to
remember; `tests/bench_box.sh status` shows the current values.

`bench.py` defaults its build dir to `build/gcc-pb25`; pass `--build-dir build/gcc` to use the
preset from [CONTRIBUTING.md](../CONTRIBUTING.md), or point it at whichever prefix you built protobuf
into (see [Choosing the protoc baseline](#choosing-the-protoc-baseline)).

```sh
python3 tests/bench.py run --build-dir build/gcc # build both benches, run them pinned, write a snapshot
python3 tests/bench.py table SNAPSHOT [...]     # render one snapshot, or compare several
python3 tests/bench.py diff OLD NEW             # GB/s regression check (exit 1 past the threshold)
python3 tests/bench.py experiment BASE [VAR]    # build+snapshot two git refs, then diff them
```

Four things to know before acting on a number:

- **`run` executes each bench `--repeat` times (default 5)** and keeps each arm's best run, because
  one run is not reproducible across process launches — see the appendix. Budget roughly five times
  a single run; `experiment` builds and measures two revisions, so about ten.
- **Both snapshots must use the same `--repeat`.** Best-of-K rises with K, so mixing them biases the
  comparison; `diff` refuses rather than printing a number you should not act on.
- **Read the `noise` column.** `diff` prints each arm's own run-to-run spread and will not fail an
  arm on a delta smaller than it. An arm with large noise cannot resolve a change that size — that
  is information, not a pass.
- **Sanity-check with a self-comparison** (`experiment <rev> <rev>`) before believing a surprising
  result. It should pass; if it does not, the box is not quiet.

A snapshot is NDJSON tagged with the compiler, protobuf version, and git revision, so a number is
never separated from what it was measured against, and it carries every run's GB/s so it can be
re-analysed without re-measuring. GB/s is the primary signal; cyc/B and ins/B are diagnostics.

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

Why `--repeat`, the `noise` column and the quiesce step exist. Numbers below were measured on one
Linux box with a hybrid Intel CPU; treat the magnitudes as illustrative and the *method* as the
transferable part.

**Quiescing.** Same binary, five runs on an otherwise idle box, over the arena bench's 259 gated
arms. "Spread" here is (max − min) / min per arm — note this is *not* the `spread_pct` the gate uses,
defined further down:

| | median spread | arms over 5% |
|---|---|---|
| as found (`powersave`, turbo on, SMT on) | 1.0% | 21 |
| turbo off + `performance` governor | 0.6% | 14 |
| + SMT off | 0.7% | 13 |

Frequency is the change that measurably helps the summary. Disabling SMT does not move these counts
further, but it removes a per-run coin flip — whether the scheduler puts work on the pinned core's
sibling — that makes affected arms look bimodal when sampled a few times. A fixed sibling state is
worth having even where the aggregate barely moves. (Pinning a *constant* load on the sibling cut
arms-over-5% to 2, but that also loads the shared cache, so it is not evidence about SMT alone.)

Each cell is one five-run estimate and the second column is dominated by whatever else the machine
was doing: repeating the bottom row with a single background process alive moved arms-over-5% from 13
to 84. Read the column as an ordering under one ambient-load condition, not a number to reproduce —
and note that "was anything else running" matters more than any of the three knobs.

**What the residual is.** Per-arm counters (`RAPIDPROTO_BENCH_EVENT`), 12 runs of the least stable
arm, `rv fx1 1M` / `arena-warm`:

| | run-to-run spread | correlation with cycles |
|---|---|---|
| retired instructions | 0.0% | — (identical every run) |
| branch misses | 0.1% | — |
| dTLB misses | 7% | 0.28 (n=12; too few to distinguish from zero) |
| **LLC misses** | **100%** | **0.94** |

Identical work, variable stalls, and the stalls track last-level-cache misses. The LLC is shared by
every core, so ambient activity anywhere evicts our lines. The affected arms are those whose live
footprint is *comparable to* the LLC — evictable, but small enough that residency matters. Both
sweeps rotate a pool of buffers, so the live set is much larger than one buffer, and it is
arm-specific. `rv fx1 1M` / `arena-warm` holds 8 input buffers of ~1 MB plus ~8 MB of decoded arena —
comparable to a typical LLC — and is the least stable arm in the suite: its run-to-run spread ranges
from ~10% to occasional 3x outliers depending on ambient load. The same scenario's `protozero` arm
materializes nothing, touches only the 8 MB pool, and holds under 1%. `rv fx10 1M` holds ~80 MB,
misses regardless, and measures 0.8%. Read your own LLC with `getconf LEVEL3_CACHE_SIZE`.

An intervention is *consistent with* that reading: streaming loads on other cores collapse the
miss-rate spread from 100% to 14% and GB/s spread from 14% to 5%. It is not proof — it also costs
~40% throughput, so the arm may simply be bandwidth-clamped there, and it changes core occupancy and
uncore clock at the same time. The control that would separate those (a cache-resident spinner on the
same cores) has not been run.

**Tried and rejected as fixes:**

- *Transparent huge pages* made it worse (spread 14% → 23%). THP also adds compaction as its own
  noise source and relocates the cache mapping rather than removing variance, so this rules THP out
  as a remedy without ruling physical page colouring in or out. A direct test — an identical
  pre-faulted mapping reused across runs — has not been done.
- *Loading the machine deliberately* stabilises the numbers but costs ~40% throughput and measures a
  different operating point.

Ruled out by measurement: ASLR (`setarch -R`, no effect over 20 runs per condition), and drift across
a session (whole-run level, i.e. the median of per-arm ratios against the first run, stays 1.000
±0.01 across every run of every batch). Ruled out by construction: payload variation — the generators
are fixed-seed, and every arm's checksum is cross-checked against the baseline each round.

**Why 5 runs.** Bootstrapped from 40 repeated runs of the worst arm, the p95 apparent delta between
two snapshots of identical code falls from ~14% at 1 run to ~9% at 3, ~8% at 5 and ~6% at 7 — about
0.8pp per added run beyond 3, with no knee. 5 is a cost/benefit pick, taken on the worst arm so it
over-samples the quiet ones.

`spread_pct` — the noise the gate uses, and a different statistic from the table above — is how far
the kept value sits above the second-best run, (max − runner-up) / runner-up. Not the full range and
not the median: the kept value is the max, so disturbed runs are what best-of discards and must not
widen the gate, and a median still lands among them once they are the majority (3 of 5 is ordinary
for an arm that reads bimodal). It needs at least 3 runs, and it is still a small-sample estimate —
expect it to move between snapshots. A high outlier is the one case it cannot help with: best-of
records that outlier as the value *and* reports the gap as noise, so the arm goes un-gated until the
next snapshot. Every run's GB/s is kept in the snapshot (`gb_s_runs`) so that is auditable.

**Investigating one arm.** Both variables are read by the bench binaries directly, so they work with
or without `bench.py`:

```sh
RAPIDPROTO_BENCH_ONLY="rv fx1 1M"    # only scenarios whose name contains this, and skips the other
                                     # rows' payload builds too -- under 1s for the arena bench
                                     # against tens of seconds unfiltered
RAPIDPROTO_BENCH_EVENT=llc-miss      # one extra PMU counter, per arm, as xtra/B
                                     # (l1d-miss | llc-miss | dtlb-miss | branch-miss)
```

The counter is scoped to exactly the region the harness times. That is what makes it usable:
`perf stat` counts the whole process, where a per-arm effect is diluted away — process-level cycles
moved 1.6% across runs whose *arm* throughput moved 19%.
