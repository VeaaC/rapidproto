# Consumer example

A minimal crate that decodes protobuf with RapidProto. [`build.rs`](build.rs) turns
[`proto/message.proto`](proto/message.proto) into Rust decoders at build time (`.stream(true)`, so it
emits **both** models), and [`src/main.rs`](src/main.rs) decodes the same message with each:

- **arena** — materialize the message, then read borrowed accessors (a string, a nested message, an
  enum, a repeated field), and handle invalid input;
- **streaming** — visit each field with a callback, nothing materialized, parsing into a struct via the
  macro's shared-state form; the nested `origin` is handled by the **mid-decode hybrid** — the stream
  hands it over as a reader handle, and `bytes()` feeds the arena decoder to materialize just it.

Both agree field-for-field. It builds with cargo alone: no `protoc` and no separate code generator.

## Run

```sh
cargo run -p consumer-example
```

Decoded messages also implement `serde::Serialize` behind an optional `serde` feature
(serialize-only — RapidProto stays decode-only). Enable it to serialize with any serde backend;
this example then also prints the shape as JSON:

```sh
cargo run -p consumer-example --features serde
```
