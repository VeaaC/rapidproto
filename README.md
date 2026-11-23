# RapidProto

[![CI](https://github.com/VeaaC/rapidproto/actions/workflows/ci.yml/badge.svg)](https://github.com/VeaaC/rapidproto/actions/workflows/ci.yml)
[![Rust CI](https://github.com/VeaaC/rapidproto/actions/workflows/rust-ci.yml/badge.svg)](https://github.com/VeaaC/rapidproto/actions/workflows/rust-ci.yml)
[![Docs](https://img.shields.io/badge/docs-veaac.github.io-2dd4bf)](https://veaac.github.io/rapidproto/)
[![Release](https://img.shields.io/github/v/release/VeaaC/rapidproto)](https://github.com/VeaaC/rapidproto/releases)
[![crates.io](https://img.shields.io/crates/v/rapidproto)](https://crates.io/crates/rapidproto)
[![docs.rs](https://img.shields.io/docsrs/rapidproto)](https://docs.rs/rapidproto)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

RapidProto generates Protobuf decoders for C++ and Rust from your `.proto` schemas. On every
payload in the benchmarks they decode faster than protoc's generated C++ code and
[prost](https://docs.rs/prost): 1.2 to 7 times as fast in C++, and 2.6 to 12 times in Rust.

On `Dataset`, a [benchmark](docs/benchmarks.md) payload of 2000 mixed records, the C++ arena
decoder has about 7 times the throughput of protoc's generated C++ code allocating from a
[`google::protobuf::Arena`](https://protobuf.dev/reference/cpp/arenas/). The Rust arena decoder
has about 12 times the throughput of prost. The [optimizations article](docs/optimizations.md)
shows where the speed comes from, one optimization at a time.

Schemas can be proto2, proto3, or editions 2023 and 2024, with groups, maps and oneofs. The
generator parses them itself and carries its own copy of the well-known types, so you don't need
protoc.

On malformed input, a decode fails with an error instead of crashing, though a streaming decode
checks only [the parts it reads](docs/streaming.md#what-a-decode-checks). On every pull request, the
C++ wire reader and both C++ decoders are fuzzed under ASan and UBSan, and the Rust arena decoders
under ASan. [SECURITY.md](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md) has the
details. For untrusted input, also limit how much memory the
[arena](docs/arena.md#limiting-memory) may take with `Arena::set_capacity_limit`. The Rust crate
documents it under [untrusted input](https://docs.rs/rapidproto/latest/rapidproto/#untrusted-input).

## Two decoders

Each schema can have two decoders. The default is the arena decoder. It decodes the whole message
into an arena you own and gives you an accessor for each field.

The streaming decoder is generated when you ask for it. It reads the input once and calls your
[handler](docs/streaming.md) for each field you selected, as the fields arrive. No message is
built. Each quick start below ends with a short streaming example.

| | Arena decoder | Streaming decoder |
|---|---|---|
| Allocation | One arena, which you own | None |
| Reading a field again | Any field, in any order, as often as you like | Only by decoding the input again |
| Memory | The whole decoded message | Whatever your handlers store |
| Best for | Code that works with the message as an object | A few fields from a large message or a stream |

One program can use both decoders of a schema. A streaming handler that receives a sub-message can
also hand its bytes to the arena decoder. In C++ it gets them from `rp_bytes()`, which
[docs/using-both-models.md](docs/using-both-models.md) walks through. In Rust it calls
[`bytes()`](https://docs.rs/rapidproto/latest/rapidproto/example/stream/weather/struct.Station.html#method.bytes).

## When not to use RapidProto

RapidProto can't build or change messages. It has no encoder and decoded messages are read-only, so
a program that does either needs another Protobuf library for that part. There's no reflection
either.

RapidProto doesn't check strings for valid UTF-8 while decoding, where prost and protoc's proto3
code do. Skipping
the check accounts for part of the speed difference. The [benchmark page](docs/benchmarks.md)
measures its cost for Rust. In Rust you can check one string when you need to, with
[`ProtoStr::to_str`](https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoStr.html#method.to_str).
In C++ the check is yours to do.

A few Protobuf [behaviors differ](docs/semantics.md). Every enum is open: a value the schema
doesn't list comes through as its number, even in an enum declared closed. Extensions aren't
decoded; they arrive as unknown fields. A proto2 `[default = X]` isn't applied. A singular
sub-message that appears twice fails the arena decode instead of being merged. gRPC is
[out of scope](https://github.com/VeaaC/rapidproto/blob/main/architecture.md#known-limitations-and-non-goals)
too: `service` and `rpc` definitions are skipped.

Decoded strings and bytes point into the input, which has to
[stay alive](docs/arena.md#the-arena) while you use them. With the arena decoder, so does the arena.
If that's awkward, [`rapidproto::decode_owned<T>`](docs/arena.md#owned-messages) in C++ and
[`Msg::decode_owned`](https://docs.rs/rapidproto/latest/rapidproto/struct.Owned.html) in Rust
bundle the input and the arena with the message.

Expect breaking changes, too. The C++ side hasn't reached 1.0, and the Rust crates are test
releases ([Compatibility](#compatibility)).

## Quick start

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

Save the schema as `person.proto` and encode a test message with protoc. Neither quick start needs
protoc after this step.

```sh
echo 'name: "Ada" id: 7 address { city: "London" }' |
    protoc --encode=example.Person -I. person.proto > person.bin
```

### C++

You need a C++17 compiler and the `rapidprotoc` generator. The
[latest release](https://github.com/VeaaC/rapidproto/releases/latest) has unsigned binaries for
Linux on x86-64 and arm64, macOS on Apple silicon and Windows on x86-64. If Gatekeeper blocks
`rapidprotoc`, clear its quarantine flag with `xattr -d com.apple.quarantine rapidprotoc`; on
Windows, click past the SmartScreen prompt. On an Intel Mac, or to build the generator yourself on
Linux or macOS, use CMake 3.21 or newer:

```sh
cmake --preset release
cmake --build --preset release --target rapidprotoc   # writes build/release/rapidprotoc
```

Put `rapidprotoc` on your `PATH`, then generate the arena decoder:

```sh
rapidprotoc -I. --out-dir=out person.proto
# writes out/person.rp.hpp, out/person.rp.common.hpp and out/rapidproto/*.hpp
```

`person.rp.hpp` is the arena decoder and `person.rp.common.hpp` holds the schema's enums. The
headers in `out/rapidproto/` are a copy of the [runtime](docs/integration.md). It's all headers on
top of the standard library, with nothing to link.

```cpp
// read_person.cpp
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
        return 1;  // malformed input: err.code says why
    }

    std::uint32_t id = person->id();
    std::string_view name = person->name();  // points into input
    if (const ex::Address* address = person->address()) {  // nullptr when absent
        std::cout << name << " (" << id << ") lives in " << address->city() << '\n';
    }
}
```

Each package gets a namespace under `rp::arena::`. If your input is a `const std::uint8_t*` and a
size, `rapidproto::byte_view(ptr, size)` makes the `rapidproto::ByteView` for you. Repeated fields,
maps, oneofs and `optional` fields
[return different types](docs/arena.md#what-each-field-kind-returns).

```sh
g++ -std=c++17 -Iout read_person.cpp -o read_person
```

From the directory with `person.bin`, `./read_person` prints `Ada (7) lives in London`.

In a CMake project, [`rapidproto_generate()`](docs/integration.md#cmake-integration) runs the
generator at build time. The same page covers FetchContent, `find_package`, and the Conan recipe and
vcpkg overlay port in this repository.

For the [streaming decoder](docs/streaming.md), pass `--stream` and the generator writes
`person.rp.stream.hpp` instead. `--arena --stream` writes both. Generate both, add
`#include "person.rp.stream.hpp"` to `read_person.cpp`, and replace everything in `main` after
`input` is read with this, which prints the name and the id:

```cpp
namespace ex = rp::stream::example;
rapidproto::DecodeStatus status = ex::Person{rapidproto::ByteView(input)}.decode(
    [](ex::Person::name, std::string_view name) { std::cout << name << '\n'; },
    [](ex::Person::id, std::uint32_t id) { std::cout << id << '\n'; });
if (!status.ok()) {
    return 1;  // malformed input
}
```

Each handler's first parameter type names the field it wants.

### Rust

You need Rust 1.91 or newer. [`rapidproto`](https://crates.io/crates/rapidproto) is the
[`no_std`](https://docs.rs/rapidproto/latest/rapidproto/#features) runtime that the generated code
uses. [`rapidproto-build`](https://crates.io/crates/rapidproto-build) generates that code from your
build script. Pin both to the same version:

```toml
# Cargo.toml
[dependencies]
rapidproto = "=0.1.0-alpha.2"

[build-dependencies]
rapidproto-build = "=0.1.0-alpha.2"
```

Generated code works only with the runtime from the same release. When the two differ, the build
usually fails with an error asking you to match them.

Put `person.proto` and `person.bin` next to `Cargo.toml`.
[`Config`](https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html) resolves
schema paths against the package root, where build scripts run.

```rust
// build.rs
fn main() {
    rapidproto_build::Config::new("crate::schema")
        .compile(&["person.proto"])
        .expect("generate decoders");
}
```

`compile` names its output after the first schema, here `person.rs` in `OUT_DIR`.
`rapidproto-build` compiles the generator with your C++ compiler. Without one it warns and falls
back to a slower WebAssembly build. The crate's notes on
[how the generator runs](https://docs.rs/rapidproto-build/latest/rapidproto_build/#how-the-generator-runs)
show the profile setting that makes the WebAssembly generator fast.

Then decode the test message:

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

`Config::new` takes the path of the module that holds the `include!`, here `crate::schema`. Arena
types sit under `<module path>::arena::<package>`, as in C++. The
[`example` module](https://docs.rs/rapidproto/latest/rapidproto/example/index.html#accessors) on
docs.rs shows what each kind of field returns. `cargo run` from the package root prints
`Ada (7) lives in London`.

Streaming in Rust needs
[`.stream(true)`](https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.stream)
on the `Config` in `build.rs`. In `src/main.rs`, swap the arena import for the `use` line below.
Then, in `main`, replace the lines between reading `input` and `Ok(())` with the decode call:

```rust
use schema::stream::example::{person_stream, Person};

Person::new(&input)?.decode(person_stream! {
    name => |name| println!("{name}"),
    id => |id| println!("{id}"),
})?;
```

The generated `person_stream!` macro pairs each field name with its handler. The
[`stream` module](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html) docs explain the
rest.

## Documentation

The [manual](docs/) is written for C++ and is also on
[the project site](https://veaac.github.io/rapidproto/). Its decoding rules and benchmarks pages
cover Rust as well.

- [The arena decoder](docs/arena.md): accessors, the `Arena` and `reset`, `decode_owned`, errors
- [The streaming decoder](docs/streaming.md): handlers and field kinds, errors, stopping a decode
  early
- [The debug dumper](docs/dumper.md): an optional `--dump` header that prints decoded messages as
  JSON-like text
- [Decoding rules](docs/semantics.md): what both decoders, in C++ and Rust, do with absent,
  repeated or unknown fields, enums and malformed input
- [Using both models](docs/using-both-models.md): both decoders in one program, and living next to
  protoc-generated code
- [Decode profiles](docs/profiles.md): dropping fields, keeping sub-messages as raw bytes and
  tracking unknown fields. Rust sets them with
  [`Config::field_modes`](https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.field_modes).
- [CLI & CMake](docs/integration.md): generator flags, CMake, package managers, cross-compiling
- [OSM PBF walkthrough](docs/osm-pbf.md): one small program per decoder reading OSM planet files
- [One optimization at a time](docs/optimizations.md): how the C++ decoders got fast, each step
  measured
- [Benchmarks](docs/benchmarks.md): the current numbers for both languages and how to reproduce
  them, with prost, rust-protobuf, quick-protobuf, femtopb and google-protobuf in the
  [Rust comparison](docs/benchmarks.md#other-rust-decoders)

The Rust guide is on docs.rs. [`rapidproto`](https://docs.rs/rapidproto) documents decoding,
including [`Debug`](https://docs.rs/rapidproto/latest/rapidproto/#debug-output) output, opt-in
[`serde::Serialize`](https://docs.rs/rapidproto/latest/rapidproto/#serde) (there's no
`Deserialize`), and
[`Arena::with_buffer`](https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html#method.with_buffer),
which lets a decode run without an allocator once the default `alloc` feature is off.
[`rapidproto-build`](https://docs.rs/rapidproto-build) documents generating. That includes the
`rapidproto-rust` command, for projects that commit the generated code instead of generating it in
`build.rs`.

[examples/consumer](examples/consumer) and
[rust/examples/consumer](https://github.com/VeaaC/rapidproto/tree/main/rust/examples/consumer) each
decode one schema with both decoders. The C++ one builds with `rapidproto_generate()` and adds a
second build with a decode profile. For something larger, [examples/osm-pbf](examples/osm-pbf) and
[rust/examples/osm-pbf](https://github.com/VeaaC/rapidproto/tree/main/rust/examples/osm-pbf) read
OpenStreetMap files.

## Compatibility

C++ and Rust have separate releases. The badges at the top show the current version of each.
Before 1.0, a C++ minor release may break things but a patch release won't.
[CHANGELOG.md](https://github.com/VeaaC/rapidproto/blob/main/CHANGELOG.md) lists the changes in
every C++ release and opens with the full compatibility policy, including what changes at 1.0.

CI builds and tests the C++ side on Linux, macOS and Windows with GCC, Clang, AppleClang and MSVC.
The C++ arena decoder stops at [100 levels of nesting](docs/arena.md#depth-limits), a limit sized
for the 8 MB stack of Linux and macOS. Windows gives a program 1 MB, so link programs that decode
untrusted input there with `/STACK:8388608`.

The Rust crates are test releases. Until they're stable, any release may change their API and the
code the generator writes. CI runs the full Rust test suite on Linux. On macOS and Windows it builds
the generator and runs it in a smoke test.

## Contributing and security

- [CONTRIBUTING.md](https://github.com/VeaaC/rapidproto/blob/main/CONTRIBUTING.md): building
  RapidProto, running the gate (`./check.sh`) and updating the golden files
- [architecture.md](https://github.com/VeaaC/rapidproto/blob/main/architecture.md): the internals
  and the invariants a change must keep
- [rust/README.md](https://github.com/VeaaC/rapidproto/blob/main/rust/README.md): the Cargo
  workspace and the Rust benchmarks
- [SECURITY.md](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md): the threat model and
  which releases get fixes

Please report vulnerabilities through GitHub's private vulnerability reporting, not in a public
issue.

## License

RapidProto is licensed under [Apache-2.0](LICENSE), and so are the runtime headers `rapidprotoc`
copies into your output and both Rust crates. RapidProto claims no rights in the code generated from
your schemas.

The generator includes Google's well-known type `.proto` files (copyright 2008 Google Inc.,
BSD-3-Clause), and the wasm generator in `rapidproto-build` bundles wasi-libc and the LLVM C++
runtime under their own licenses. [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) list them. Test and benchmark dependencies such as
Catch2, protozero and prost aren't distributed with RapidProto.
