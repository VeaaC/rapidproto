# Rust consumer example

A runnable, **cargo-only** consumer of the generated Rust decoders — no C++ toolchain. It mirrors the
C++ [`examples/consumer`](../../../examples/consumer) and decodes the identical `demo.Shape` bytes,
over the same schema ([`examples/consumer/proto/message.proto`](../../../examples/consumer/proto/message.proto),
which imports [`types.proto`](../../../examples/consumer/proto/types.proto) so the import closure is
exercised).

- [`build.rs`](build.rs) calls `rapidproto_build::Config::compile()`, which runs the committed wasm
  generator blob (through the `wasmi` interpreter) and writes the decoders to `OUT_DIR`.
- [`src/main.rs`](src/main.rs) `include!`s that generated file and decodes a buffer with the arena
  object-tree API: a cross-file message reference, the shared enum, a string, and a packed repeated
  field, plus a truncated-input error case.

## Running

```sh
cargo run -p consumer-example
```

The only build input is cargo and this workspace: the generator ships as a prebuilt blob in the
`rapidproto-build` build-dependency, so nothing here needs `protoc`, CMake, or a C++ compiler.
