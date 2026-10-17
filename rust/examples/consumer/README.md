# Consumer example

A minimal crate that decodes protobuf with RapidProto. [`build.rs`](build.rs) turns
[`proto/message.proto`](proto/message.proto) into Rust decoders at build time, and
[`src/main.rs`](src/main.rs) includes them and decodes a message — reading a string, a nested
message, an enum, and a repeated field, and handling invalid input.

It builds with cargo alone: no `protoc` and no separate code generator to install.

## Run

```sh
cargo run -p consumer-example
```
