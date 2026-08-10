# Changelog

Notable, user-visible changes per release. Pre-1.0, the MINOR version is the breaking axis (the
SemVer-0 convention): expect breaking changes between 0.x and 0.(x+1), never within a patch.

## Unreleased

### Changed

- **`Callbacks` is a usable field name again**, and `RpFs` / `RpT` / `RpTag` never break a schema.
  The generated code's own template parameters and aliases are now spelled `rp_Callbacks`, `rp_Fs`,
  `rp_T` and `rp_Tag` — inside the `rp_` prefix the generator already reserves — so none of them
  needs a reserved word of its own. `Callbacks` came off the reserved list as a result; the other
  three were never on it, which is why `oneof RpFs` failed to compile rather than being escaped.

  The principle: reserve a name only when it is public API a user writes and so cannot take the
  prefix. That leaves `Value`, `Key`, `kNumber`, `kName`, `decode`, plus the two namespaces `std`
  and `rapidproto`.

  Only the streaming decoder's *declaration* changes shape (`template <class... rp_Callbacks>`);
  callbacks are deduced, so calling code is unaffected. **Regenerate** to pick it up.

### Fixed

- **Proto names that collide with what the generated code or its runtime defines are now escaped**
  with a trailing `_`. All of the shapes below are protoc-valid and produced a header that did not
  compile; they were found together.

  - **`std` wherever it becomes a C++ *type*** — a message or enum at any nesting depth, a package
    component after the first, a streaming field or map tag struct, an arena oneof-member tag
    struct. It is the one namespace the generated code names unrooted (`std::int32_t`,
    `std::string_view`, `std::optional`), so a type of that name shadowed it from inside the class.
  - **A package named `std`**, which emitted `namespace std { … }` — undefined behaviour per
    [namespace.std] that compiles *without a diagnostic*, so nothing would have told you.
  - **A package named `rapidproto`**, which merged the schema's types into the runtime's own
    namespace; any that shared a name with something the runtime declares (`wire`, `Arena`,
    `ByteView`, `WireType`, `dump`, `ArrayView`, … — not a closed list) clashed with it.
  - **`RP_FLATTEN` and `RP_NOINLINE`**, the runtime's two object-like macros, in any role. The rule
    is the whole `RP_` prefix, matching the existing `rp_` one, so it holds for macros added later.
  - **A oneof named `EOF`** or another common macro. arenagen synthesizes its visit-tag struct from
    the raw proto name, so `sanitize()` never saw it and the preprocessor ate `struct EOF`. This one
    gets a deliberately narrow escape — only names that would macro-expand — because a tag struct
    may legitimately be called `Value`, `Key` or `decode`, and escaping the full reserved set there
    renames the public tag struct of 77 corpus schemas for no compile benefit.

  **Renames of API that already compiled.** One reserved set serves every role, and the `rp_`/`RP_`
  prefixes are reserved wholesale, so these change even though they never broke: `std` as an arena
  or dump accessor (`msg.std()` → `msg.std_()`), an enum value, or a oneof; `rapidproto` in *any*
  role; and any `RP_…` name in any role, whether or not it is a macro. Wire names and proto names
  are untouched, but a renamed **enum value** does change the `--dump` text output, which prints the
  C++ identifier.

  **Regenerate only if your schema uses one of these spellings** — all 8018 schemas in the
  real-world corpus generate byte-identical output before and after this change.

- **Arena headers no longer fail to compile when a nested type shadows the name it is nested in.**
  The out-of-line `rp_decode_into` / `decode` definitions named themselves with a qualifier assembled
  from local names (`A::B::rp_decode_into(A::B& out, …)`). Naming a definition enters that class's
  scope for the rest of the declarator, so the parameter type `A::B` was resolved from *inside*
  `A::B` — where a nested `A` shadows the top-level one and the header stops compiling. Both shapes
  are protoc-valid and both failed on gcc and clang:

  ```proto
  message A { message B { enum A { A_X = 0; } optional A e = 1; } optional B b = 1; }
  message P { message P { message P { optional int32 x = 1; } } }   // three levels
  ```

  The definition and its `out` parameter are now both spelled with the absolute
  `::pkg::A::B::…` name (`::A::B::…` without a package), which cannot be shadowed. **Regenerate** to
  pick this up: every out-of-line definition in every arena header changes, though only these
  schemas changed behaviour. Streaming and dump headers are unaffected. No schema in the ~8000-file
  real-world corpus hits it, and the corpus gate only generates headers rather than compiling them,
  which is why it went unnoticed.

