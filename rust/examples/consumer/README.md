# Consumer example

A minimal crate that decodes protobuf with RapidProto. [`build.rs`](build.rs) turns
[`proto/message.proto`](proto/message.proto) into Rust decoders at build time (`.stream(true)`, so it
emits **both** models), and [`src/main.rs`](src/main.rs) decodes the same message with each:

- **arena** — materialize the message, then read borrowed accessors (a string, a nested message, an
  enum, a repeated field), print it with `Debug`, and handle invalid input;
- **streaming** — visit each field with a callback, nothing materialized, parsing into a struct via the
  macro's shared-state form; the nested `origin` is handled by the **mid-decode hybrid** — the stream
  hands it over as a reader handle, and `bytes()` feeds the arena decoder to materialize just it.

Both agree field-for-field. It needs no `protoc` and no separate code generator.

## Run

```sh
cargo run -p consumer-example
```

The generator can also implement `serde::Serialize` for decoded messages (`Config::serde`). RapidProto
only decodes, so there is no `Deserialize`. This example turns it on with its optional `serde` feature,
and then also prints the shape as JSON:

```sh
cargo run -p consumer-example --features serde
```
