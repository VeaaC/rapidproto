# rapidproto-build

Generates [`rapidproto`] Rust decoders for Protobuf schemas in a build script, without `protoc`.

> This is a test release. The API and the generated code will change.

Add both crates at the same version, because the generated code targets the matching runtime:

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
you passed to `Config::new`:

```rust
// src/lib.rs
mod schema {
    include!(concat!(env!("OUT_DIR"), "/my_schema.rs"));
}
```

The [documentation] covers the options: stream decoders, decode profiles and `serde`. The
`rapidproto-rust` command-line tool generates the same code, for projects that commit it.

## How the generator runs

The generator is written in C++. The crate's build script compiles it, which needs a C++17
compiler. Without one, the crate uses a copy of the generator built as WebAssembly and run in the
`wasmi` interpreter, and `Config::compile` warns. Both produce the same output.

The `native` and `wasm` features choose between them, and both are on by default. If you have a
C++ compiler, `default-features = false, features = ["native"]` avoids building the interpreter.
To use the wasm generator on purpose, without the warning, enable only `wasm`.

Cargo builds build scripts without optimization, which makes the interpreter slow: seconds for a
small schema, minutes for a very large one. If you use the wasm generator, optimize build-time code
in your workspace:

```toml
[profile.dev.build-override]
opt-level = 3
[profile.release.build-override]
opt-level = 3
```

Licensed under Apache-2.0 (see `LICENSE` and `NOTICE`). The crate ships the prebuilt wasm
generator, which statically links third-party code, wasi-libc and the LLVM C++ runtime. Their
license notices and the build provenance are in `THIRD_PARTY_NOTICES.md`.

[`rapidproto`]: https://crates.io/crates/rapidproto
[documentation]: https://docs.rs/rapidproto-build