- **An edition RapidProto has no feature defaults for is now refused instead of silently decoded by
  2023's rules.** `edition = "2025"` (or any unrecognized value) was accepted and given the
  2023/2024 defaults. That is invisible today because both known editions share every
  decode-relevant default — and it is precisely why it was worth fixing: the day an edition changes
  one, such a schema would have decoded by the wrong rules with no diagnostic. It now fails with
  `unknown edition "…"`, naming the editions that are known and pointing at the declaration, the way
  `protoc` does. The known set is one list in `src/features.cpp`.

  Adding an edition is therefore a deliberate act: check its defaults against the table in
  [architecture.md](architecture.md), do not just append a string. Note protobuf's
  `edition = "UNSTABLE"` marker is refused by this rule, since the in-development edition's defaults
  are by definition not settled.

- **The debug dumper (`--dump`) dropped an implicit-presence `float`/`double` holding `-0.0`.** A
  proto3 singular field equal to its zero default is omitted, matching protobuf's JSON — but the test
  for "is this the default" compared with `==`, and `-0.0 == 0.0` is true. So a negative zero, which
  protobuf treats as non-default (it serializes the field, and its JSON prints `-0.0`), vanished from
  the dump entirely and read back as absent. The comparison is now made on the bit pattern, so `-0.0`
  prints and positive zero is still omitted.

  The same `==` also meant such a header did not **compile** for a consumer building with
  `-Wfloat-equal -Werror` ("comparing floating-point with `==` or `!=` is unsafe"), which is the
  louder symptom of the two. Regenerate the `*.rp.dump.hpp` headers to pick this up; only schemas
  with a singular implicit-presence float or double are affected.

- **Two duplicate sub-message cases no longer decode to a tree protobuf would not produce.** A
  singular sub-message repeating on the wire has always been rejected, because protobuf *merges* the
  occurrences and a read-only tree cannot. Two paths slipped through that check and silently accepted
  the buffer instead:
  - a **oneof member**, because the oneof's last-wins rule ran first, so the later occurrence
    replaced the earlier one;
  - a **map entry repeating its `value`**, where protobuf merges within the entry. This one differed
    by how the value is stored: a pointer-stored value was replaced, while an inline-stored one was
    decoded *into the already-populated struct* — an in-place partial merge that accumulated presence
    bits but replaced any repeated field inside, matching neither protobuf nor a plain overwrite.

  Both now fail with `ArenaDecodeError::Code::RepeatedSingularMessage`, carrying the field number you
  wrote in the schema — for a map, the map's own number rather than the synthetic entry's `value`.
  The ordinary cases are untouched: a oneof whose members alternate still decodes (a different member
  clears the oneof, and protobuf also starts the later occurrence fresh), as do two map entries
  sharing a key and an entry repeating its scalar `key`. The check is unconditional, so a buffer
  whose occurrences would have merged to the same tree an overwrite produces is now rejected too —
  telling those apart needs the merge machinery this deliberately does not carry.

  **Regenerate** to pick this up — arena headers change for schemas with a sub-message oneof member
  or a message-valued map. The rules for every duplicate-field kind, in both decode models, are now
  written down in [docs/semantics.md](docs/semantics.md).

- **`option message_set_wire_format = true` is accepted instead of rejected.** A MessageSet is a
  proto1-era container holding only extensions, encoded as repeated groups in field 1. RapidProto
  does not materialize extensions, so its contents were never going to be readable — but rejecting
  the option failed the whole **file**, taking every unrelated message with it (protobuf's own
  proto2 conformance schema has ~200 that decode perfectly). Such a message now generates, emits a
  warning naming it, and decodes as unknown fields; a malformed group is still a wire error. Eight
  schemas in the real-world corpus, including `test_messages_proto2.proto`, now generate.

