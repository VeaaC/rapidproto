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

A snapshot is NDJSON tagged with the compiler, protobuf version, and git revision, so a number is
never separated from what it was measured against. GB/s is the primary signal; cyc/B and ins/B are
kept as diagnostics. Cross-build GB/s carries a ~10% code-placement noise floor — read
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
