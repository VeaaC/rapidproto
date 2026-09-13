# RapidProto documentation

RapidProto compiles a `.proto` schema into header-only C++ decoders: no runtime library to link and
no third-party dependencies. It is decode-only - no serialization, no reflection - and generates two
models from one schema: an arena decoder that materializes a message tree, and a streaming decoder
that hands each field to a callback. Both validate untrusted wire input and never crash on malformed
bytes. On a mixed real-world payload the arena decoder decodes about 7× as fast as `protoc` + Arena
([benchmarks](benchmarks.md)).

Start with the [quick start](../README.md#quick-start); [how it got fast](optimizations.md) walks
the optimizations one at a time. The pages below are the topic-by-topic reference.

- [arena.md](arena.md) - the arena decoder: accessors, the `Arena`, `decode_owned`, error handling
- [streaming.md](streaming.md) - the streaming decoder: field tags, the consumption patterns, aborting
- [dumper.md](dumper.md) - the `--dump` debug dumper: JSON-like inspection text, `DumpOptions`
- [semantics.md](semantics.md) - the shared rules: lifetimes, validation & trust, presence, open enums,
  duplicate fields
- [using-both-models.md](using-both-models.md) - both models in one TU; coexisting with protoc
- [profiles.md](profiles.md) - decode profiles (`drop` / `raw`) and unknown-field detection (arena)
- [integration.md](integration.md) - the `rapidprotoc` CLI reference and the CMake helper
- [osm-pbf.md](osm-pbf.md) - real-world walkthrough: OSM's planet format with both models
- [optimizations.md](optimizations.md) - how the decoders got fast: seven optimizations,
  re-enabled one at a time and measured
- [benchmarks.md](benchmarks.md) - the numbers and how to reproduce them

Contributor docs live at the repository root: [architecture.md](https://github.com/VeaaC/rapidproto/blob/main/architecture.md) (internals,
invariants, design rationale), [CONTRIBUTING.md](https://github.com/VeaaC/rapidproto/blob/main/CONTRIBUTING.md),
[SECURITY.md](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md), and [CHANGELOG.md](https://github.com/VeaaC/rapidproto/blob/main/CHANGELOG.md).