- **Every C++ keyword is escaped in generated headers, and they now compile as C++20/23 too.** The
  reserved-identifier set was missing thirteen keywords. Five (`static_cast`, `const_cast`,
  `dynamic_cast`, `reinterpret_cast`, `static_assert`) broke **C++17** as well: a field with one of
  those names produced a header that does not compile at all. The other eight (`char8_t`, `concept`,
  `consteval`, `constinit`, `co_await`, `co_return`, `co_yield`, `requires`) are fine at C++17 and
  hard errors at C++20 — which matters because a generated header is compiled in the *consumer's*
  translation unit, commonly at a newer standard than this library targets. The set is now checked
  against the full `[lex.key]` table through C++23, a keyword fixture is in the corpus, and
  `./check.sh` compiles the generated headers at `-std=c++20` and `-std=c++23`.

- **The debug dumper (`--dump`) printed the same name for two different enum values.** A value's
  rendered name is the C++ enumerator the generated `enum class` declares, but the dumper re-derived
  it without the dedup step that enumerator went through — so `enum E { decode = 0; decode_ = 1; }`
  (protoc-valid; `decode` is reserved, so both sanitize to `decode_`) declared `decode_` and
  `decode__` while dumping both as `"decode_"`, leaving the dump ambiguous about which value the
  wire carried. Both now come from one shared helper. Regenerate `*.rp.dump.hpp` to pick this up;
  only enums with two values that sanitize alike are affected.

- **The debug dumper (`--dump`) rendered bools, integers and floating-point values wrongly.** Four
  defects, all from letting the output stream's formatting state decide how a value prints:
  - A `bool` printed as `1`/`0` instead of `true`/`false` whenever its group fit on one line. The
    dumper set `boolalpha` on the caller's stream, but a group that fits is rendered into an
    internal scratch buffer first, and the flag never reached it — so the same value printed
    differently depending on the surrounding line width. This applied to a `map<bool, …>`'s **keys**
    as well as to bool fields and values.
  - A `float`/`double` printed at the stream default of 6 significant digits, silently truncating:
    `3.141592653589793` dumped as `3.14159`, a *different* value. Both now print with enough digits
    to read back exactly, without padding every value out to the type's maximum (`0.1` stays `0.1`).
  - `NaN` and the infinities printed as bare `nan` / `inf`, which no JSON parser accepts. They now
    use protobuf's JSON convention: the quoted strings `"NaN"` / `"Infinity"` / `"-Infinity"`.
    They are recognized from the exponent bits rather than via `std::isnan`/`std::isinf`, so the
    rendering survives a consumer building the generated header with `-ffast-math`, under which
    those two fold to a constant `false`.
  - An integer took its base, sign and digit grouping from the stream it was written to. Under a
    German locale a dump contained `1.234.567.890`; with `std::hex` set — sticky, and ordinary in a
    debugging session — `255` printed as `ff`, and an out-of-range enum's `UNKNOWN(99)` marker
    became `UNKNOWN(63)`, a wrong number that still looks like a plausible one.

  Every value is now formatted by the dumper itself and handed to the stream as characters, so the
  output is identical whatever locale and flags that stream carries. Dumping also leaves that stream
  exactly as it found it, where it previously set `boolalpha` permanently — and it deliberately does
  not go the other way and re-configure the stream, since re-imbuing one that another thread may be
  reading is a data race.

  Regenerate the `*.rp.dump.hpp` headers to pick this up; the arena and streaming headers are
  unaffected.


- **Generated headers compile warning-free on GCC 13 and on Clang versions that diagnose an
  untaken ternary branch.** Both warnings fired in consumer builds but not in this repo's own build,
  so neither was caught here:
  - GCC 13 (and 14 for `ArrayView`) `-Wdangling-reference` on `msg.field()[i]`, where the accessor
    returns the view by value. It is a false positive — the element lives in the arena the view
    borrows, which outlives the temporary — and is now suppressed at the accessor's declaration.
    Note the trade-off: this also silences the warning for a view over storage owned by a temporary,
    which would be a genuine use-after-free. `data()`, `begin()` and `end()` are unaffected.
  - `-Wshift-count-overflow` in the packed-varint kernel: the width-8 specialization's length mask
    contained a `uint64 << 64` in the never-taken branch of a ternary. Recomputed with `if
    constexpr`, so the out-of-range shift is no longer part of the program, and the mask is pinned
    by `static_assert` at every width — decoding is bit-for-bit identical. As a side effect
    `fixed_fill<9>` now fails to compile instead of silently evaluating an out-of-range shift.

  Header-only — no regeneration needed, but the installed headers must be updated.

