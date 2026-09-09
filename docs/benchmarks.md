# Benchmarks

*The single home for the headline numbers, and how to reproduce them. Back to the
[README](../README.md); the measurement methodology (placement noise, same-binary A/B, GB/s vs ins/B)
is in [architecture.md](https://github.com/VeaaC/rapidproto/blob/main/architecture.md#decoder-performance).*

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

## The upb arm — validating the C-parser baseline

The arena bench also measures **upb** (protobuf's C parser, the engine under the Python/Ruby/PHP
protobuf runtimes) on the same `Dataset`, cross-checked against every other decoder's checksum.
The arm decodes through a **runtime-built MiniTable** (from an embedded descriptor, with upb's
fasttable enabled, and string ALIASING on — the same borrow-from-the-input semantics our arena
uses, worth +5-11% to upb in pure-decode terms and ~1-4% on the walked table rows below)
because upb's plugin-generated tables would require building protobuf's compiler from source.
The decode configuration is **validated, not assumed**: a standalone decode-vs-decode probe (2M
back-to-back iterations per arm, no checksum walks, aliasing off — a strictly conservative
variant of the configuration, since aliasing only helps upb) measures it at **2.16× protoc** on
`google_message1` — the band upstream's own figures put upb in — so the decoder itself runs at
full strength.

The in-tree rows read very differently, and deliberately so: **every arm's timed lambda decodes
AND reads every present field** (the checksum walk — the work a real consumer does), and upb
pays that walk through the same pre-resolved `upb_MiniTableField` accessors its generated code
compiles to. Reading data OUT of a upb message costs measurably more than reading our arena's
structs (per-field hasbit checks and offset indirection; upb's ins/B rises ~57-60% when the walk
joins the timed loop), which is where most of the probe-vs-table gap goes. Maps never take
upb's fast path, so the map-heavy `Dataset` is its weakest shape. upb's sources are fetched at
the corpus's protobuf pin (`tests/fetch_corpus.py`), never vendored; without the corpus the arm
is skipped and says so.

## The published datasets — google_message1 and google_message2

Protobuf's own cross-language benchmark payloads (anonymized real production shapes, fetched at
a pinned tag by `tests/fetch_corpus.py`), decoded by every arm and cross-checked on one
checksum, so these numbers are comparable with figures third parties already publish. Measured
like the tables above (g++-13, protobuf 4.25.3, quiesced box), throughput vs the protoc
baseline:

Every arm decodes **and reads every present field** (one shared checksum, cross-validated):

| | google_message1 (228 B) | google_message2 (84.5 KB, group-heavy) |
|---|---|---|
| streaming | **+121%** | **+143%** |
| arena (warm) | **+95%** | **+29%** |
| arena (cold) | +41% | +16% |
| upb | +16% | +17% |

Two honest readings: `google_message2` is proto2's home turf — one huge repeated *group* of
small mixed fields — and it is where our arena's lead over both protoc and upb is smallest (upb
ties the cold-arena row there), while the streaming decoder leads every arm on both — by a wide
margin on `google_message2`.
And `google_message1` at 228 bytes shows the cold-arena setup cost that the warm row amortizes;
a consumer decoding many small messages should reuse the arena. (Read coarse ratios, not
decimals: small-payload rows swing a few points between runs — short rotated batches magnify
per-iteration overheads — and whenever the bench binary itself changes, EVERY row can shift by
the ~10% cross-build placement floor the methodology notes document. Compare within one table,
not across published revisions of it.)

## Real-world data — OSM PBF vs libosmium

OpenStreetMap's planet format ([walkthrough](osm-pbf.md)), on a pinned, sha256-verified
Geofabrik extract (`tests/fetch_osm_dataset.py`; Bremen, ~20 MB — 1.6 M nodes, 320 K ways,
full metadata). The baseline is **libosmium**, the standard C++ OSM library, compiled from a
corpus pin. Every arm computes one cross-validated checksum covering ids, coordinates,
metadata, resolved way refs and relation members, and per-tag key/value/role stringtable
accesses; `tests/bench_arm_osm.hpp` documents the convention and the reuse rules (protoc
keeps one message across blocks — its standard repeated-parse idiom, and its fastest).
Measured like the tables above (g++-13, protobuf 4.25.3, libosmium 2.23.1, quiesced box).

PBF blobs are zlib-deflated and inflate rivals the protobuf work itself, so the comparison is
split in two:

**The protobuf layer alone** (every block pre-inflated once, untimed; MB/s of inflated
payload):

