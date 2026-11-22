# rapidproto-build

`rapidproto-build` generates Rust decoders from Protobuf schemas while your crate builds, without
protoc. You call it from `build.rs`, and your crate includes the Rust file it writes. The decoders
in that file run on the [`rapidproto`][runtime] runtime.

It accepts proto2, proto3, and editions 2023 and 2024. The well-known types are built in, so
`import "google/protobuf/timestamp.proto";` works with no such file on disk.

This version is a test release, and the next one may change the [`Config`][config] API or the
generated code.

## Setup

```toml
# Cargo.toml
[dependencies]
rapidproto = "=0.1.0-alpha.2"

[build-dependencies]
rapidproto-build = "=0.1.0-alpha.2"
```

Both lines pin with `=` because generated code needs the runtime from its own release. A mismatched
pair usually fails to build, with `use the same version of rapidproto and rapidproto-build` as the
first error.

```proto
// proto/my_schema.proto
syntax = "proto3";
package shop;

message Order {
  string id = 1;
  repeated int64 quantities = 2;
}
```

```rust
// build.rs
fn main() {
    rapidproto_build::Config::new("crate::schema")
        .include("proto")
        .compile(&["proto/my_schema.proto"])
        .expect("generate decoders");
}
```

`compile` writes the generated code to `OUT_DIR` as `my_schema.rs`, named after the first schema in
the list. The schema path is relative to the package root. Imports resolve against the `include`
directories instead.

```rust
// src/main.rs
mod schema {
    include!(concat!(env!("OUT_DIR"), "/my_schema.rs"));
}

fn main() -> Result<(), rapidproto::DecodeError> {
    let input = b"\x0a\x02A7\x12\x02\x03\x04"; // encodes id "A7" and quantities [3, 4]
    let arena = rapidproto::Arena::new();
    let order = schema::arena::shop::Order::decode(input, &arena)?;
    println!("{order:?}");
    Ok(())
}
```

The `include!` has to sit at the [module path][new] you gave `Config::new`, here `crate::schema`, or
the generated code fails to compile. For `package shop;` the [arena decoders][arena] are in
`schema::arena::shop`. `cargo run` prints:

```text
Order { id: "A7", quantities: [3, 4] }
```

To commit the generated code to your repository instead, install the [`rapidproto-rust`][docs]
command from this crate. It generates what `compile` would, outside the build.

## Streaming, decode profiles and serde

- [`.stream(true)`][stream] generates the streaming decoders as well.
- [`.field_modes(...)`][field-modes] applies a decode profile, which can drop fields, keep
  sub-messages as raw bytes or flag messages that had unknown fields.
- [`.serde(true)`][serde] adds `serde::Serialize` impls. They need `rapidproto`'s `serde` feature.

## Native and wasm generators

The generator is a C++ program. By default, the build script of this crate builds it with your
C++17 compiler, which adds ten seconds or more to a clean build. If there's no compiler, or the C++
build fails, the build script falls back to the [wasm generator][how], a prebuilt WebAssembly copy
that runs in the wasmi interpreter. Either way, the generated code is identical.

A fallback shows up as a warning, `rapidproto-build uses the slower wasm generator`, followed by
the reason the C++ build failed.

With a working compiler you can drop the fallback. Setting
`default-features = false, features = ["native"]` leaves out the wasm generator's roughly 60
dependency crates, but then a machine without a compiler can't build your crate. With only `wasm`
enabled, the build never tries the C++ compiler and doesn't warn.

Cargo compiles the interpreter at opt-level 0, as it does all build-time code, even for a release
build. That accounts for most of the wasm generator's slowness. These lines turn optimization on for
it. They belong in the workspace root's `Cargo.toml`, since Cargo ignores profiles in any other
manifest.

```toml
# Cargo.toml at the workspace root
[profile.dev.build-override]
opt-level = 3

[profile.release.build-override]
opt-level = 3
```

Timed on one machine, generating each schema took:

| Schema | Wasm, as Cargo builds it | Wasm, with the setting | Native |
|---|---|---|---|
| `my_schema.proto` above | 0.5 s | 0.04 s | 0.001 s |
| OpenStreetMap's `osmformat.proto`, 9 KB | 4.5 s | 0.2 s | 0.005 s |
| Google's `pubsub.proto`, 114 KB plus imports | 50 s | 1.9 s | 0.04 s |
| Google's `compute.proto`, 4.3 MB plus imports | 13 min | 28 s | 0.6 s |

With the setting, build scripts, proc macros and everything they depend on are compiled with
optimizations, which made a clean build more than twice as long. The native generator doesn't need
it.

## License

The license is Apache-2.0 ([LICENSE][license], [NOTICE][notice]). The package also contains Google's
well-known type definitions and the wasm generator, which has wasi-libc and the LLVM C++ runtime
linked in. Their licenses are in [THIRD_PARTY_NOTICES.md][third-party]. RapidProto claims no rights
in the code generated from your schemas.

[runtime]: https://crates.io/crates/rapidproto
[arena]: https://docs.rs/rapidproto
[config]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html
[new]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.new
[docs]: https://docs.rs/rapidproto-build
[stream]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.stream
[field-modes]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.field_modes
[serde]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.serde
[how]: https://docs.rs/rapidproto-build/latest/rapidproto_build/#how-the-generator-runs
[license]: https://github.com/VeaaC/rapidproto/blob/main/rust/rapidproto-build/LICENSE
[notice]: https://github.com/VeaaC/rapidproto/blob/main/rust/rapidproto-build/NOTICE
[third-party]: https://github.com/VeaaC/rapidproto/blob/main/rust/rapidproto-build/THIRD_PARTY_NOTICES.md