- **Generated headers no longer fail to compile when a generated name matches its own message.**
  C++ forbids a member with the same name as its class, and the generator deduped names only against
  a message's *siblings*, never against the message itself — and never deduped private storage
  members at all. So `message Callback { oneof callback { … } }`, which protoc accepts and which is
  common in real schemas, emitted a `Callback` tag struct inside `class Callback`, and the header
  failed to compile at the consumer. **137 of the ~8000 schemas in the real-world corpus** emit a
  different arena header once this is fixed.

  Every generated name is now assigned in one dedup scope that includes the enclosing class's name:
  the oneof visit-tag struct and its reader method, a map's `<Map>Entry` type, nested messages and
  enums (shared with the streaming model), fields, and the private `m_…` storage members, union and
  presence mask. The message keeps its name; the colliding generated one gains the usual `_` suffix.

  A field named `m_bytes` also keeps its accessor now: the streaming decoder's private byte-span
  member moved into the `rp_` prefix that `sanitize()` already puts out of every proto name's reach,
  so `m_bytes` is no longer a reserved word and the *user's* field is no longer the one renamed.

  **Regenerate to pick this up.** An arena header changes where a name collided, and in a few more
  schemas where a oneof's private storage is now derived from its deduped id rather than its raw
  proto name. Streaming headers change only in that one private member, and only for schemas that
  declare a message at all.

### Added

- **Debug dumper: `DumpOptions` (start indent + skip fields).** `rp_dump_write` / `rp_dump_string` now
  take an optional `rapidproto::dump::DumpOptions { width, indent, skip }`. `indent` starts the dump at a
  nesting level (each level = 2 columns) so it drops cleanly under surrounding output, and `skip` omits
  fields by **qualified dotted path** (e.g. `{"people.email", "address"}` — a leaf name is hidden only at
  that path, and a message path drops its whole subtree; the field is still decoded, just not printed).
  Backward-compatible: the old width argument still works (an integer converts to a width-only
  `DumpOptions`).

### Changed

- **Arena decoders: `RP_FLATTEN` is bounded by sub-message closure size — large schemas compile
  dramatically faster.** Generated decoders force `RP_FLATTEN`, which inlines a message's entire
  transitive sub-message closure; on a large schema one decoder absorbed most of the rest, so build
  time exploded. A googleapis `compute.proto` `Instance` translation unit took 99 s and 646 KB of
  `.text`, and `container/v1beta1/cluster_service.proto` (288 messages) did not finish compiling at
  all in 280 s — it now takes 129 s. The layout planner accumulates each message's closure cost and
  additionally emits `RP_NOINLINE` past a threshold, stopping a parent's flatten there. No decode
  regression was found. The threshold is `LayoutOptions::flatten_budget`, a defaulted field on the
  public options struct, not yet exposed as a CLI flag. **Regenerate to pick this up** — generated
  headers change.

- **Debug dumper: wide arrays print as aligned columns.** An array too wide for one line used to print
  one value per line; it now fills as many aligned columns as fit, so a long array is far shorter and
  numeric values line up by place value. Objects are unaffected. Upgrading the runtime header is
  enough — no regeneration needed — but dumps of wide arrays now look different.
- **Debug dumper: generated internals moved out of the public namespaces.** A generated
  `.rp.dump.hpp` now puts only its two public entry points (`rp_dump_write(std::ostream&, ...)` and
  `rp_dump_string`) in the message's namespace; the `Writer`-threaded core they forward to moved to
  `pkg::rp_dump_detail`, and the generated enum value-name tables moved from the runtime's public
  `rapidproto::dump` to `rapidproto::dump::detail`. Recursive calls are now emitted fully qualified
  rather than resolved by ADL. **No call-site change** — `pkg::rp_dump_string(m)` is unchanged — but
  regenerate to pick it up, and update any code that reached for the internal
  `rp_dump_write(const T&, Writer&)` overload directly.

## 0.3.1 — 2026-07-18

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
