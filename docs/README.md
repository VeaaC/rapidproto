# RapidProto documentation

These pages are the C++ manual for RapidProto. The [README](../README.md) gives the overview and
the [quick starts](../README.md#quick-start). The Rust guide is the documentation of the
[`rapidproto`](https://docs.rs/rapidproto) and [`rapidproto-build`](https://docs.rs/rapidproto-build)
crates on docs.rs. The decoding rules and the benchmarks cover both languages.

- [The arena decoder](arena.md) covers accessors, the `Arena`, `decode_owned` and error handling.
- [The streaming decoder](streaming.md) covers handlers, catch-alls, sub-messages, unknown fields,
  errors and what a decode checks.
- [The debug dumper](dumper.md) covers the `--dump` option, its JSON-like text and `DumpOptions`.
- [Decoding rules](semantics.md) covers what both decoders do with malformed input, absent and
  required fields, enums, fields sent twice and unknown fields.
- [Using both models](using-both-models.md) covers both models in one translation unit, streaming
  into the arena decoder, and coexisting with `protoc`.
- [Decode profiles](profiles.md) covers dropping fields, keeping sub-messages as raw bytes, and
  detecting unknown fields in the arena decoder.
- [CLI & CMake](integration.md) covers the `rapidprotoc` command line and the CMake helper.
- [OSM PBF walkthrough](osm-pbf.md) decodes OpenStreetMap's planet format with both models.
- [One optimization at a time](optimizations.md) shows how the decoders got fast, with seven
  optimizations re-enabled in turn and measured.
- [Benchmarks](benchmarks.md) has the numbers and how to reproduce them.
- [Benchmark method](benchmark-method.md) explains how the published numbers are made and how far
  they move between runs.
- [Running the benchmarks](running-benchmarks.md) shows how to build and run the benchmarks and
  compare two builds.

Other documents are on GitHub.
[architecture.md](https://github.com/VeaaC/rapidproto/blob/main/architecture.md) describes the
internals, invariants and design.
[CONTRIBUTING.md](https://github.com/VeaaC/rapidproto/blob/main/CONTRIBUTING.md) covers building and
testing, and [SECURITY.md](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md) covers the
threat model. [CHANGELOG.md](https://github.com/VeaaC/rapidproto/blob/main/CHANGELOG.md) lists the
changes in each C++ release.
[rust/README.md](https://github.com/VeaaC/rapidproto/blob/main/rust/README.md) describes the Rust
workspace.
