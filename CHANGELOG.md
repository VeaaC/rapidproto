# Changelog

Notable, user-visible changes per release. Pre-1.0, the MINOR version is the breaking axis (the
SemVer-0 convention): expect breaking changes between 0.x and 0.(x+1), never within a patch.

## Unreleased

### Changed

- **Breaking: generated types moved under per-model roots.** Every spelling changes, so **regenerate
  and update your call sites**:

  | | before | after |
  |---|---|---|
  | arena | `pkg::Msg` | `rp::arena::pkg::Msg` |
  | streaming | `pkg::stream::Msg` | `rp::stream::pkg::Msg` |
  | enums | `pkg::Enum`, `pkg::Msg::Kind` | `rp::common::pkg::Enum`, `rp::common::pkg::Msg::Kind`, aliased into both models |

  A namespace alias per file leaves most bodies unchanged — one shape per model you use:

  ```cpp
  namespace pkg { using namespace rp::arena::pkg; }           // arena only
  namespace pkg { using namespace rp::common::pkg;             // streaming only
                  namespace stream = rp::stream::pkg; }
  namespace pkg { using namespace rp::arena::pkg;             // both
                  namespace stream = rp::stream::pkg; }
  ```

  The streaming row needs the common root because a streaming codebase spelled top-level enums at
  package scope (`pkg::Status`), which only the arena model's alias brings back. A package-less
  schema takes the same shapes at global scope. Put the alias in a `.cpp`: at file scope in a header
  it leaks to every includer. Do not combine two rows — `using namespace rp::arena::pkg;` beside
  `using namespace rp::common::pkg;` makes `pkg::Msg` ambiguous between the arena class and the
  mirror namespace.

  Two things the alias does not carry over, both compile errors rather than silent:

  - **A helper you wrote in `namespace pkg` for ADL** — `operator<<`, `to_string` — is no longer
    found, because lookup now searches `rp::arena::pkg`. Move it there, or call it qualified.
  - **A `namespace pkg::stream` of your own** collides with the streaming row's alias. Rename one.
  - A leftover `namespace pkg { class Msg; }` forward declaration wins over the using-directive and
    rebinds `pkg::Msg` to an empty class. Delete it; generated types cannot be forward-declared.

  If you already passed `--namespace-prefix=rp` for protoc coexistence, your types move from
  `rp::pkg::Msg` to `rp::arena::pkg::Msg` — and the flag is no longer needed for that purpose. The root is named by
  `--namespace-prefix`, which now defaults to `rp` and **no longer accepts an empty value** — the
  three root segments would otherwise land at global scope. Pass `--namespace-prefix=myco` if your
  codebase already owns `rp`. A prefix component is refused rather than silently renamed when it
  would not compile as written (a keyword, `std`, a name that macro-expands), when it starts with
  `rp_`/`RP_`, or when it is `rapidproto` — the generator's and the runtime's own names.

  **Nested enums are now shared too.** A `Msg::Kind` used to be defined separately inside each
  model's class, so an enum read from the arena decoder was a different C++ type from the streaming
  one and could not be compared or passed across. Both models
  now alias one definition, mirrored under `rp::common`. The spelling you already write is unchanged.

  **The dumper's entry points moved with them.** `pkg::rp_dump_string(m, opts)` and
  `pkg::rp_dump_write(os, m, opts)` become `rapidproto::dump(m, opts)` and
  `rapidproto::dump(os, m, opts)` — one spelling for every schema, usable from generic code — and
  `rapidproto::dump::DumpOptions` becomes `rapidproto::DumpOptions`. Calling `dump` on a type with no
  generated dumper now names the fix in a `static_assert` instead of failing on an incomplete type.
  `Writer` and the `write_*` helpers move to `rapidproto::dump_detail`; they were never documented.

  What this buys: a schema can now declare a top-level type named `stream` (previously the two
  headers collided, and a top-level *enum* of that name broke the streaming header on its own); a
  package and a sibling `pkg.stream.*` package no longer collide; and generated headers coexist with
  protoc's `.pb.h` for the same schema **by default**, including the well-known types, where
  RapidProto previously redefined `google::protobuf::Timestamp` for any schema importing one.

- **Breaking: `MapView::find` returns an iterator, not a pointer.** It now compares against `end()`
  the way `std::map` does. Previously it returned `nullptr` on a miss while `end()` was
  one-past-the-end, so the habitual `find(k) != end()` compiled clean under `-Wall -Wextra`, was
  true for a **miss** on a non-empty map, and then dereferenced null — while being accidentally
  correct on a map with no entries, so it passed a unit test and crashed on the first message that
  had any.

  The iterator is deliberately not convertible to a pointer or to `bool`, so both old spellings —
  `if (auto* e = m.find(k))` and `m.find(k) != nullptr` — are now compile errors rather than
  silently inverted. Rewrite them as `if (auto it = m.find(k); it != m.end())`. Iteration,
  `size()`, `empty()` and `.key()`/`.value()` are unchanged, and the iterator is a forward iterator,
  so range-`for` and `std::` algorithms work as before.


