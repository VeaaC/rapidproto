# rapidproto

Decode-only Protocol Buffers runtime for the decoders RapidProto generates. Messages decode into a
bump [`Arena`] and the generated types are lightweight **views that borrow it** — a decode copies
nothing, and scalar/packed fields are read straight out of the arena.

Decoders are generated at build time by the companion [`rapidproto-build`] crate (no `protoc`), or
from committed generated code.

- **Zero-copy, arena-based.** Repeated scalar columns come back as `&[i64]`, strings as
  `&ProtoStr`.
- **`no_std`-ready.** `default = ["std"]`; `alloc` alone is the no_std + allocator tier; neither
  feature is the no_std-no-alloc embedded tier (a caller-seeded arena). No third-party deps in the
  default build.
- **Optional `serde`.** Off by default; enables generated serialize-only `Serialize` impls.

> **Status:** work in progress, developed on the `rust` branch; the public API is not yet stable and
> the crate is **not yet published**.

Licensed under Apache-2.0 (see `LICENSE` and `NOTICE`).

[`Arena`]: https://docs.rs/rapidproto
[`rapidproto-build`]: https://crates.io/crates/rapidproto-build
