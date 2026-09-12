# RapidProto documentation

The user manual, one page per topic. The quick start is in the [project README](../README.md).

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