- **A reserved-name escape no longer takes an identifier another type really uses.** `decode` is
  reserved (the decoders expose a static `decode()`), so `enum decode` is emitted as `decode_` — and
  a schema that also declares `message decode_` got two `p::decode_`, which does not compile. Names
  are now deduplicated across the whole PACKAGE rather than per file, so the two no longer land on
  each other even when they live in different files of that package.

  Two consequences for generated identifiers, so **regenerate**. At PACKAGE scope — top-level enums
  and messages — a literal name and an escape that contend now resolve the other way round: the
  literal keeps its spelling and the escape moves, so that schema emits `decode_` (the message) and
  `decode__` (the enum) where it previously emitted `decode_` (the enum) and `decode__` (the
  message). That applies to single-file schemas too, not only the cross-file case that failed to
  compile. Names INSIDE a message (nested types, fields, oneofs, map entries) are unaffected: there
  the escape still keeps `decode_` and the literal takes `decode__`. And because the dedup scope is
  now the whole resolved file set, adding a file to a package can shift an escaped id in a sibling
  file — including in a header a previous generator run already wrote, which is a compile error
  rather than a silent mismatch.

  Unchanged for every one of the 8018 schemas in the corpus sweep: this only moves names when a
  reserved-name escape and a real identifier actually contend.

- **`Callbacks` is a usable field name again**, and `RpFs` / `RpT` / `RpTag` never break a schema.
  The generated code's own template parameters and aliases are now spelled `rp_Callbacks`, `rp_Fs`,
  `rp_T` and `rp_Tag` — inside the `rp_` prefix the generator already reserves — so none of them
  needs a reserved word of its own. `Callbacks` came off the reserved list as a result; the other
  three were never on it, which is why `oneof RpFs` failed to compile rather than being escaped.

  The principle: reserve a name only when it is public API a user writes and so cannot take the
  prefix. That leaves `Value`, `Key`, `kNumber`, `kName`, `decode` and the namespace `std`
  (`rapidproto` is escaped only as a namespace component, where it actually collides).

  Only the streaming decoder's *declaration* changes shape (`template <class... rp_Callbacks>`);
  callbacks are deduced, so calling code is unaffected — unless your schema actually spells a name
  `Callbacks`, whose generated identifier goes back from `Callbacks_` to `Callbacks`.
  **Regenerate** to pick it up.

### Fixed

- **`rapidproto_generate(NAMESPACE_PREFIX N)` no longer silently ignores the prefix.** The helper
  tested the value for truth, and CMake reads `N`, `no`, `off`, `false` and `0` as false — so those
  prefixes were dropped and headers were generated under the default while the build file said
  otherwise. It now tests whether the keyword was given at all.

- **A deleted `<stem>.rp.common.hpp` is regenerated instead of breaking the build for good.** The
  CMake helper declared the decoder headers as outputs but not the shared common header, so removing
  it (or losing it to a partial clean) left `fatal error: <stem>.rp.common.hpp: No such file or
  directory` until something unrelated invalidated the batch.

- **A "maximum nesting depth exceeded" error now points at the token that exceeded it.** The
  parser reports positions as token indices, which the resolver maps back to `file:line:col`; this
  one error stored a byte offset instead, so the diagnostic landed in the wrong column and, on a
  file with more bytes than tokens, past the end of the file (`deep.proto:122:1` for a 121-line
  file). The rejection itself was always correct — only where it pointed was wrong.

- **`rapidprotoc` names generated headers by one rule, and refuses inputs it cannot name.** A
  header is placed at the schema's path relative to the first `-I` that contains it, else at its
  basename — the rule the CMake helper already documented and followed. The CLI deviated for
  relative arguments, naming the header after the path as given, so `rapidprotoc --out-dir gen
  ../schemas/x.proto` reported `wrote gen/../schemas/x.rp.hpp` and created it **outside** `gen`,
  exiting 0. It now writes `gen/x.rp.hpp`, as the same schema always did via CMake or an absolute
  path. A relative argument that resolves under no `-I` therefore loses its directory prefix:
  `rapidprotoc --out-dir gen sub/a.proto` writes `gen/a.rp.hpp`, not `gen/sub/a.rp.hpp`. Pass
  `-I .` to keep the subdirectory.

  Three inputs that silently lost work are now errors, each naming both files. Two schemas that
  generate the same header (`/a/x.proto` and `/b/x.proto` both wrote `x.rp.hpp`, the second over
  the first); two that share a canonical name via different `-I` directories (deduplicated as one
  file, so only one was generated at all); and a `..` in an `import`, which names a header outside
  the output directory and which protoc rejects too. Nothing is written when any of these fires.
  Diagnostics now print entry paths as absolute, since that is the form the CLI resolves.

