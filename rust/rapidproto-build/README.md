# rapidproto-build

Generates [`rapidproto`] Rust decoders for Protobuf schemas in a build script, without `protoc`.

> This is a test release. The API and the generated code will change.

Pin both crates to the same version with `=`, because the generated code targets the matching
runtime:

```toml
# Cargo.toml
[dependencies]
rapidproto = "=0.1.0-alpha.1"

[build-dependencies]
rapidproto-build = "=0.1.0-alpha.1"
```

Generate the decoders from a build script:

```rust
// build.rs
fn main() {
    rapidproto_build::Config::new("crate::schema")
        .include("proto")
        .compile(&["proto/my_schema.proto"])
        .expect("generate decoders");
}
```

`compile` writes `my_schema.rs` to `OUT_DIR`, named after the first entry. Include it at the path
you passed to `Config::new`, and decode with it:

```rust
mod schema {
    include!(concat!(env!("OUT_DIR"), "/my_schema.rs"));
}

// For `package shop;` and `message Order` in my_schema.proto:
let arena = rapidproto::Arena::new();
let order = schema::arena::shop::Order::decode(&input, &arena)?;
```

The [documentation] covers stream decoders, decode profiles and `serde`. The `rapidproto-rust`
command-line tool generates the same code, for projects that commit it.

## How the generator runs

The generator is written in C++. The crate's build script compiles it, which needs a C++17
compiler. Without one, the crate uses a copy of the generator built as WebAssembly and run in the
`wasmi` interpreter, and `Config::compile` warns. Both produce the same output.

The `native` and `wasm` features choose between them, and both are on by default. If you have a
C++ compiler, `default-features = false, features = ["native"]` avoids building the interpreter.
To use the wasm generator on purpose, without the warning, enable only `wasm`.

Cargo builds build-time code without optimization, which makes the interpreter slow. It takes
seconds for a small schema and minutes for a very large one. If you use the wasm generator,
optimize build-time code in your workspace:

```toml
[profile.dev.build-override]
opt-level = 3
[profile.release.build-override]
opt-level = 3
```

Licensed under Apache-2.0 (see `LICENSE` and `NOTICE`). The crate ships the prebuilt wasm
generator, which statically links wasi-libc and the LLVM C++ runtime. The crate also contains the
Protobuf well-known type definitions. `THIRD_PARTY_NOTICES.md` has their license notices and the
build provenance.

[`rapidproto`]: https://crates.io/crates/rapidproto
[documentation]: https://docs.rs/rapidproto-build
