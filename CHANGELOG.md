# Changelog

Notable, user-visible changes per release. Pre-1.0, the MINOR version is the breaking axis (the
SemVer-0 convention): expect breaking changes between 0.x and 0.(x+1), never within a patch.

## 0.2.3 — 2026-07-06

### Changed

- **Faster generated decode, no API change.** The wire reader's `read_tag` gained a fused
  1-byte-tag fast path (tags are the most frequent varint), and both generated decoders (arena +
  streaming) now drive their decode loops with a fused `read_tag_or_end`: one bounds check per
  field instead of `at_end()` + `read_tag()`, with the tag held as a value rather than
  `std::optional`. Together these close most of the throughput gap to mapbox/protozero on nested
  and message-heavy payloads (≈2× faster nested-message streaming decode on gcc; at or above
  protozero on clang), and speed the arena decoder comparably. Regenerate to pick it up; decoded
  results and the generated API are unchanged.

## 0.2.2 — 2026-07-05

### Changed

- Multiple `rapidprotoc` entries (and a CMake target's `PROTOS`) now generate as **one batch**:
  shared imports parse once, every file generates exactly once, `--depfile` covers the whole
  batch in one rule, and a `--field-modes` profile resolves against the union of all entries'
  schemas — one global profile can span schemas living in different entry files, while a name
  unknown across the whole batch is still a hard error.

## 0.2.1 — 2026-07-04

### Added

- Every generated enum carries `rp_known_min` / `rp_known_max`: the schema's declared value range
  (negatives included, aliases collapsed), so a consumer can range-check or size against the known
  values without hand-tracking the schema.
- Streaming decoders expose `rp_bytes()`: the exact undecoded span (a LEN payload, or a
  group/DELIMITED body without its framing). A callback can hand a sub-decoder's span straight to
  the arena model's `decode()` — stream the outer message, materialize chosen sub-messages.
- Arena decode profiles: `--field-modes=<file>` / `--drop=<name>` / `--raw=<name>` (CMake:
  `FIELD_MODES` / `DROP` / `RAW`) select, per field or per type, whether the arena decoder
  materializes a field, **drops** it (no storage, no accessor — reading it is a compile error), or
  keeps a message field **raw** (its payload as an arena-copied `ByteView` — one per element for
  repeated fields — which the field type's own `decode()` accepts directly, deferring that decode
  until the consumer actually wants the tree). Profiled headers carry the profile in their banner
  and wrap the types in an `inline namespace rp_modes_<id>` keyed to a content hash, so TUs
  generated under different profiles hold distinct types — exchanging them is a link error, never
  a silent ODR violation.

## 0.2.0 — 2026-07-03

0.1.0 predates the unified CLI and the two-model coexistence design, so 0.2.0 is effectively a new
public surface; treat this entry as its definition rather than a delta.

### Breaking

- **Generated arena API.** Explicit-presence scalar/string/enum fields return `std::optional<T>`;
  `has_<field>()` is gone, and a proto2 `[default = X]` is no longer materialized (apply it via
  `value_or`). Oneofs are read with a typed visitor (`msg->pick(handlers...)`) instead of a case enum
  plus per-member getters; oneof handlers must return `void`. Sub-message accessors are `const T*`.
- **Stricter compile-time dispatch.** A callback that matches no field of the message it is passed
  to (typically pasted from another message's `decode()`) is now a compile error in both models, as
  is a wrong-shape `std::monostate` or `UnknownField` handler. Code that relied on stray callbacks
  being silently ignored must remove them.
- **One CLI.** `rapidprotoc` (with `--arena` / `--stream` / both) replaces the per-model
  generators; outputs are `<stem>.rp.hpp`, `<stem>.rp.stream.hpp`, and the shared
  `<stem>.rp.common.hpp`. The generated entry point is `decode()`.
- **CLI behavior.** Output is silent on success (`-v`/`--verbose` restores the per-file `wrote`
  lines) and unknown flags are errors (exit 2) instead of being treated as entry files.
- **CMake.** Install/export rules, `-Werror`, and the test suite are top-level-only by default
  (`RAPIDPROTO_INSTALL`, `RAPIDPROTO_WERROR`, `RAPIDPROTO_BUILD_TESTS`), so add_subdirectory /
  FetchContent consumers are unaffected by them.

### Added

- Two decode models for one schema, usable in one translation unit: the arena object-tree decoder
  and the streaming callback decoder, sharing one C++ enum type per proto enum
  (`--namespace-prefix` for protoc coexistence).
- `rapidprotoc --help` / `--version`; generated files carry the generator version in their banner.
- A missing-import error now says how many `-I` paths were searched.
- `rapidproto_generate()` targets propagate C++17; the installed package's version check is
  architecture-independent (host-tool + header-only runtime), and LICENSE/NOTICE install with it.
- `default` and `release` CMake presets (system compiler, no `-Werror`).
- Editions 2024 decoder coverage; editions 2023 DELIMITED and presence semantics decode-tested from
  real bytes.

### Fixed / hardened

- A signed-overflow UB on `-9223372036854775808` in schema parsing; unbounded generator recursion
  over message-reference chains (now depth-capped with graceful pointer-storage degradation) and in
  sibling ordering (now iterative).
- `rapidprotoc` no longer aborts on an unwritable `--out-dir`, and a failed header write is a
  reported error (exit 1) instead of a silent success.
- A duplicate type FQN fails schema analysis with a clear error instead of generating
  duplicate-class C++.
- Fuzzing now drives the wire-exhaustive, many-required, and unknown-present decoders (arena) and
  recursive sub-decoders (streaming); crash reproducers are uploaded from CI.

## 0.1.0 — 2026-06-28

Initial tag: schema front-end (proto2/proto3/editions), wire reader, and the first generated
decoders.
