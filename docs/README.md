# RapidProto documentation

These pages are the C++ manual for RapidProto. The [README](../README.md) gives the overview and
the [quick starts](../README.md#quick-start). The Rust guide is the documentation of the
[`rapidproto`](https://docs.rs/rapidproto) and [`rapidproto-build`](https://docs.rs/rapidproto-build)
crates on docs.rs. The benchmarks cover both languages.

- [The arena decoder](arena.md) covers accessors, the `Arena`, `decode_owned` and error handling.
- [The streaming decoder](streaming.md) covers field tags, the three ways to consume fields, and
  stopping early.
- [The debug dumper](dumper.md) covers the `--dump` option, its JSON-like text and `DumpOptions`.
- [Shared semantics](semantics.md) covers the rules both models follow, such as lifetimes,
  validation, presence, open enums and duplicate fields.
- [Using both models](using-both-models.md) covers both models in one translation unit, streaming
  into the arena decoder, and coexisting with `protoc`.
- [Decode profiles](profiles.md) covers dropping fields, keeping sub-messages as raw bytes, and
  detecting unknown fields in the arena decoder.
- [CLI & CMake](integration.md) covers the `rapidprotoc` command line and the CMake helper.
- [OSM PBF walkthrough](osm-pbf.md) decodes OpenStreetMap's planet format with both models.
- [One optimization at a time](optimizations.md) shows how the decoders got fast, with seven
  optimizations re-enabled in turn and measured.
- [Benchmarks](benchmarks.md) has the numbers and how to reproduce them.

Other documents are on GitHub.
[architecture.md](https://github.com/VeaaC/rapidproto/blob/main/architecture.md) describes the
internals, invariants and design.
[CONTRIBUTING.md](https://github.com/VeaaC/rapidproto/blob/main/CONTRIBUTING.md) covers building and
testing, and [SECURITY.md](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md) covers the
threat model. [CHANGELOG.md](https://github.com/VeaaC/rapidproto/blob/main/CHANGELOG.md) lists the
changes in each C++ release.
[rust/README.md](https://github.com/VeaaC/rapidproto/blob/main/rust/README.md) describes the Rust
workspace.
