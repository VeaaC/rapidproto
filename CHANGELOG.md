# Changelog

Notable, user-visible changes per release. Pre-1.0, the MINOR version is the breaking axis (the
SemVer-0 convention): expect breaking changes between 0.x and 0.(x+1), never within a patch.

## Unreleased

### Fixed

- **Debug dumper (`--dump`): multi-file schemas no longer redefine `rp_dump_enum_name`.** When two
  generated `.rp.dump.hpp` headers both referenced an enum imported from a third file (e.g. a message and
  one of its imports both use the same enum), including them in one translation unit failed to compile
  with `redefinition of 'rp_dump_enum_name'`. The value-name helper is now emitted once, at the enum's
  definition site (like the enum type itself), and referenced elsewhere through the included dependency
  header — so cross-file dumpers compose cleanly.

## 0.3.0 — 2026-07-18

### Added

- **`rapidproto::decode_owned<T>(std::string input) -> std::shared_ptr<const T>`.** A self-contained
  arena decode: it moves the input in, decodes into a default `Arena`, and returns a `shared_ptr` that
  owns **both** the input bytes and the arena (via the aliasing constructor). Every borrowed
  `string_view` stays valid for as long as any copy of the handle lives — no external lifetime to
  manage. Returns an empty `shared_ptr` on decode failure. Use the low-level `T::decode(ByteView,
  Arena&)` when you want a custom `Arena` or hold a `string_view` you'd rather not copy.

### Removed

- **`WireReader` and the schema-less `read_message` / `read_field` pull API are removed (breaking).**
  The wire-format primitives are now free functions in the `rapidproto::wire` namespace —
  `read_varint`, `read_tag`, `read_tag_or_end`, `read_fixed32`, `read_fixed64`,
  `read_length_delimited`, `skip_value`, and `read_group` — each threading the byte cursor as a
  `(cur, end, begin)` pointer triple and returning the advanced cursor (`nullptr` on a wire error,
  with the `WireError` written to a caller-owned slot). The generated decoders already decode through
  these; the stateful `WireReader` class, the `WireField` record, the `read_message` collector, and
  `DecodeStatus::from_reader` were used only by test/dev code and are gone. Code that walked wire bytes
  by hand through `WireReader` should switch to the `rapidproto::wire` free functions.

### Changed

- **The arena decoder now borrows `string`/`bytes` from the input instead of copying them (breaking).**
  A decoded arena tree holds strings/bytes as `std::string_view`s into the input wire buffer (zero-copy;
  the arena keeps only tree structure). The tree is therefore valid only while **both** the `Arena` and
  the input buffer outlive it — previously the input was freeable right after `decode()`. Audit any code
  that freed or reused the input after `decode()`, or switch it to `decode_owned` (above). `ArenaString`
  shrank 16→12 bytes (no more SSO), so string/map field offsets change — regenerate the headers.
  `ArenaDecodeError::StringTooLong` is now reserved and never produced (an over-4 GiB input reports as
  `InputTooLarge`).
- **`raw` field-mode payloads are borrowed too, not arena-copied (breaking).** A `raw` field now stores
  the same borrowed view as a string/bytes field: a singular payload shrinks from a 16-byte copied
  `ByteView` to a 12-byte borrowed one, and a repeated `raw` accessor returns a `StringArrayView` instead
  of `ArrayView<ByteView>` (each element is still a `ByteView`, i.e. `std::string_view`). Like every
  borrowed value, a raw payload is valid only while the input outlives the tree.
- **Generated decoders reference `::rapidproto::wire::` wire readers.** The value-threaded readers the
  generated arena and streaming decoders call moved into the `rapidproto::wire` namespace (previously
  free functions prefixed `vt_`). Regenerate and upgrade the runtime header together — a decoder
  emitted against the old runtime will not compile against the new one and vice versa.

## 0.2.4 — 2026-07-10

### Changed

- **Faster field dispatch in both generated decoders, no API change.** Fields 1–15 (whose whole tag
  is a single byte) now dispatch through a raw-byte peek switch, with the field/wire split and the
  wire-type check folded into the case label; everything else falls back to the unchanged validating
  path. Generated `decode()` is also flattened (`RP_FLATTEN`), so GCC inlines the wire primitives and
  sub-decodes in a large translation unit the way Clang already did (it had been leaving ~30% more
  retired instructions on message- and skip-heavy shapes). Regenerate to pick both up. Decoded
  results are unchanged for protoc-produced input; one wire-acceptance detail changes — a
  non-canonical over-long encoding of a low field number's tag (which no conformant encoder emits) is
  now skipped rather than decoded.

- **Much faster packed scalar arrays (arena).** Packed repeated scalars are pre-sized from the field's
  wire length instead of grown one element at a time (about 2–2.5× on packed varint), and packed
  fixed-width arrays are filled with a single bulk copy on little-endian targets (about 5× on packed
  fixed); both are now ahead of protoc + `Arena`. Regenerate to pick it up. Note for capacity-limited
  consumers: a packed *varint* array is pre-sized to its byte length (an upper bound on the element
  count) and then trimmed, so its transient arena peak can briefly reach a few times the field's
  payload — size a tight `set_capacity_limit()` for that peak.

- **Faster string-heavy arena decode.** The arena's short-string copy (`ArenaString`, the inline SSO
  path) now uses overlapping fixed-width loads/stores instead of a runtime-length `memcpy`, which
  lowers to a slow generic small-copy — most on clang, where it is up to ~18% faster on a
  string-heavy whole-message decode (~3.5% on gcc). No API change; picked up by upgrading the runtime
  headers (no regeneration needed).

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