- **A schema reached through a symlink no longer regenerates on every build.** `rapidproto_generate()`
  declares the generated headers as a build rule's outputs, and the CMake helper computed that path
  by resolving symlinks in a case where the generator does not — so for an entry whose link name
  differs from its target's, the declared header was never the one written and the target rebuilt
  forever. The same applied to a `.proto` produced by another rule under a symlinked path: CMake
  cannot resolve a path that does not exist yet, so the helper now resolves the longest part of it
  that does — matching what the generator computes at build time, when the file is there.

- **Ninja no longer regenerates the headers on every build.** The depfile listed its targets in
  sorted order, but Ninja accepts a depfile only when its *first* target is the build rule's first
  output — and a mismatch is not an error, it silently marks the outputs dirty forever. Any target
  whose first generated header was not the alphabetically smallest was affected: `DUMP` always
  (`<stem>.rp.dump.hpp` sorts ahead of the `<stem>.rp.hpp` anchor), and multi-schema targets whose
  first `PROTOS` entry does not sort first. Makefile generators were unaffected — they read the
  depfile as ordinary rules, in any order. ([#40](https://github.com/VeaaC/rapidproto/issues/40))

- **`group` inside a `oneof` now parses.** protoc accepts it; RapidProto answered `unexpected input`
  and refused the schema outright:

  ```proto
  message M { oneof pick { int32 i = 1; group G = 2 { optional int32 x = 3; } } }
  ```

  A oneof is not a scope, so the synthesized message lands in the enclosing message while the
  delimited field joins the oneof — the same shape editions already reached through
  `features.message_encoding = DELIMITED`, which is why this was a front-end gap rather than a
  decode one.

  A oneof member still may not carry a label, and that diagnostic got better along the way: it now
  says so (`a oneof member must not have a label`) and points at the label token, where before it
  said `unexpected input` and pointed past it. It also now rejects `oneof p { optional x = 1; }` in
  a file that happens to declare a message named `optional`, which used to parse as a field of that
  type. Both match protoc's position exactly.

  No schema in the real-world corpus hits it — groups are proto2-era and deprecated, and this
  combination rarer still — which is why nothing exercised the path.

  Matching the [language spec](https://protobuf.com/docs/language-spec) while in the area removed
  two stray acceptances, both of which protoc also rejects:

  - A bare `;` between **oneof** members. `MessageElement` lists an empty statement; `OneofElement`
    does not. An empty oneof *body* (`oneof p { }`) is still accepted: the grammar admits it, and
    the separate prose rule that a oneof must hold at least one member is a semantic check we do
    not make.
  - A bare `;` **or an `option`** inside an `extend` body. `ExtensionElement` is the only element
    list in the grammar carrying neither. That is deliberate on the spec's part, not an omission:
    `ExtensionFieldDeclIdentifier` subtracts only the cardinality keywords, *not* `option`, so
    `option` is a legal extension-field **type name** there. Accepting an option declaration did
    more than widen the grammar — it swallowed the grammar-legal `extend M { option x = 100; }` as
    an option instead of a field. `ExtendNode::options` is gone with it; nothing consumed it beyond
    a feature-resolution call that could never fire.

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
    `ByteView`, `WireType`, `dump`, `ArrayView`, … — not a closed list) clashed with it. Escaped as
    a *namespace component only* — a message or field of that name sits in the schema's own
    namespace and never collided, so it keeps its name.
  - **`RP_FLATTEN` and `RP_NOINLINE`**, the runtime's two object-like macros, in any role. The rule
    is the whole `RP_` prefix, matching the existing `rp_` one, so it holds for macros added later.
  - **A oneof named `EOF`** or another common macro. arenagen synthesizes its visit-tag struct from
    the raw proto name, so `sanitize()` never saw it and the preprocessor ate `struct EOF`. This one
    gets a deliberately narrow escape — only names that would macro-expand — because a tag struct
    may legitimately be called `Value`, `Key` or `decode`, and escaping the full reserved set there
    renames the public tag struct of 77 corpus schemas for no compile benefit.

  **Renames of API that already compiled.** One reserved set serves every role, and the `rp_`/`RP_`
  prefixes are reserved wholesale, so these change even though they never broke:

  - `std` as an arena or dump accessor (`msg.std()` → `msg.std_()`), an enum value, or a oneof.
  - Any `RP_…` name in any role, whether or not it is a macro — **including one the generator
    *capitalizes* into that prefix**, so a oneof or map field spelled `rP_…` renames its visit-tag
    or `…Entry` struct even though the proto name has no `RP_` in it.
  - An enum with *any* value whose prefix-stripped remainder starts with `RP_` loses prefix
    stripping **for the whole enum**, so every value in it keeps its full name
    (`KIND_RP_A`/`KIND_OK` rather than `RP_A`/`OK`).

  Wire names and proto names are untouched, but a renamed **enum value** does change the `--dump`
  text output, which prints the C++ identifier.

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