| arm | MB/s | vs protoc |
|---|---|---|
| protoc | 273 | baseline |
| arena (cold) | 443 | +62% |
| arena (warm) | 447 | **+63%** |
| streaming | 432 | **+58%** |

**End to end** (raw file bytes in memory → framing, blob decode, inflate, block decode, walk;
MB/s of file bytes):

| arm | MB/s | vs arena |
|---|---|---|
| arena (warm) | 57 | baseline |
| streaming | 56 | −0.3% (a wash) |
| libosmium | 21 | **−63%** |

Caveats that matter when reading the tables. The end-to-end table is mostly a zlib table:
the same arena decode that runs at 447 MB/s over inflated payload delivers ~128 MB/s of it
end-to-end (a ~3.5× framing + inflate tax), and arena-vs-streaming collapses to a wash — a
PBF number that doesn't isolate inflate mostly benchmarks the compressor. The libosmium
comparison is single-core: the harness pins the process to one core before libosmium's
thread pool is created, so its reader threads (a real strength on unpinned machines) share
that core, and the scenario is compared on wall time (per-thread cycle counters can't see
its workers, so those columns are not reported). libosmium always materializes full OSM
objects — its API offers nothing else — which makes the arena arm the like-for-like
materializing comparison; it reads the same data ~2.7× faster on that one core. Finally,
cold and warm arena rows nearly coincide: at ~187 KB per block, arena construction is noise,
so the lead over a message-reusing protoc is not an allocator artifact.

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

The benches need `libprotobuf-dev` (+ a matching `protoc`) and `protozero` installed —
though when the corpus is fetched, the protozero arm compiles against the corpus **pin**
(v1.8.2) rather than the system install, so the measured version is recorded;
`rapidproto_arena_bench` is built only when protobuf is found — and both bench targets
exist only on Linux (`tests/bench_harness.hpp` is built on `perf_event` self-monitoring
and refuses other platforms). The corpus-dependent scenarios (upb, google_message1/2, the
OSM PBF pair, the large-schema arm) additionally want the fetched corpus and dataset:

```sh
python3 tests/fetch_corpus.py       # pinned schemas + the upb/libosmium/protozero sources
python3 tests/fetch_osm_dataset.py  # the pinned Geofabrik extract (sha256-verified)
```

**Quiesce the box first.** `tests/bench_box.sh setup` disables SMT and turbo, sets the
`performance` governor, and enables the hardware counters — saving whatever was there before, so
`tests/bench_box.sh restore` puts your machine back. None of it survives a reboot. `bench.py` checks
the same settings on every run and prints the exact fix if any is off; `tests/bench_box.sh status`
shows the current values.

`--build-dir` defaults to `build/gcc-pb25`, which no preset creates — pass `--build-dir build/gcc`
for the `gcc` preset from [CONTRIBUTING.md](https://github.com/VeaaC/rapidproto/blob/main/CONTRIBUTING.md), or the build dir you configured
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
  a single run (plus ~2 min under
  [`--compile`](#compile-cost--what-the-throughput-costs-to-build));
  `experiment` builds and measures two revisions, so about double all of it.
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
`.text` size, and the compiler's peak RSS** for the generated decoders. `bench.py run` and
`experiment` embed that sweep into the snapshot with **`--compile`** (opt-in — a ~2-minute
ride-along per snapshot that a throughput-focused loop should not pay; pass it when the change
under test touches codegen), measured by `tests/compile_bench.py`'s machinery — six schema
shapes from single-message schemas through a 10-deep nesting chain, `descriptor.proto`, and a
103k-line generated compute schema, each compiled per model per compiler as one TU with an external-linkage function per
message (see that file's docstring for the methodology and its caveats). `table` renders the
columns beside the throughput tables (compiler launchers are neutralized: every measured compile
sets `CCACHE_DISABLE`, and a compiler resolving to an sccache/distcc/icecc shim is refused up
front), and `diff`/`experiment` **gate** all three metrics, each at its own threshold: `.text`
5% — deterministic, byte-stable across independent sweeps — peak RSS 10%, and wall-clock
*seconds* 20%, the one metric that is load-sensitive like any timing. A codegen change that
bloats `.text` therefore fails the same experiment that would previously have reported only its
(possibly invisible) speed effect. For magnitudes: an arena 10-message nesting chain costs
~7.2s and 174 KB
of `.text` on gcc-13 against ~1.7s and 48 KB on clang-20 — build cost is strongly
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
