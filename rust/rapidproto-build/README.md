# rapidproto-build

Generate [`rapidproto`] Rust decoders from `.proto` files **with cargo alone** — no `protoc`, no C++
toolchain. The generator ships as a prebuilt `wasm32-wasi` module run through the `wasmi`
interpreter (pure Rust), so a consumer's build stays cargo-only.

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

> **Status:** work in progress, developed on the `rust` branch; **not yet published**.

Licensed under Apache-2.0 (see `LICENSE` and `NOTICE`). The crate distributes a prebuilt generator
blob that statically links third-party code (wasi-libc, the LLVM C++ runtime); its license notices
and build provenance are in `THIRD_PARTY_NOTICES.md`.

[`rapidproto`]: https://crates.io/crates/rapidproto
