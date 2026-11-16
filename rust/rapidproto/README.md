# rapidproto

Runtime for the Protobuf decoders that [`rapidproto-build`] generates.

> This is a test release. The API and the generated code will change.

RapidProto only decodes. `rapidproto-build` generates decoders from your schema at build time,
without `protoc`, and the generated code uses this crate. Add `rapidproto` to your dependencies and
`rapidproto-build` to your build dependencies, both pinned to the same version with `=`.

An arena decoder decodes the whole message into an [`Arena`]. `string` and `bytes` values borrow
from the input, and a repeated scalar field is a slice such as `&[i64]`. A stream decoder builds no
message. It passes the fields you select to your handlers and skips the rest.

The crate is `no_std`. The default `alloc` feature lets the arena grow on the heap. Without it, the
arena uses only a buffer you provide. With the optional `serde` feature and `rapidproto-build`'s
`Config::serde`, decoded messages implement `Serialize`. Without `serde`, the crate has no
dependencies.

The [documentation] has examples.

Licensed under Apache-2.0 (see `LICENSE` and `NOTICE`).

[`Arena`]: https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html
[`rapidproto-build`]: https://crates.io/crates/rapidproto-build
[documentation]: https://docs.rs/rapidproto
