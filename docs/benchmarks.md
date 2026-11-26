# Benchmarks

RapidProto generates decoders and no encoders, so every number here is for decoding. Its C++
decoders are measured against protoc's generated C++ code with a
[`google::protobuf::Arena`](https://protobuf.dev/reference/cpp/arenas/), against
[protozero](https://github.com/mapbox/protozero) and
[upb](https://github.com/protocolbuffers/protobuf/tree/main/upb), and on OpenStreetMap data against
[libosmium](https://github.com/osmcode/libosmium). The Rust decoders are measured against
[prost](https://docs.rs/prost) and four other crates. A throughput table names its baseline in the
header and gives each decoder's throughput as a multiple of the baseline's. Unless a table says
otherwise, its decoders ran in one program, taking turns on the same payload. All absolute speeds on
this page come from one test machine and depend on its CPU. [Benchmark method](benchmark-method.md)
explains how each number is made.

The C++ decode numbers were measured on 2026-11-25 with g++-13 and clang++-20 at `-O3 -DNDEBUG`,
against protozero 1.8.2, libosmium 2.23.1 and protobuf v35.1 (libprotobuf 7.35.1), which supplies
both protoc and upb. Every figure relative to protoc depends on that release. With the older
libprotobuf 4.25.3, the `Dataset` throughput ratios would stay within about 3% of these, and the
ratios on Google's payloads and the OSM blocks would be about 4% to 17% higher
([Choosing the protoc baseline](benchmark-method.md#choosing-the-protoc-baseline)).
The Rust numbers are from 2026-11-14, built with rustc 1.91.1.

## C++

| `Dataset`, relative to protoc | g++-13 | clang++-20 |
|---|---|---|
| protoc, reusing one `google::protobuf::Arena` | 1 | 1 |
| RapidProto [arena decoder](arena.md) | 6.6 | 5.7 |
| RapidProto [streaming decoder](streaming.md) | 12.1 | 8.8 |
| protozero | 6.2 | 6.0 |
| upb | 1.2 | 1.3 |

`Dataset` is a proto3 message of 2,000 `Person` records, 368,611 bytes as protoc encodes it
([bench.proto](https://github.com/VeaaC/rapidproto/blob/main/tests/bench/bench.proto)). Each record
mixes numbers and strings with two kinds of sub-message, three repeated strings, a packed array of
five `int32` values and a map with two entries. With g++-13, protoc decodes it on the test machine
at 0.235 GB/s, the arena decoder at 1.56 GB/s and the streaming decoder at 2.85 GB/s. Both columns
link the same GCC-built protoc library, which leaves clang++-20 compiling only the part of protoc's
code generated from the schema.

On `Dataset` the streaming decoder is nearly twice as fast as protozero with g++-13 and more than
1.8 times as fast as the arena decoder. With clang++-20 both margins are about 1.5. The streaming
decoder's own benchmark program pits it against protozero on 102 other payloads of single
fields, message shapes and packed arrays. With clang++-20 it's faster on all of them. With g++-13
it's 25% slower on one, `packed-int32`, whose payload is a single long packed `int32` array.

The arena decoder builds its message in an arena, as protoc does here, but leaves every string and
bytes field pointing into the input, which has to outlive the message
([The arena decoder](arena.md)). protoc copies each string into its message. Repeated fields and
maps go in the same arena, and a map keeps its entries in the order they arrived.

protoc's code for a proto3 schema rejects any string that isn't valid UTF-8, while RapidProto
[returns strings unchecked](semantics.md#strings-and-utf-8). The C++ benchmarks don't measure what
that check costs protoc. In the [Rust comparison](#other-rust-decoders), checking every string costs
RapidProto about half of its throughput on `Dataset`.

Both protoc and the arena decoder keep a single arena and reset it between decodes. If your program
decodes many messages, [reuse one arena](arena.md#the-arena) the same way. upb could reuse memory
as well, but the benchmark [gives it a new arena](benchmark-method.md#the-upb-baseline) for each
decode.

The timing includes a walk that adds every decoded field to a checksum. A decoder that decodes a
field only when it's read does that work inside the walk. All decoders on a payload must produce
the same checksum.

For memory, protoc and the arena decoder each decode `Dataset` once into a new arena. In the table,
held is what an arena has taken from the heap, and in use is the part of it handed out while
decoding. protoc's figures also count the text of every string that `std::string` keeps on the
heap, outside the Arena, because it's too long to store in the object. In use counts the bytes
requested for that text, and held what the allocator reserves for it. Neither side's figures count
the 368,611-byte input the arena decoder's message points into.

| `Dataset`, one decode, either compiler | In use | Held |
|---|---|---|
| protoc | 1,837,855 bytes | 1,994,752 bytes |
| RapidProto arena decoder | 970,336 bytes | 1,159,408 bytes |
| RapidProto relative to protoc | 0.53 | 0.58 |

Each C++ throughput figure is the median of five runs of its program. For a typical figure the middle three
runs lie within about 0.15% of each other. Of the 1,388 figures from both compilers, seven spread by
more than 5%, all on packed varint arrays and none by more than 30%. A rebuild that moves the code
in memory can shift a row by a few percent more
([Measurement noise](benchmark-method.md#measurement-noise)).

### Google's benchmark payloads

| Relative to protoc | `google_message1`, g++-13 | `google_message1`, clang++-20 | `google_message2`, g++-13 | `google_message2`, clang++-20 |
|---|---|---|---|---|
| protoc, reusing one `google::protobuf::Arena` | 1 | 1 | 1 | 1 |
| RapidProto arena decoder | 2.4 | 3.4 | 1.2 | 1.3 |
| RapidProto streaming decoder | 4.3 | 3.7 | 2.4 | 1.6 |
| upb | 0.90 | 1.1 | 1.1 | 1.1 |

These anonymized production messages come from the benchmarks that Protobuf's repository carried
until release [v21.12](https://github.com/protocolbuffers/protobuf/tree/v21.12/benchmarks).
`google_message1` is a 228-byte proto2 message. With g++-13 one decode of it takes protoc 318 ns on
the test machine, the arena decoder 133 ns and the streaming decoder 73 ns. `google_message2` is
84,570 bytes long, and one repeated group of small fields makes up 98% of it.

### OpenStreetMap data

Both tables decode `bremen-250101.osm.pbf`, [Geofabrik](https://download.geofabrik.de/)'s extract of
Bremen from 2025-01-01. The 20 MB [OSM PBF](osm-pbf.md#the-format) file holds 1.6 million nodes and
320,000 ways with full metadata, packed into 246 zlib-compressed data blocks that follow a header
block. The first table decodes the data blocks after an untimed inflate.

| Inflated blocks, relative to protoc | g++-13 | clang++-20 |
|---|---|---|
| protoc, one message reused for every block | 1 | 1 |
| RapidProto arena decoder | 1.4 | 1.3 |
| RapidProto streaming decoder | 1.4 | 1.5 |

The streaming decoder makes two passes over each block, the first for its string table, and both
are timed. The second table starts from the file's bytes in memory and adds the framing and the
inflate. With either compiler those take about 70% of the time the arena decoder spends on the whole
file.

| Whole file, relative to the arena decoder | g++-13 | clang++-20 |
|---|---|---|
| RapidProto arena decoder | 1 | 1 |
| RapidProto streaming decoder | 0.99 | 1.0 |
| libosmium | 0.37 | 0.37 |

libosmium reads a file with a pool of worker threads. Here those threads share the benchmark's
single core, and the whole file is timed by the wall clock. libosmium also builds a complete OSM
object for every entity and copies user names into buffers of its own.

### Arena or streaming

From a packed varint array of 256 bytes or more, the arena decoder fills the message's array in a
loop that reads several bytes per step; a shorter array it reads a byte at a time. The streaming
decoder calls your handler once per value. Because the table's two decoders were timed in different
benchmark programs, a cell close to 1 could go either way. Each cell is the arena decoder's
throughput divided by the streaming decoder's, and a cell above 1 means the arena decoder is faster.

| Value length | Compiler | 10 values | 100 | 1,000 | 10,000 | 100,000 | 1,000,000 |
|---|---|---|---|---|---|---|---|
| One byte | g++-13 | 0.66 | 0.87 | 1.2 | 0.77 | 0.68 | 0.54 |
| One byte | clang++-20 | 0.51 | 0.73 | 1.0 | 1.1 | 1.1 | 0.71 |
| Longer or mixed | g++-13 | 0.54–0.98 | 0.58–2.0 | 1.2–2.3 | 1.2–2.4 | 1.2–2.4 | 1.1–2.3 |
| Longer or mixed | clang++-20 | 0.55–0.94 | 0.77–2.0 | 1.1–2.2 | 1.2–2.3 | 1.1–2.1 | 1.1–2.0 |

The longer or mixed rows cover [14 distributions](benchmark-method.md#payloads) of value length,
from two bytes each to ten, and mixes such as nine one-byte values in ten. With g++-13 the arena
decoder's speed on one-byte arrays of 10,000 values and up varies from process to process, and the
100,000-value cell moved by 11% between two measurements of the same RapidProto code
([The remaining noise](benchmark-method.md#the-remaining-noise)).

The [README](../README.md#two-decoders) compares the two decoders on everything besides speed.

## Rust

[`rapidproto-bench`](https://github.com/VeaaC/rapidproto/tree/main/rust/rapidproto-bench) times
RapidProto's Rust decoders against five other Rust decoders on payloads that the C++ benchmark
programs write out. On every payload in the table below, each decoder's checksum has to equal the
one the C++ program stored with the payload.

### Other Rust decoders

| Payload, relative to prost | RapidProto | prost | rust-protobuf | quick-protobuf | femtopb | google-protobuf |
|---|---|---|---|---|---|---|
| `Dataset` | 12.4 | 1 | 1.0 | 1.9 | n/a | 1.0 |
| packed `int64` | 2.6 | 1 | 1.3 | UB | 0.29 | 0.38 |
| packed `double` | 3.7 | 1 | 1.1 | UB | 0.69 | 0.43 |
| packed `sint64` deltas and enums | 2.8 | 1 | 0.89 | UB | 0.78 | 0.53 |
| few messages, big arrays | 2.8 | 1 | 1.2 | UB | 0.37 | 0.39 |
| many small messages | 8.1 | 1 | 0.94 | 1.4 | 2.0 | 1.4 |
| scalar records | 4.5 | 1 | 0.69 | 0.97 | 1.1 | 0.27 |
| particles | 3.7 | 1 | 0.43 | 0.89 | 0.74 | 0.20 |

Each cell is the median of five runs of a release build without LTO, with functions and loops
aligned to 64 bytes as in the C++ benchmark builds. The RapidProto column is its arena decoder with
a new arena per decode, since each of the other crates also builds a fresh message every time.
RapidProto's streaming decoder was timed on `Dataset` but not on the other seven payloads. There it
reaches 23.6 times prost's throughput.

UB marks the four payloads that quick-protobuf decodes through a message type with a packed
`double` field. Its generated code hands out that field's values as a `&[f64]` pointing straight
into the input. Wherever the input doesn't put the values on an 8-byte boundary, the slice is
misaligned, which is undefined behaviour even where x86 tolerates it, and Miri reports it. Since
some input will produce such a slice, the decoder is unsound, and we show UB instead of its numbers.
femtopb has no map support, hence the n/a on `Dataset`.

- `Dataset`: the payload of the [C++ table](#c).
- Packed `int64` and packed `double`: 200 messages of 1,000 packed values each.
- Packed `sint64` deltas and enums: 200 messages, each with 1,000 small `sint64` deltas and 1,000
  enum values, both packed.
- Few messages, big arrays: 30 messages, each with 10,000 `int64` and 10,000 `double` values.
- Many small messages: 50,000 messages, each with four `int32` arrays of 3 values and two string
  arrays of 2.
- Scalar records: 20,000 records of 18 scalar fields and two sub-messages of 6 scalars.
- Particles: 50,000 messages of an id, a color and two vectors of three `double` values.

| Crate | Version | What a decoded message is |
|---|---|---|
| RapidProto | this repository | lives in an arena, with strings and bytes borrowed from the input |
| [prost](https://crates.io/crates/prost) | 0.14.6 | owns its data, in `String`, `Vec` and `HashMap` |
| [rust-protobuf](https://crates.io/crates/protobuf/3.7.2) | 3.7.2 | owns its data, like prost |
| [quick-protobuf](https://crates.io/crates/quick-protobuf) | 0.8.1 | strings, bytes and packed fixed-width arrays borrow from the input; other repeated fields go in a `Vec`, maps in a `HashMap` |
| [femtopb](https://crates.io/crates/femtopb) | 0.9.0 | borrows from the input, and a repeated field is decoded while you iterate over it |
| [google-protobuf](https://crates.io/crates/google-protobuf) | 0.36.2-release, through `protobuf` 4.36.2-release | lives in a upb arena, with strings copied in |

google-protobuf, Google's Rust implementation, is a beta release. It decodes with upb's C code,
which the crate's build script compiles without `-DNDEBUG`, leaving upb's `assert`s active in a
release build. The table uses the build the crate ships. Twice, on two versions of the benchmark
program, we ran that build alternately with one rebuilt using `CFLAGS=-DNDEBUG`, three runs each.
Both times the rebuilt one was more than 20% faster on each of the four packed payloads.

Every other crate in the table fails a decode on a string that isn't valid UTF-8. RapidProto hands
strings over unchecked, and the benchmark's checksum reads only their lengths. You check a string
by calling
[`ProtoStr::to_str`](https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoStr.html#method.to_str)
on it. We also timed RapidProto calling it on every string, in a separate set of three runs. It kept
47% of its throughput on `Dataset` and 79% on many small messages, still about 6 times as fast as
prost on both.

### Rust and C++

This comparison takes its Rust half from the measurements of 2026-11-14 and its C++ half from the
clang++-20 measurements of 2026-11-25 behind the C++ tables. RapidProto's C++ decoders and runtime
didn't change in the days between. For each payload that both languages time, it
[pairs](benchmark-method.md#comparing-rust-with-c) the two arena decoders, each reusing its arena,
and the two streaming decoders.

| Rust relative to C++ | Arena decoder pairs | Streaming decoder pairs |
|---|---|---|
| Within 3% | 85 | 48 |
| Rust more than 3% faster | 27 | 57 |
| Rust more than 3% slower | 11 | 5 |
| All | 123 | 110 |

The median arena pair runs at the same speed in both languages and the median streaming pair 3%
faster in Rust. No pair has Rust more than 10.4% behind. Rust's largest lead, 5.2 times, is on
records decoded under a [decode profile](profiles.md) that drops most of their fields. The C++
arena decoder is slow on those. With clang++-20 it manages only 1.3 times the throughput of protoc,
which gets a new Arena per decode and parses all 20 fields of each record.

## Compile cost

The [compile-cost sweep](benchmark-method.md#the-compile-cost-sweep) builds one source file per row
at `-O3 -DNDEBUG` and records the wall-clock seconds and the size of the machine code (`.text`). We
ran it on 2026-11-24 on the test machine, with nothing else running.

| Schema | Decoder | g++-13 seconds | g++-13 `.text` | clang++-20 seconds | clang++-20 `.text` |
|---|---|---|---|---|---|
| 10-message chain | arena | 7.1 | 174 KB | 1.7 | 48 KB |
| 10-message chain | streaming | 1.9 | 51 KB | 1.2 | 33 KB |
| `descriptor.proto`, 5 messages | arena | 11.6 | 238 KB | 2.7 | 77 KB |
| `descriptor.proto`, 5 messages | streaming | 1.0 | 25 KB | 0.6 | 10 KB |

In the chain, each message holds the next one as a field, beside a string, an `int32` and a
repeated `int64`. The streaming rows compile a single catch-all handler that doesn't descend into
sub-messages. A program with real handlers for them takes longer to build and gets more code. The
`.text` sizes repeat to the byte for a given compiler version and flags, while the seconds are wall
time and vary a little from one sweep to the next.

## Reproducing

The C++ benchmarks build only on Linux, where the harness reads the CPU's counters through
`perf_event`. They need:

- protobuf: `libprotobuf-dev` with a `protoc` from the same release, or your own build. The
  published numbers used protobuf v35.1, built from source as
  [Building protobuf](running-benchmarks.md#building-protobuf) shows. Its `protoc --version` prints
  `libprotoc 35.1`.
- The
  [fetched corpus](https://github.com/VeaaC/rapidproto/blob/main/CONTRIBUTING.md#the-real-world-schema-corpus),
  for protozero 1.8.2, upb, libosmium, Google's payloads and `compute Instance`, a 40,498-byte
  message of googleapis' `compute.proto` that the Rust and C++ comparison uses. Without the corpus
  the benchmarks use any protozero that's installed. The OpenStreetMap payloads also need the Bremen
  extract and zlib.

Fetch the corpus and the extract before you configure, with
[`fetch_corpus.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/fetch_corpus.py) and
[`fetch_osm_dataset.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/fetch_osm_dataset.py):

```sh
python3 tests/fetch_corpus.py        # schemas and the upb, libosmium and protozero sources, ~100 MB
python3 tests/fetch_osm_dataset.py   # bremen-250101.osm.pbf, checked against its SHA-256
```

Then configure, set up the machine, run, and put the machine back:

```sh
cmake --preset gcc -B build/gcc-pb35 -DCMAKE_PREFIX_PATH=$HOME/protobuf-35.1
tests/bench_box.sh setup                 # uses sudo
python3 tests/bench.py run               # builds and runs build/gcc-pb35
tests/bench_box.sh restore
python3 tests/bench.py table bench_snapshots/g++-13-<rev>.ndjson
```

For clang++-20, configure with `--preset clang -B build/clang-pb35` and the same
`-DCMAKE_PREFIX_PATH`, then pass `--build-dir build/clang-pb35` to `bench.py run`.
[Running the benchmarks](running-benchmarks.md) explains what
[`bench.py`](https://github.com/VeaaC/rapidproto/blob/main/tests/bench.py) `run` writes and how
`diff` compares two snapshots. On a CPU with performance and efficiency cores, pass a performance
core with `--core`.

[`bench_box.sh setup`](https://github.com/VeaaC/rapidproto/blob/main/tests/bench_box.sh) turns off
SMT (the second hardware thread on each core) and turbo, puts every online CPU on the `performance`
governor and lowers `kernel.perf_event_paranoid` to 2 if it's higher. It can turn turbo off only
through the `intel_pstate` driver and reports the setting as skipped under any other. `restore`
writes back the values `setup` saved in `build/bench_box.state`. None of the changes outlives a
reboot. Run nothing else on the machine while the benchmarks run. `bench.py run` checks the settings
again and warns about any that are off, though it checks the governor only on its own core.

### Reproducing the Rust numbers

Building and running `rapidproto-bench` is described in
[rust/README.md](https://github.com/VeaaC/rapidproto/blob/main/rust/README.md#running-the-bench).
The published measurements used two optional features. `compute` adds `compute Instance`, and
`google-upb` adds google-protobuf. The benchmark generates that crate's message code for its
schemas with protoc 36.2, found through `RAPIDPROTO_UPB_PROTOC`. The C++ half of the Rust and C++
comparison is a clang++-20 snapshot made as above in `build/clang-pb35`.
