# Changelog

Notable, user-visible changes per release. Pre-1.0, the MINOR version is the breaking axis (the
SemVer-0 convention): expect breaking changes between 0.x and 0.(x+1), never within a patch.

## Unreleased

### Added

- Every generated enum carries `rp_known_min` / `rp_known_max`: the schema's declared value range
  (negatives included, aliases collapsed), so a consumer can range-check or size against the known
  values without hand-tracking the schema.

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
