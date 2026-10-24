# rapidproto-build

Generate [`rapidproto`] Rust decoders from `.proto` files in a build script, without `protoc`.

Use it from a build script:

```rust
// build.rs
fn main() {
    rapidproto_build::Config::new()
        .include("proto")
        .compile(&["proto/my_schema.proto"])
        .expect("generate decoders");
}
```

```toml
# Cargo.toml
[dependencies]
rapidproto = "..."

[build-dependencies]
rapidproto-build = "..."
```

The decoders land in `OUT_DIR`; `include!` them from your crate. A CLI (`rapidproto-rust`) mirrors the
same generation surface for committing generated code instead.

## How the generator runs

The crate's build script compiles the generator natively from the C++ sources the crate ships,
which needs a C++17 compiler. Without one, the crate uses a copy of the generator embedded as
WebAssembly and run in the `wasmi` interpreter, and `Config::compile` warns. Both produce the same
output.

The `native` and `wasm` features choose between them; both are on by default. With a C++ compiler
at hand, `default-features = false, features = ["native"]` builds the least: no interpreter. To
use the wasm generator on purpose, without the warning, enable only `wasm`.

Cargo builds build scripts unoptimized by default, which makes the interpreter slow: seconds for a
small schema, minutes for a very large one. If you use the wasm generator, optimize build-time code
in your workspace:

```toml
[profile.dev.build-override]
opt-level = 3
[profile.release.build-override]
opt-level = 3
```

> **Status:** work in progress, developed on the `rust` branch; **not yet published**.

Licensed under Apache-2.0 (see `LICENSE` and `NOTICE`). The crate distributes the prebuilt wasm
generator, which statically links third-party code (wasi-libc, the LLVM C++ runtime); its license
notices and build provenance are in `THIRD_PARTY_NOTICES.md`.

[`rapidproto`]: https://crates.io/crates/rapidproto
