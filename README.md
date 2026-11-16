# RapidProto

[![CI](https://github.com/VeaaC/rapidproto/actions/workflows/ci.yml/badge.svg)](https://github.com/VeaaC/rapidproto/actions/workflows/ci.yml)
[![Docs](https://img.shields.io/badge/docs-veaac.github.io-2dd4bf)](https://veaac.github.io/rapidproto/)
[![Release](https://img.shields.io/github/v/release/VeaaC/rapidproto)](https://github.com/VeaaC/rapidproto/releases)
[![crates.io](https://img.shields.io/crates/v/rapidproto)](https://crates.io/crates/rapidproto)
[![docs.rs](https://img.shields.io/docsrs/rapidproto)](https://docs.rs/rapidproto)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

RapidProto generates fast Protobuf decoders for C++ and Rust from your `.proto` schemas. On a
realistic mixed payload, the C++ arena decoder is about 7 times as fast as `protoc`'s generated code
with an arena, and the Rust one is about 12 times as fast as prost.

The decoders validate untrusted input and never crash on malformed bytes. They support proto2,
proto3 and editions 2023 and 2024, including groups, maps and oneofs.

Each schema gives you two decoders:

- An arena decoder decodes the whole message into an arena. You read the fields through accessors,
  in any order and as often as you like.
- A streaming decoder reads the input once and passes each field to your code. It builds no message
  and does not allocate.

In C++, the `rapidprotoc` compiler turns a schema into headers. You include them, and there is no
library to link and no other dependency. In Rust, the `rapidproto-build` crate generates the
decoders in your build script. It does not need `protoc`. The generated code uses the `rapidproto`
crate. The Rust crates are test releases so far.

---

## Why RapidProto

The C++ streaming decoder is faster than protozero. The Rust decoders are as fast as the C++ ones.
On every payload in the comparison, the Rust arena decoder is faster than prost, rust-protobuf,
quick-protobuf, femtopb and google-protobuf. The [benchmarks](docs/benchmarks.md) have the numbers
and how they were measured.

Strings and bytes are not copied. They borrow from the input. A decoded C++ message takes less
memory than `protoc`'s.

RapidProto only decodes. It cannot encode messages, it has no reflection, and decoded messages are
read-only. To build or change messages, use another Protobuf library.

Enums are open, even where the schema declares them closed. Decoding does not check that strings
are valid UTF-8. In Rust, `ProtoStr::to_str` checks a string when you need it as a `&str`.

To inspect decoded messages, C++ has an optional [debug dumper](docs/dumper.md) that prints them as
JSON-like text. In Rust, decoded messages implement `Debug`, and serde's `Serialize` if you enable
it.

---

## Quick start

Both quick starts decode `person.proto`:

```proto
syntax = "proto3";
package example;

message Person {
  string name = 1;
  uint32 id = 2;
  repeated string email = 3;
  Address address = 4;
}

message Address {
  string city = 1;
  string country = 2;
}
```

Encode a test message with `protoc`:

```sh
echo 'name: "Ada" id: 7 address { city: "London" }' |
    protoc --encode=example.Person -I. person.proto > person.bin
```

### C++

You need a C++17 compiler. Prebuilt `rapidprotoc` binaries for Linux, macOS and Windows are on the
[releases page](https://github.com/VeaaC/rapidproto/releases). The binaries are unsigned. If
Gatekeeper blocks one on macOS, run `xattr -d com.apple.quarantine rapidprotoc`. On Windows, dismiss
the SmartScreen prompt. You can also build `rapidprotoc` from source:

```sh
cmake --preset release
cmake --build --preset release --target rapidprotoc   # writes build/release/rapidprotoc
```

In a CMake build, the [`rapidproto_generate()` helper](docs/integration.md#cmake-integration) runs
the generator for you. You can get RapidProto through FetchContent, as an installed package, or from
the Conan recipe or the vcpkg overlay port. [CLI & CMake](docs/integration.md) shows each way. This
quick start runs the generator by hand.

With `rapidprotoc` on your `PATH`, generate the arena decoder and a copy of the runtime headers into
`out/`:

```sh
rapidprotoc -I. --out-dir=out person.proto
# writes out/person.rp.hpp, out/person.rp.common.hpp and out/rapidproto/*.hpp
```

Decode a message and read its fields:

```cpp
// my_consumer.cpp
#include "person.rp.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace ex = rp::arena::example;

int main() {
    std::ostringstream file;
    file << std::ifstream("person.bin", std::ios::binary).rdbuf();
    const std::string input = file.str();

    rapidproto::Arena arena;
    rapidproto::ArenaDecodeError err;
    const ex::Person* person = ex::Person::decode(rapidproto::ByteView(input), arena, &err);
    if (person == nullptr) {
        return 1;  // malformed input: err.code and err.offset say why
    }

    std::uint32_t id = person->id();
    std::string_view name = person->name();  // points into input
    if (const ex::Address* address = person->address()) {  // nullptr when absent
        std::cout << name << " (" << id << ") lives in " << address->city() << '\n';
    }
}
```

`rapidproto::ByteView` is a `std::string_view`, so it does not own the input. For a `std::uint8_t`
buffer, `rapidproto::byte_view(ptr, size)` makes one. Compile with only `out/` on the include path:

```sh
g++ -std=c++17 -Iout my_consumer.cpp -o my_consumer
```

[The arena decoder](docs/arena.md#what-each-field-kind-returns) shows what repeated fields, maps,
oneofs and `optional` fields return. Pass `--stream` to generate the
[streaming decoder](docs/streaming.md) in `person.rp.stream.hpp` instead, or `--arena --stream` for
both.

### Rust

You need Rust 1.91 or newer. Add both crates at the same version, pinned with `=`. The generated
code needs the runtime of the version that generated it:

```toml
# Cargo.toml
[dependencies]
rapidproto = "=0.1.0-alpha.2"

[build-dependencies]
rapidproto-build = "=0.1.0-alpha.2"
```

Generate the decoders in `build.rs`, with `person.proto` next to `Cargo.toml`. `rapidproto-build`
compiles its generator with your C++ compiler, or runs a WebAssembly build of it if you have none.

```rust
// build.rs
fn main() {
    rapidproto_build::Config::new("crate::schema")
        .compile(&["person.proto"])
        .expect("generate decoders");
}
```

Include the generated file at the module path you passed to `Config::new`, then decode a message and
read its fields:

```rust
// src/main.rs
mod schema {
    include!(concat!(env!("OUT_DIR"), "/person.rs"));
}

use schema::arena::example::Person;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let input = std::fs::read("person.bin")?;

    let arena = rapidproto::Arena::new();
    let person = Person::decode(&input, &arena)?;

    let id = person.id();
    let name = person.name().to_str()?; // borrows from input, checks UTF-8
    if let Some(address) = person.address() {
        println!("{name} ({id}) lives in {}", address.city());
    }
    Ok(())
}
```

The [`example` module](https://docs.rs/rapidproto/latest/rapidproto/example/index.html#accessors)
shows what each field kind returns. To generate stream decoders as well, turn on `Config::stream`
and see the [`stream` module](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html).

---

## Choosing a model

| | Arena | Streaming |
|---|---|---|
| What you get | a decoded message that you read through accessors | a call to your code for each field, in wire order |
| Allocation | one arena, which you own | none |
| Reading a field again | yes, any field, in any order | no, the input is read once |
| Memory | the whole decoded message | only what your code keeps |
| Best for | working with the message as an object | extracting a few fields, processing a stream, lowest overhead |

Both models can decode the same schema in one program, and a streaming decode can pass a
sub-message to the arena decoder. For C++, see [using both models](docs/using-both-models.md). In
Rust, pass a sub-message's
[`bytes()`](https://docs.rs/rapidproto/latest/rapidproto/example/stream/weather/struct.Station.html#method.bytes)
to its arena decoder.

---

## Documentation

The C++ manual is in [docs/](docs/) and on <https://veaac.github.io/rapidproto/>:

| Page | What's in it |
|---|---|
| [The arena decoder](docs/arena.md) | Accessors, the `Arena`, `decode_owned` and error handling |
| [The streaming decoder](docs/streaming.md) | Field tags, the three ways to consume fields, and stopping early |
| [The debug dumper](docs/dumper.md) | The `--dump` option, its JSON-like text and `DumpOptions` |
| [Shared semantics](docs/semantics.md) | Lifetimes, validation and trust, presence and defaults, open enums, duplicate fields and thread safety |
| [Using both models](docs/using-both-models.md) | Both models in one translation unit, streaming into the arena decoder, and coexisting with `protoc` |
| [Decode profiles](docs/profiles.md) | Dropping fields, keeping sub-messages as raw bytes, and detecting unknown fields in the arena decoder |
| [CLI & CMake](docs/integration.md) | The `rapidprotoc` command line and the CMake helper, including cross-compiling |
| [OSM PBF walkthrough](docs/osm-pbf.md) | OpenStreetMap's planet format, decoded with both models |
| [One optimization at a time](docs/optimizations.md) | How the decoders got fast, with seven optimizations re-enabled in turn and measured |
| [Benchmarks](docs/benchmarks.md) | The numbers for C++ and Rust, and how to reproduce them |

The Rust guide is the crates' documentation on docs.rs. [`rapidproto`](https://docs.rs/rapidproto)
covers decoding with both models, and [`rapidproto-build`](https://docs.rs/rapidproto-build) covers
generating the code. [CHANGELOG.md](https://github.com/VeaaC/rapidproto/blob/main/CHANGELOG.md)
lists the changes in each C++ release.

Each language has a runnable example that decodes one schema with both models,
[`examples/consumer`](examples/consumer) for C++ and
[`rust/examples/consumer`](https://github.com/VeaaC/rapidproto/tree/main/rust/examples/consumer) for
Rust. The OpenStreetMap examples are [`examples/osm-pbf`](examples/osm-pbf) and
[`rust/examples/osm-pbf`](https://github.com/VeaaC/rapidproto/tree/main/rust/examples/osm-pbf).

---

## Compatibility & stability

Before 1.0, a C++ minor release, such as 0.4 to 0.5, may break things. A patch release never does.
The [CHANGELOG](https://github.com/VeaaC/rapidproto/blob/main/CHANGELOG.md) lists every break. From
1.0, only a major release may break or remove anything a consumer depends on. That covers the
generated API, the decode profile format, the CLI flags, the `rapidproto_generate()` contract and
the shape of the runtime headers. A minor release may deprecate something first. The CHANGELOG then
announces it under Deprecated, and the old spelling keeps working until the next major release.

For C++, RapidProto supports Linux, macOS and Windows with GCC, Clang, AppleClang and MSVC, and CI
builds and tests all of them. The C++ decoders' depth limits, which reject deeply nested input, are
sized for an 8 MB stack, the default on Linux and macOS. Windows gives an executable a 1 MB stack,
so a program that decodes untrusted input on Windows should link with a larger one
(`/STACK:8388608`).

Any Rust release before a stable one may change the API and the generated code. CI tests the Rust
crates on Linux, and builds and runs the generator on macOS and Windows too.

---

## Contributing

See [CONTRIBUTING.md](https://github.com/VeaaC/rapidproto/blob/main/CONTRIBUTING.md) for building,
the `./check.sh` quality gate and the golden tests. The design and the invariants a change must
keep are in [architecture.md](https://github.com/VeaaC/rapidproto/blob/main/architecture.md). The
Rust workspace and its benchmarks are described in
[rust/README.md](https://github.com/VeaaC/rapidproto/blob/main/rust/README.md).

## Security

See [SECURITY.md](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md) for the threat model and
how to report a vulnerability.

---

## License

RapidProto is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE), with attributions in
[`NOTICE`](NOTICE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

The runtime headers that `rapidprotoc` copies into your output directory and the Rust crates carry
the same license. The WebAssembly generator in `rapidproto-build` also contains wasi-libc and the
LLVM C++ runtime, under their own licenses, which `THIRD_PARTY_NOTICES.md` lists. The code generated
from your schema is your own work, and RapidProto claims no rights over it. The embedded Protocol
Buffers well-known type definitions are Copyright 2008 Google Inc., licensed 3-Clause BSD. Test and
benchmark dependencies, such as Catch2, protozero and prost, are not distributed.
