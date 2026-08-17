# RapidProto — fast, header-only Protobuf decoders for C++

[![CI](https://github.com/VeaaC/rapidproto/actions/workflows/ci.yml/badge.svg)](https://github.com/VeaaC/rapidproto/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/VeaaC/rapidproto)](https://github.com/VeaaC/rapidproto/releases)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**Faster than `protoc` + Arena when materializing a full message tree, and faster than protozero
when streaming fields — with wire validation that never compiles out ([benchmarks](docs/benchmarks.md)).**

RapidProto compiles a `.proto` schema into **header-only C++ decoders**. One CLI, `rapidprotoc`, turns
your schema into headers you `#include`. Nothing to link. A single schema gives you two
decode models, and you pick whichever fits the job:

- **Arena.** `decode()` materializes the whole message into a read-only object tree in a bump
  arena, which you navigate with accessors (`person->name()`) in any order, as many times as you like.
- **Streaming.** `decode()` walks the wire once and hands each field's typed value to a
  callback you supply. Nothing is materialized, and there's zero allocation.

A `--dump` flag adds a third, optional emitter: a **debug dumper** that prints a decoded arena tree
as human-readable, JSON-*like* text — an inspection aid for logging and debugging, not a spec-compliant
JSON codec (see [the debug dumper](docs/dumper.md)).

Both decode models are **decode-only**: no serialization, no JSON codec. Both fully validate untrusted wire input
(truncation, length overruns, group nesting), and both trust the schema — they assume `protoc` already
accepted it, so field *values* aren't range-checked. They cover **proto2, proto3, and the newer
editions schema format (2023/2024)**, including groups, maps, and oneofs.

You can read the same schema with either model, and even use both **in one translation unit** (see
[using both models](docs/using-both-models.md)).

> See [`architecture.md`](architecture.md) for the internals and design rationale: the layout planner,
> the compile-time dispatch, the arena, the coexistence design, and the benchmark methodology.

---

## Why RapidProto

RapidProto does one thing: decode Protobuf fast in C++, for the common case where you never serialize.
`protoc`'s C++ runtime is a linked library that builds a full mutable message object per decode;
zero-copy pull parsers like protozero drop that allocation but leave you to hand-write the read loop.
RapidProto generates the decoder — specialized to your schema at compile time, header-only — and gives
you both shapes.

- **Faster decode.** On a realistic mixed payload the arena decoder materializes a full object tree
  **~5× faster than `protoc` + `google::protobuf::Arena`**, and the streaming decoder — materializing
  nothing — is faster still, beating `protozero`, the zero-copy yardstick, on the realistic payload and
  most microbenchmark shapes. See [benchmarks](docs/benchmarks.md).
- **Less memory.** The arena tree holds **~half** of protoc's (both payload bytes and total allocation):
  strings, bytes, and `raw` payloads are **borrowed** as views into the input rather than copied, so the
  arena carries only a read-only bump-allocated tree with no per-field object overhead.
- **Header-only, nothing to link.** One CLI turns your `.proto` into headers you `#include` — no runtime
  library, no `libprotobuf` on your link line.
- **Two models, one schema.** Materialize a navigable tree (arena) *or* stream each field to a typed
  callback with zero allocation — chosen per call site, even in one translation unit.
- **Safe on untrusted input.** Both models fully validate wire structure (truncation, length overruns,
  group nesting) and are built never to crash on malformed bytes; only field *values* are trusted to the
  schema.
- **Compile-time typed.** Dispatch is entirely compile-time — no `std::function`, no virtual calls, and
  a wrong value type or a renamed field is a compile error rather than a silent bug.

Know the trade-offs: RapidProto is **decode-only** (no serialization, no JSON codec, no
reflection) — the one exception being an opt-in [debug dumper](docs/dumper.md) that emits JSON-*like*
inspection text — the arena tree is **read-only** (you navigate it, you don't build or mutate messages), it
decodes enums as **open** even when the schema declares them closed, and it does not validate string
UTF-8 (it accepts `string` bytes `protoc` would reject). If you also need to *produce* or mutate
messages, keep `protoc` for that side and use RapidProto for the hot decode path.

---

## Quick start

**Requirements:** C++17 and a recent GCC or Clang. Header-only — nothing to link.

Using CMake? The [`rapidproto_generate()` helper](docs/integration.md#cmake-integration) wires
generation into your build in a few lines. This section drives the tool by hand so each step is
visible. Build it once:

```sh
cmake --preset release                               # system compiler, optimized
cmake --build --preset release --target rapidprotoc
# binary: build/release/rapidprotoc
```

Given `person.proto`:

```proto
syntax = "proto3";
package example;

message Person {
  string name = 1;
  uint32 id = 2;
  repeated string email = 3;   // repeated: navigable array
  Address address = 4;         // sub-message
}

message Address {
  string city = 1;
  string country = 2;
}
```

**1. Generate** the arena decoder (the default model) and a self-contained copy of the runtime, into
`out/`:

```sh
./build/release/rapidprotoc -I. --out-dir=out person.proto   # add -v to log each written file
# out/person.rp.hpp + out/person.rp.common.hpp + out/rapidproto/{runtime,arena_runtime}.hpp
```

**2. Decode.** You supply the serialized message bytes (from a file, socket, database, …) as a
`rapidproto::ByteView` (an alias for `std::string_view`, so a **non-owning** view; for a
`std::uint8_t` buffer, `rapidproto::byte_view(ptr, size)` builds one without a manual cast). Create
an `Arena`, call `decode()`, then navigate the returned tree:

```cpp
#include "person.rp.hpp"

namespace ex = rp::arena::example;   // generated types live under rp::arena; alias it once

std::string buf = /* the serialized Person bytes */;

rapidproto::Arena arena;
rapidproto::ArenaDecodeError err;
const ex::Person* p = ex::Person::decode(rapidproto::ByteView(buf), arena, &err);
if (p == nullptr) { /* malformed input: see err.code / err.wire / err.offset */ }

std::uint32_t id = p->id();                        // scalar, by value
std::string_view name = p->name();                 // string, a view into the input buffer
if (const ex::Address* a = p->address())           // sub-message: a pointer (nullptr if absent)
    std::string_view city = a->city();
```

> **Need test bytes?** Encode some with `protoc`: `protoc --encode=example.Person -I. person.proto < values.txt > person.bin`

**3. Compile** with only the output directory on the include path:

```sh
g++ -std=c++17 -Iout my_consumer.cpp -o my_consumer
```

That's the arena model. To stream instead, pass `--stream` (or `--arena --stream` for both) and use
the [callback API](docs/streaming.md).

---

## Choosing a model

| | **Arena** | **Streaming** |
|---|---|---|
| What you get | a materialized object tree you read by accessor | a callback fired per field, in wire order |
| Allocation | one bump arena (you own it) | none |
| Random access / re-reading | yes: any field, any order, repeatedly | no: a single forward pass |
| Memory | the whole decoded tree | only what your callbacks keep |
| `#include` | `<stem>.rp.hpp` | `<stem>.rp.stream.hpp` |
| Best for | needing the message as a navigable object; a faster/lighter `protoc`+`Arena` | extracting a few fields, stream-processing, lowest overhead |

Use **arena** when you need the decoded message as an object to navigate — random access, multiple
passes, passing the tree around. Use **streaming** when you handle each field and move on: summing a
column, pulling two fields from a big message, transcoding, or anywhere you want zero allocation. You
can use both models for one schema in one translation unit; see
[using both models](docs/using-both-models.md).

---

## Documentation

| Page | What's in it |
|---|---|
| [docs/arena.md](docs/arena.md) | The arena decoder: accessors, the `Arena`, `decode_owned`, error handling |
| [docs/streaming.md](docs/streaming.md) | The streaming decoder: field tags, the three consumption patterns, aborting |
| [docs/dumper.md](docs/dumper.md) | The `--dump` debug dumper: JSON-like inspection text, `DumpOptions` |
| [docs/semantics.md](docs/semantics.md) | The shared rules: lifetimes, validation & trust, presence/defaults, open enums, duplicate fields, thread-safety |
| [docs/using-both-models.md](docs/using-both-models.md) | Both models in one TU, the mid-decode hybrid, coexisting with protoc |
| [docs/profiles.md](docs/profiles.md) | Decode profiles (`drop` / `raw`) and unknown-field detection (arena) |
| [docs/integration.md](docs/integration.md) | The `rapidprotoc` CLI reference and the CMake helper (incl. cross-compiling) |
| [docs/benchmarks.md](docs/benchmarks.md) | The numbers, how they're measured, and how to reproduce them |
| [CHANGELOG.md](CHANGELOG.md) | Notable user-visible changes per release (SemVer-0: the MINOR version is the breaking axis) |
| [architecture.md](architecture.md) | Internals and design rationale, for contributors |

A runnable end-to-end example (one schema, both models in one TU, a decode profile) is in
[`examples/consumer`](examples/consumer).

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for building, the `./check.sh` quality gate, and how the golden
tests work. The design and the invariants a change must preserve are in
[architecture.md](architecture.md).

## Security

RapidProto decodes **untrusted** wire input and is built never to crash on it. See
[SECURITY.md](SECURITY.md) to report a vulnerability or read the threat model.

---

## License

RapidProto is licensed under the **Apache License 2.0**; see [`LICENSE`](LICENSE), with attributions in
[`NOTICE`](NOTICE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

The vendored runtimes (`rapidproto/runtime.hpp`, `rapidproto/arena_runtime.hpp`, and
`rapidproto/dump_runtime.hpp`) carry the same Apache-2.0 license, so the headers `rapidprotoc` drops
into your out-dir are usable under those terms.
The decoder code generated *from your schema* is your own work product, and RapidProto claims no rights
over it. The embedded Protocol Buffers well-known-type definitions are Copyright 2008 Google Inc.,
licensed 3-Clause BSD. Catch2 and protozero are development-time dependencies and are not distributed.
