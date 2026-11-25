# Benchmark method

The figures on [Benchmarks](benchmarks.md) come from benchmark programs in this repository. Two C++
programs time several decoders side by side in one process, and
[`rapidproto-bench`](https://github.com/VeaaC/rapidproto/tree/main/rust/rapidproto-bench) does the
same for the Rust decoders. The compile-cost table comes from a script that times the compiler. To
build and run the benchmarks yourself, or check a change against the code before it, see
[Running the benchmarks](running-benchmarks.md).

The decoders on a C++ payload take turns, round after round, each making the same number of decodes
per turn, until the harness knows how each one compares with the first or a time limit runs out. All
of them have to compute the same checksum over what they decoded. Each program runs five times on
one core of a machine with nothing else running. A decoder's published speed is its median run. The
Rust crate takes turns the same way for a fixed 20 rounds. Where the linker puts a short decode
loop can change its speed by as much as 22% between two builds of the same code. The benchmark
builds align every function and loop to 64 bytes, which in a relink study held that to about 6.5%
on the worst decoders.

## How a number is made

[`tests/bench.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/bench.py) builds and runs
both C++ programs. `rapidproto_arena_bench`
([bench_arena.cpp](https://github.com/VeaaC/rapidproto/blob/main/tests/bench_arena.cpp)) times the
[arena decoder](arena.md) against protoc and the other C++ decoders. `rapidproto_bench`
([bench_streamgen.cpp](https://github.com/VeaaC/rapidproto/blob/main/tests/bench_streamgen.cpp))
times the [streaming decoder](streaming.md) against protozero and a hand-written decode loop. That
loop calls the same low-level field readers as the generated code and checks the input as
thoroughly. Any gap between the two is what the generated code itself costs or gains. Both programs
share the harness in [bench_harness.hpp](https://github.com/VeaaC/rapidproto/blob/main/tests/bench_harness.hpp).

[CMakeLists.txt](https://github.com/VeaaC/rapidproto/blob/main/CMakeLists.txt) compiles both
programs with `-O3 -DNDEBUG -falign-functions=64 -falign-loops=64` whatever the build type. No
`-march` flag is set. The two alignment flags start every function and loop on a 64-byte boundary.
A short decode loop runs faster or slower depending on how it falls across cache lines and into the
µop cache, even with its instructions unchanged. Without the flags any rebuild could move it
([Code placement](#code-placement) has the measurements).

### Scenarios and arms

Each program runs a list of scenarios. A scenario is one payload, or a pool of them, and the decoder
configurations that decode it are its arms. The harness measures every arm against the first one,
arm 0, which the [scenario table](#payloads) names for each scenario.

| Arm | Program | What it times |
|---|---|---|
| `protoc` | arena | protoc's generated C++ code, with a new Arena for each decode (on `osm_blocks`, one reused message) |
| `protoc-warm` | arena | the same code with one reused Arena |
| `arena-warm` | arena | the arena decoder with one arena, reset before each decode |
| `arena-cold` | arena | the arena decoder with a new arena for each decode |
| `streamgen` | arena | the streaming decoder |
| `protozero`, `upb`, `libosmium` | arena | those libraries, each described under [Baselines](#baselines) |
| `generated` | streaming | the streaming decoder |
| `wire` | streaming | the hand-written loop |
| `protozero` | streaming | protozero |

The streaming decoder goes by a different name in each program. The arena program calls it
`streamgen`. In the streaming program, where it races the hand-written loop, it's `generated`.

### The arms behind each table

Each cell of a throughput table on Benchmarks is one arm's median run divided by the baseline arm's.
The two figures of a ratio can come from different runs of the program.

| Table on Benchmarks | Scenarios | Divided by | Rows |
|---|---|---|---|
| `Dataset` | `Dataset` | `protoc-warm` | `arena-warm`, `streamgen`, `protozero`, `upb` |
| Google's benchmark payloads | `google_message1`, `google_message2` | `protoc-warm` | `arena-warm`, `streamgen`, `upb` |
| Inflated blocks | `osm_blocks` | `protoc` | `arena-warm`, `streamgen` |
| Whole file | `osm_file` | `arena-warm` | `streamgen`, `libosmium` |
| Arena or streaming | the `rv` pools | `generated`, from the streaming program | `arena-warm`, from the arena program |
| Other Rust decoders | the Rust crate's payloads | prost | `rapidproto`, the arena decoder with a new arena per decode, and the other crates |
| Rust and C++ | see [Comparing Rust with C++](#comparing-rust-with-c) | the paired C++ arm | the paired Rust arm |

The arena-or-streaming cells are the only C++ ratios whose two halves never ran in the same
process. When Benchmarks compares the streaming decoder with protozero on 102 other payloads, it
uses the streaming program's `generated` and `protozero` arms on its 12 scenarios of one field type
or message shape and its 90 `rv` pools.

### Checksums

Each arm adds what it decoded into a checksum, and all arms of a scenario must arrive at the same
one. On `Dataset` a string adds its length, and no walk reads its bytes. On `Dataset`, Google's
payloads and the OSM scenarios the arena program compares the checksums once before timing and
stops at the first mismatch. Every scenario's checksums are compared after timing. A mismatch at
either point makes the program exit with a non-zero status, after which `bench.py` writes no
snapshot.

A matching checksum doesn't mean the arms did the same work. Where it differs:

- On Google's payloads the checksum counts only the fields present in the input. Some of their
  proto2 fields have non-zero defaults, which protoc's accessors return for a missing field and the
  streaming decoder never sees.
- [bench_arm_osm.hpp](https://github.com/VeaaC/rapidproto/blob/main/tests/bench_arm_osm.hpp)
  defines the OSM checksum. Its header comment lists where the arms' work still differs. libosmium,
  for one, builds a full object for every entity.
- On the OSM data the streaming arm reads each block's string table in a pass of its own, because
  tags refer to strings by their index in the table and nothing obliges a block to store the table
  first.
- On `records sparse (dropped)` protoc decodes all 20 fields of a record and reads only the 3 that
  the profile keeps.

### Timing

The harness times its samples by the clock. Through `perf_event` it also counts each arm's CPU
cycles and retired instructions over the timed calls, given per byte as cyc/B and ins/B. No
published table shows them, but ins/B barely moves between runs of one binary on one input. When
GB/s moves and ins/B doesn't, the same instructions ran at a different speed.

A sample is one batch of back-to-back calls of one arm, where a call decodes the payload once or
makes one pass over a pool. All arms of a scenario make the same number of calls per sample, chosen
so that arm 0's sample lasts about 10 ms. The harness picks that number, `calls`, after one call of
every arm. It times single calls of arm 0, taking the median of five when one call is under 10 ms,
and divides 10 ms by the result.

A timed single call also pays for reading the clock and the counters. On a small payload the time
per call then comes out too high and `calls` too low. Its samples fall short of 10 ms.
With g++-13 a `google_message1` sample is about 18,000 calls. There `protoc`, the arm with a new
Arena per decode, takes 376 ns a call. Its sample lasts about 7 ms. `google_message2` gets 63 or 64
calls a sample and `Dataset` 7.

Samples are taken in rounds. A round takes one sample of every arm, starting one arm later than the
round before. The harness drops the first round's samples as a warm-up.

After each round the harness divides every arm's cost by arm 0's. It counts both in cycles, or in
wall time when the counters are unavailable or an arm runs threads. After 17 rounds (9 for
`osm_blocks` and `osm_file`), and every eighth round after that, it checks whether each arm has
settled. An arm has settled once the harness can tell which of it and arm 0 is faster, or knows
their ratio to within a fraction of a percent. The test is that the mean of the arm's ratios lies
more than three times the half-width of their 95% confidence interval away from 1, or that the
half-width is under 0.3% of the mean. Sampling stops at whichever of these comes first:

- a check at which every arm has settled;
- after the first check, the end of the scenario's time budget, settled or not: 3 seconds for most
  scenarios, 20 for `osm_blocks` and 30 for `osm_file`;
- before the first check, 20 seconds, or the budget where that's longer.

In the published runs, 3 of the 464 arms other than arm 0 built with g++-13 and 4 built with
clang++-20 hadn't settled when sampling stopped. None of them feeds a published figure.

The rule only sets the number of rounds. The mean ratio it tests isn't published. An arm's
throughput, in decimal GB/s, is the payload's size times `calls`, divided by the arm's median sample
time over the timed rounds.

### Runs

For the published snapshots `bench.py` ran each program five times and kept, for each arm, the run
with the median GB/s ([Five runs and the median](#five-runs-and-the-median) says why). The programs
ran under `taskset` on core 2 of a quiesced machine: nothing else ran, SMT and turbo were off, and
every CPU used the `performance` governor. [Reproducing](benchmarks.md#reproducing) shows how
`tests/bench_box.sh` prepares the machine. What each setting was worth is under
[Machine settings](#machine-settings).

### Memory

The [memory table](benchmarks.md#c) comes from one more decode of `Dataset` by protoc and by the
arena decoder, outside the timing, each into a new arena. The published figures come from runs of
both builds limited to `Dataset`. Both gave the same bytes.

The arena decoder's in-use and held figures are its arena's `bytes_used()` and `bytes_reserved()`.
Its strings point into the input and add nothing outside the arena. protoc's figures start from its
Arena's `SpaceUsed()` and `SpaceAllocated()`. Neither sees the text of a long string. The Arena
holds each `std::string` object, but libstdc++ keeps at most 15 bytes of text inside it and gives a
longer string a heap buffer of its own. `Dataset` has 3,001 strings that long. For each buffer,
protoc's in-use figure adds the bytes requested (the string's capacity plus one). Its held figure
adds the size of the allocator's chunk, which under glibc is the buffer's usable size plus an 8-byte
header.

## Payloads

[Benchmarks](benchmarks.md) describes `Dataset`, Google's two payloads, the OpenStreetMap extract
and the [Rust comparison's payloads](benchmarks.md#other-rust-decoders). Every payload the programs
generate starts from a fixed seed.

| Scenario | Program | Arm 0 | Other arms | Payload |
|---|---|---|---|---|
| `Dataset` | arena | `protoc` | `protoc-warm`, `arena-cold`, `arena-warm`, `streamgen`, `protozero`, `upb` | 368,611 bytes |
| `google_message1`, `google_message2` | arena | `protoc` | `protoc-warm`, `arena-cold`, `arena-warm`, `streamgen`, `upb` | 228 and 84,570 bytes |
| `osm_blocks` | arena | `protoc` | `arena-cold`, `arena-warm`, `streamgen` | the extract's 246 data blocks, inflated before timing: 45,946,946 bytes per pass |
| `osm_file` | arena | `arena-warm` | `streamgen`, `libosmium` | the file's 20,324,876 bytes, from memory |
| `compute Instance` | arena | `arena-cold` | `arena-warm` | an `Instance` message of 40,498 bytes, from googleapis' `compute.proto` |
| `packed int64(varint)`, `packed double(fixed)`, `osm: sint64 deltas + enum`, `few msgs, big arrays`, `many msgs, tiny arrays`, `scalar records (dispatch-bound)` | arena | `protoc` | `protoc-warm`, `arena-warm` | six of the Rust comparison's payloads |
| `records sparse (dropped)` | arena | `protoc` | `arena-warm` | the scalar records under a [decode profile](profiles.md) that keeps 3 of 20 fields |
| `rv <values> <count>` | arena | `protoc` | `arena-warm`, `protozero` | a pool of packed varint arrays |
| `rv <values> <count>` | streaming | `generated` | `wire`, `protozero` | the same pools |
| `rv-zz <values> 1M`, `rv-enum <values> 1M` | arena | `protoc` | `arena-warm`, `protozero` | the same arrays of a million values as packed `sint64` (15 scenarios) and as packed enums (5) |
| `varint-1byte`, `nested-msg`, `packed-int32` and 9 more | streaming | `generated` | `wire`, `protozero` | one field type or one message shape each |
| `groups`, `records dense (stream)`, `records sparse (stream)` | streaming | `generated` | `wire` | a group, which protozero can't read; the scalar records, read in full or 3 fields of 20 |

The `rv`, `rv-zz` and `rv-enum` scenarios together are the packed-varint pools. In
`rv <values> <count>` the count runs from 10 to 1M in powers of ten. The values follow one of 15
distributions of encoded length:

- `fx1` to `fx10`: every value takes that many bytes.
- `unif`: 1 to 10 bytes, uniformly.
- `skew`: nine values in ten take one byte, the rest 1 to 10 bytes.
- `mix12` and `mix13`: 1 to 2 bytes, or 1 to 3, uniformly.
- `mix13out`: like `mix13`, with one value in a hundred taking 10 bytes.

The one-byte rows of the arena-or-streaming table on Benchmarks are `fx1`. Its longer or mixed rows
cover the other 14. A pool holds enough distinct arrays that the branch predictor can't learn
their sequence of lengths. Each has at least 2^17 values in at least 64 arrays. For the longest
arrays a cap of about 64 MB lowers the array count, down to 8 arrays of a million values.

[`fetch_corpus.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/fetch_corpus.py) fetches
each third-party source as a shallow sparse checkout of a pinned ref and fails if a tag has moved.

| Source | Pinned as | Commit | Used for |
|---|---|---|---|
| protobuf | tag v35.1 | `35cd01f9` | upb; `descriptor.proto` in the compile-cost sweep |
| protobuf | tag v21.12 | `f0dc78d7` | `google_message1`, `google_message2` |
| googleapis | the commit itself | `09bc253e` | `compute Instance`; `compute.proto` in the compile-cost sweep |
| libosmium | tag v2.23.1 | `97dccf10` | `osm_file` |
| protozero | tag v1.8.2 | `af2f6e27` | the `protozero` arms and libosmium |

[`fetch_osm_dataset.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/fetch_osm_dataset.py)
downloads `bremen-250101.osm.pbf` into `build/corpus/osm/` and refuses any file whose SHA-256 isn't
`f650fc00f0545501ec75e93f5919b13ce037a8e0b22aba97495a0f6ec1356bef`. The data is © OpenStreetMap
contributors under the ODbL 1.0 and isn't part of the repository.

## Baselines

protoc reuses memory as the arena decoder does: one Arena on the `Dataset` and Google tables, one
message on the OSM blocks. It runs on protobuf v35.1, the release that was faster than 4.25.3 on
Google's payloads and the OSM blocks and within 3% of it on `Dataset`. Each of the other baselines
differs from RapidProto's setup in one way. protozero skips checks that the generated decoders
make, which works in its favour. upb gets a new arena for every decode and reads its fields back
through a slower path than its generated code would take. libosmium's worker threads share the
benchmark's single core.

Both protoc arms build the complete message. Everywhere but `osm_blocks`, `protoc` parses into a
new [`google::protobuf::Arena`](https://protobuf.dev/reference/cpp/arenas/) on every call.
`protoc-warm` keeps one Arena; each call empties it with `Reset()`, creates the message in it with
`Arena::Create` and runs `ParseFromString`. The `Dataset` and Google tables divide by
`protoc-warm`, though the harness keeps `protoc` as arm 0. The six payloads shared with the Rust
comparison have a `protoc-warm` arm too, but no published table compares them with protoc. The
packed-varint pools and `records sparse (dropped)` have only `protoc`.

In protobuf v35.1, `Reset()` frees all of an Arena's memory except a first block handed to its
constructor. So `protoc-warm` builds its Arena over a buffer the program owns
([bench_protoc.cpp](https://github.com/VeaaC/rapidproto/blob/main/tests/bench_protoc.cpp)): 32 MB
for `Dataset` and the six shared payloads, 8 MB for Google's. Outside that block, in heap chunks,
the Arena keeps a list of the destructors `Reset()` will run. For the map entries of `Dataset` that
list takes roughly 64 KB on every parse. We checked every `protoc-warm` parse, and apart from that
list each one fits in its block. Without the reuse protoc is about 1% slower on `Dataset`, at most
1.2% slower on `google_message2` and 15% to 17% slower on `google_message1`.

Both compilers' builds link one copy of libprotobuf, compiled by GCC 13 in Release mode. clang++-20
compiles only the `.pb.cc` files that protoc generates, so in the clang++-20 build most of protoc's
decoding runs in GCC-built code.

On `osm_blocks` protoc parses every block into one reused `PrimitiveBlock` message, with no Arena.
At the blocks' average size of 186,776 bytes `arena-cold` runs within 0.5% of `arena-warm` with
either compiler. Arena allocation explains little of the arena decoder's lead over protoc on these
blocks.

### Choosing the protoc baseline

The published runs use protobuf v35.1, whose library calls itself 7.35.1. The fetched corpus already
pins that release for upb. Using it for protoc as well puts protoc's code and upb's on one release.

We also timed libprotobuf 4.25.3 on the same night, a few hours before 7.35.1, with identical
RapidProto code on the quiesced machine. The 561 arms per compiler that run no protobuf code
(RapidProto's decoders, protozero, the `wire` loop and libosmium) serve as a control. Between the
two snapshots they changed by a median of about 0.1%, and 533 of them by less than 1%. protoc moved
a good deal more:

| protoc 7.35.1 against 4.25.3 | Change |
|---|---|
| `Dataset`, `protoc-warm` | within 3% |
| Google's payloads, `protoc-warm` | 4% to 17% faster |
| OSM blocks, one reused message | 14% to 15% faster |
| the six shared payloads, `protoc-warm`, in no published protoc ratio | 1% to 32% faster |

### protozero

[protozero](https://github.com/mapbox/protozero) checks wire types only in `assert`s, and `-DNDEBUG`
compiles those out. Built that way, protozero accepts some malformed input that the generated
decoders and the `wire` loop reject.

### The upb baseline

[upb](https://github.com/protocolbuffers/protobuf/tree/main/upb) is the C library behind protobuf's
Python, Ruby and PHP packages. The arena program times it on `Dataset` and Google's payloads, built
from protobuf v35.1's sources in the fetched corpus with the benchmarks' flags.

upb decodes from MiniTables. Rather than compiling upb's generated code, the benchmark builds them
at startup from a `FileDescriptorSet` that protoc writes at build time (`--descriptor_set_out`).
That doesn't slow upb's decoding, because at v35.1 upb fills a runtime table's fast-path entries
with `upb_DecodeFast_BuildTable`, the same function its code generator calls for generated tables.
The build sets `UPB_ENABLE_FASTTABLE=1` on x86-64 and arm64, where upb supports the fast path
under GCC and Clang. That path doesn't handle map fields, and every `Dataset` record has one. upb
tries the fast path again field by field. Only the `counters` entries take the slower MiniTable
path.

Every timed decode passes `kUpb_DecodeOption_AliasString`. With it, upb's strings point into the
input as RapidProto's do. Each decode gets a new arena from `upb_Arena_New`, like `arena-cold`.
`upb_Arena_Init` would let every decode start on one reused buffer, upb's counterpart of the
caller-owned block that `protoc-warm` uses. The upb arms don't call it yet.

The read-out walk calls upb's inline accessors and hands each one the `upb_MiniTableField` pointer
the program looked up for that field at startup. Since that pointer is known only at run time, every
call loads the field's offset and mode through it and tests for the extension and oneof cases before
it reads. Repeated fields come one element at a time from `upb_Array_Get`. The `counters` map is
walked through upb's hash table. Generated upb code hands its accessors a constant field description
instead, from which the compiler resolves the offset and drops those tests. The arena decoder's walk
reads plain struct members and arrays. A program built on upb's generated code would spend somewhat
less on this walk than the published upb rows do, by an amount we haven't measured.

### libosmium

[libosmium](https://github.com/osmcode/libosmium) 2.23.1 is a header-only C++ library for
OpenStreetMap data. Its PBF reader is built on zlib and on the same protozero 1.8.2. It has no
`osm_blocks` arm, because its API begins at the file's framing. libosmium decodes on worker threads,
which start after the harness has pinned the process and so share the process's single core. The
hardware counters would see only the main thread, so `bench.py table` shows `n/a` for libosmium's
cyc/B and ins/B, and the whole `osm_file` scenario settles on wall time. How libosmium scales over
several cores isn't measured here.

## Measurement noise

Most published figures barely move from run to run. The exceptions are all on packed-varint pools.
A rebuild that only moves code can shift an arm by up to about 6.5%. How that figure and the
machine settings were arrived at is under [Calibration studies](#calibration-studies).

Each noise figure on this page uses one of these measures:

| Name | Definition | Used for |
|---|---|---|
| spread (`spread_pct`) | the interquartile range of an arm's five GB/s values as a percentage of their median; with five values, the gap between the second-lowest and the second-highest | snapshots, the published runs, `bench.py diff` |
| range | (max − min) / min over an arm's runs | the July 2026 studies and single rows under [The remaining noise](#the-remaining-noise) |
| placement range | (max − min) / median over 20 relinked layouts | the September 2026 relink study |

In the runs behind the published C++ tables, each compiler's 694 arms had a median spread of 0.15%
(g++-13) and 0.16% (clang++-20). Seven arms spread by more than 5%:

| Compiler | Arm | Scenario | Spread |
|---|---|---|---|
| g++-13 | `protoc` | `rv fx1 10k` | 29.4% |
| g++-13 | `protoc` | `rv fx3 1k` | 9.8% |
| g++-13 | `protozero` | `rv fx3 10` | 7.6% |
| g++-13 | `generated` | `rv fx1 10` | 6.9% |
| clang++-20 | `protoc` | `rv fx7 1k` | 8.9% |
| clang++-20 | `protozero` | `rv fx1 10` | 8.6% |
| clang++-20 | `protoc` | `rv fx1 10k` | 7.6% |

Four of them are `protoc` arms, whose figures on these pools no published table uses. The worst, on
`rv fx1 10k` with g++-13, had two runs out of five that were more than 25% faster than the other
three. Of the other three arms, the two protozero ones feed no table either. The clang++-20 one is
among the 102 payloads of Benchmarks' comparison with protozero, where the streaming decoder runs
1.6 times as fast on it, too far ahead for an 8.6% spread to change. `generated` on `rv fx1 10` is
the denominator of one cell, the g++-13 one-byte cell for 10 values under
[Arena or streaming](benchmarks.md#arena-or-streaming).

### The remaining noise

With g++-13 the arena decoder's one-byte rows from 10,000 values up get outlying runs, and the
median of five doesn't always absorb them. Two g++-13 snapshots show it: the libprotobuf 4.25.3 one
under [Choosing the protoc baseline](#choosing-the-protoc-baseline) and the published one. They
share their RapidProto code and payloads. What differs is libprotobuf and the `protoc` arm's calls
into it. On these pools `protoc` is arm 0, running in the same process as `arena-warm`.

| `arena-warm`, g++-13 | 4.25.3 snapshot | Published snapshot |
|---|---|---|
| `rv fx1 10k` | one run 38% above the median | range 0.7%; median 0.1% lower |
| `rv fx1 100k` | one run 30% above the median; range 45% | one run 9.3% below the median; median 10.7% lower |
| `rv fx1 1M` | one run 29% above the median | range 2.9%; median 1.4% lower |

The lower median on `rv fx1 100k` took that cell [on Benchmarks](benchmarks.md#arena-or-streaming)
from 0.77 to 0.68. `bench.py diff` of the two snapshots reports it as a regression of 10.7% against
a spread of 7.7%, with ins/B 0.5% apart. The arm's own code didn't change. Its drop is larger than
the 6.5% that relinking alone moved any arm.

Other decoders on these pools moved less. With clang++-20 the arena decoder's medians moved by at
most 1.4%. The streaming program has no protoc arm and doesn't link libprotobuf. In it the
streaming decoder and protozero stayed within 1% with either compiler, but the `wire` loop's
g++-13 median on `rv fx1 10k` fell by 8.8%.

## Calibration studies

We chose the alignment flags, the machine settings and the five runs per snapshot in July and
September 2026, from studies on the harness of that time. Its samples didn't yet batch their calls
or cover a whole pool. The figures below are as it measured them.

### Code placement

In September 2026 we relinked the same object files in 20 shuffled section orders and timed each
layout once. Without the alignment flags the worst arms had a placement range of up to 22%. Their
ins/B stayed flat, apart from about 1% on upb. With the flags most arms stayed within 4%. The worst,
at about 6.5%, were upb, protoc and one streaming arm. protoc's code sits in the prebuilt
libprotobuf, which the alignment flags never reach. Since each layout ran only once, 1% to 3% of
these figures is ordinary run-to-run noise.

### Machine settings

In July 2026 we ran one binary five times under each machine setting. On the machine as we found it,
with the `powersave` governor, turbo and SMT, the arena program's 259 arms had a median range of
1.0%, and 21 of them passed 5%. Turbo off and the `performance` governor brought that to 0.6% and 14
arms. Turning SMT off as well barely changed the median. With SMT on, though, the sibling thread of
the benchmark's core may or may not pick up other work in a given run. Over five runs that can split
an arm's results into two clusters. One background process on the otherwise quiesced machine
pushed 84 arms over 5%.

The least stable arm in July was `arena-warm` on `rv fx1 1M`, with a range of 14%. We ran it twelve
times for each extra hardware event we counted. Its retired instructions didn't change and its
branch misses had a range of 0.1%. Its last-level cache misses had a range of 100% and followed its
cycle counts (r = 0.94). Other cores share that cache and can evict the benchmark's data. This arm
keeps eight arrays of roughly 1 MB and an arena of about 8 MB live. In the published g++-13
snapshot the same row has a range of 2.9%.

### Five runs and the median

In July 2026 we settled on five runs per snapshot and on keeping each arm's median run. Resampling
40 runs of the noisiest arm showed how far two snapshots of unchanged code can disagree. The change
that one comparison in twenty exceeded was about 14% with one run per snapshot, 9% with three, 8%
with five and 6% with seven.

A single stray run moves the fastest run of five but not the median. When we measured one binary
twice, five runs each and 485 arms in the check, the largest apparent change was 3.7% comparing
each arm's median run. Comparing each arm's fastest run gave 46.9%, because one arm had a lone fast
run in one snapshot and not in the other.

## The Rust benchmark

`rapidproto-bench` takes a fixed number of rounds, 20 unless `ROUNDS` sets another count, and
rotates the arms from round to round like the C++ harness. It has no settling test. A sample is a
batch of whole passes over the payload, sized from one timed pass so that the baseline's sample
lasts about 10 ms
([rust/README.md](https://github.com/VeaaC/rapidproto/blob/main/rust/README.md#optional-scenarios-and-settings)
lists the settings). A run reports, for each arm, the median time per call over its rounds. Wherever
the C++ program stored a checksum with a payload, every Rust decoder must reproduce it.

The published Rust runs, five of them, were made with SMT and turbo off and the `performance`
governor. Over the 233 Rust figures in the comparison with C++, the median spread across the five
runs was 0.16%, computed as for C++.

### Comparing Rust with C++

No tool in the repository joins the two languages. Each Rust record printed under
`RAPIDPROTO_BENCH_JSON=1` carries `cpp_scenario` and `cpp_arm`. They name the matching C++ row
where there is one, and they're `null` for prost, rust-protobuf, quick-protobuf and femtopb. The
published comparison divides each Rust GB/s by its C++ counterpart's, each taken from its own
side's median run. Of the six arms with a counterpart, it uses three:

| Rust arm | C++ arm | In the published comparison |
|---|---|---|
| `rapidproto-warm` | `arena-warm` | 123 pairs |
| `generated` | `generated` | 105 pairs |
| `rapidproto-stream` | `streamgen` | 5 pairs |
| `rapidproto` | `arena-cold` | not used |
| `google-upb` | `upb` | not used |
| `wire` | `wire` | not used |

The arena pairs are the 110 packed-varint pools and 13 single scenarios: `Dataset`, Google's two
payloads, both OSM scenarios, `compute Instance`, the six shared payloads and
`records sparse (dropped)`. The 110 streaming pairs are the streaming program's 90 pools and its 15
other `generated` scenarios, plus `Dataset`, Google's two payloads and both OSM scenarios through
`streamgen`. `rapidproto` and `arena-cold` both take a new arena per decode. Their speed depends on
the state of the C library's allocator, which differs from one process to the next.

## The compile-cost sweep

[`tests/compile_bench.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/compile_bench.py)
measures what a generated header costs to build. Its six cases run from one message of the test
schema [proto3.proto](https://github.com/VeaaC/rapidproto/blob/main/tests/corpus/proto3.proto) to
ten of the 2,223 messages in googleapis' 103,651-line `compute.proto`. In between are a schema with
more than 64 required fields in one message, nesting chains of 5 and 10 messages, and five messages
of `descriptor.proto`. Each case is one source file that gives every message an externally linked
function, which obliges the compiler to emit every decode. The sweep compiles each case for both
decoders with g++-13 and clang++-20 at `-std=c++17 -O3 -DNDEBUG`. It records wall seconds, `.text`
bytes from `objdump` and peak memory from `/usr/bin/time`.
