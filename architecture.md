# RapidProto Architecture

RapidProto compiles a Protocol Buffers schema (`.proto`) into header-only C++ **decoders**: code that
reads protobuf wire bytes into your program. One CLI, `rapidprotoc`, runs a shared front-end and emits
two complementary decode **models**:

- **Streaming** (the `streamgen` emitter). A message's `decode()` walks the wire once and hands each
  field to a callback. Nothing is materialized, and it's allocation-free. Types live at `pkg::stream::Msg`.
- **Arena** (the `arenagen` emitter). `decode()` builds a fully-allocated, read-only object tree in a
  bump arena, navigated by accessor. It's built to beat `protoc` + `google::protobuf::Arena` on decode
  time and memory. Types live at `pkg::Msg`.

```
                 lex()          parse_file()       resolve()/analyze()
  .proto text  ────────► tokens ───────────► FileNode ──────────────► ResolvedFileSet + SymbolTable
  (front-end: parse)                                                          │
                                                          rapidprotoc emits ◄─┘
                          ┌─────────────────────────────┴──────────────────────────────┐
                          ▼ arenagen → <stem>.rp.hpp        streamgen → <stem>.rp.stream.hpp ▼
                          ▼            (+ shared <stem>.rp.common.hpp for the schema's enums) ▼
```

Both models **decode only** (no serialization, no JSON), **fully validate** untrusted wire input, and
**trust the schema** (assumed to have passed `protoc`; field *values* are not range-checked). The output
is header-only, so a consumer adds `-I<out-dir>` and links nothing.

> **How to read this doc.** **Part I — Overview** is the onboarding path: read it top to bottom (~10
> minutes) to get the mental model, find the code, build it, and learn the constraints. **Part II —
> Reference** is the deep, per-subsystem detail; jump to the section for whatever you're working on.
> A user-facing guide to *using* the generated decoders is in [`README.md`](README.md).

---
---

# Part I — Overview

## Terminology

Naming is kept consistent across the code and docs:

- **parse.** The `.proto` **front-end** (lex → parse → resolve → analyze a schema into the AST). The one
  place "parse" is the right verb.
- **decode.** Reading serialized message **bytes** through a generated decoder. The runtime entry point
  on every generated type is `decode(...)`, never "parse" or "read".
- **model.** One of the two decode strategies emitted from a schema: **arena** or **streaming**.
- **emitter.** The library that generates a model's code: `streamgen` (→ the streaming model) and
  `arenagen` (→ the arena model). Both are driven by the single CLI, **`rapidprotoc`**.
- **decoder.** The generated type a user calls (`pkg::Msg` arena, `pkg::stream::Msg` streaming).
  "Parser" is reserved for the schema front-end.
- **common header.** `<stem>.rp.common.hpp`: the schema's top-level enums as one shared C++ type both
  models include.
- file extensions: **`.rp.hpp`** (arena), **`.rp.stream.hpp`** (streaming), **`.rp.common.hpp`** (the
  shared enums).

## Orientation

```
include/rapidproto/   public headers: range, result, source_id, source, combinators, lexer, ast,
                      parser, resolver, resolve, features, interpret, wellknown, runtime (wire reader +
                      streaming dispatch), arena_runtime; codegen/ (printer, naming, wire, the shared
                      codegen layer, incl. emit.hpp); streamgen/ + arenagen/ (the two emitters; arenagen also has the
                      layout planner); cli/driver.hpp (shared CLI driver)
src/                  implementations + main.cpp (schema-inspection CLI) + wellknown_generated.cpp
                      (generated); codegen/; streamgen/; arenagen/; rapidprotoc/main.cpp (the CLI)
cmake/                embed_runtime.cmake (build-time: embeds each emitter's runtime header into the CLI)
wellknown/            vendored WKT .proto sources + embed_wellknown.py
tests/                Catch2 unit tests + the golden harnesses (AST, wire, streamgen, arenagen, arena
                      layout) + corpus/ + benchmarks; regen_goldens.sh; the compile-fail + stress harnesses
CMakeLists.txt · CMakePresets.json · check.sh
```

- **Build.** `cmake --preset gcc` (or `clang`) configures a dual-compiler build (gcc-13, clang-20) with
  `-Werror`. Targets: `rapidprotoc` (the CLI), `rapidproto_tests`.
- **The gate.** `./check.sh` is the one-stop quality bar: clang-format, build + test on both compilers,
  clang-tidy (strict on the library, relaxed on tests), the compile-fail harnesses (the generated API
  rejects misuse), and a dispatch-gate stress compile. It **must be green before any commit**.
  `./check.sh fix` formats first; `./check.sh quick` is gcc-only for the inner loop (not the commit bar).
- **Goldens.** Much of the suite is golden tests (the analyzed AST, the wire structure, each emitter's
  output, the arena layout plan, all dumped to text and compared byte-for-byte). After an *intentional*
  change to a generator / dumper, run **`tests/regen_goldens.sh`**, then `./check.sh`, and review the
  diff.
- **Comment wrapping.** `.clang-format` sets `ReflowComments: false`, so wrap long comment lines yourself.

The full test inventory and benchmark commands are in [Build and test](#build-and-test) (Part II).

## Core model & invariants

The single governing constraint is **decode-only**, and it has two faces that pull in opposite
directions:

- **Trust the schema (front-end).** A `.proto` is assumed to have already passed `protoc`. The front-end
  does **no** semantic validation (no duplicate-number, range, enum-zero, or FQN-uniqueness checks) and
  trusts numeric literals to be in range. That keeps a clean seam for a future `validate(FileSet)` pass
  and lets numeric interpretation be infallible. There is **no `descriptor.proto` dependency**.
- **Distrust the wire (decoders).** Serialized message bytes are **untrusted**, so the wire reader is
  **fully validating**: every varint overflow, truncation, length overrun, reserved wire type, and group
  mismatch is detected, and adversarial nesting is depth-capped. Malformed input fails cleanly and never
  causes UB. Field *values* are still not range-checked (that's the schema's job, which it trusts).

The full list of intentional non-goals and known limitations is in [Known limitations and
non-goals](#known-limitations-and-non-goals) (Part II). The load-bearing **invariants** a contributor
must not break:

- **SymbolTable pointer stability.** The table's `extensions` (`const FieldNode*`) and `messages`/`enums`
  node index point into the node vectors inside `file_set`. `resolve_types` builds them; the only later
  pass (`interpret_options`) mutates fields in place and never reallocates a node-holding vector, so the
  pointers stay valid for `file_set`'s lifetime (which must outlive the table). They also survive *moving*
  the `file_set` (a `std::vector` move transfers its buffer, preserving element addresses), which is what
  lets the CLI return the analyzed set and its table together by value.
- **Arena trivial-destructibility.** Every object an arena decoder places in the `Arena` is trivially
  destructible (`ArenaString`'s big buffer is itself arena-owned), so no destructor ever runs. That makes
  `reset()` a pointer rewind and dropping the arena a single bulk free. The generated headers
  `static_assert` it per message.
- **AST value semantics.** The recursive `OptionValue` is fully self-owning; copies are independent and
  deep, moves leave nothing dangling.
- **Offset duality.** The lexer works in byte offsets; the parser in token indices. The single
  translation point (token index → byte offset) is in the resolver's per-file parse step.
- **Coexistence output invariance.** Each model's generated output is byte-identical whether that model is
  generated alone or together with the other (see [Coexistence design](#coexistence-design)).

## Subsystem map

The codebase is a shared front-end + wire reader, two emitters built on a shared codegen layer, and the
coexistence glue. Each has a reference section in Part II:

- **Front-end** (`lexer` → `parser` → `resolver` → semantic passes → `SymbolTable`). Turns `.proto` text
  into a normalized, fully-resolved AST that captures everything decode-relevant and nothing else. It's
  built as **parser combinators** returning `Result<T>`; if that idiom is unfamiliar, skim the
  [Foundations](#foundations) primer before the front-end code. See [Front-end](#front-end).
- **Wire reader** (`runtime.hpp`). A type-agnostic, fully-validating pull reader of the binary wire
  format, with no AST dependency; the hot path both models build on. See [Wire reader](#wire-reader).
- **Streaming emitter** (`src/streamgen/`). Emits callback decoders (`.rp.stream.hpp`) with compile-time
  dispatch and zero runtime overhead. See [Streaming emitter](#streaming-emitter).
- **Arena emitter** (`src/arenagen/`). Emits materializing decoders (`.rp.hpp`); a layout planner packs
  each message into a read-only arena tree. See [Arena emitter](#arena-emitter).
- **Coexistence.** Model namespacing plus a shared common header let both decoders for one schema compile
  in one TU. See [Coexistence design](#coexistence-design).
- **Shared codegen layer** (`codegen/`). The `Printer`, the C++ naming table, and the scalar/wire facts
  both emitters reuse. See [Shared emitter infrastructure](#shared-emitter-infrastructure).

---
---

# Part II — Reference

## Front-end

`resolve(entry_file, config)` reads the entry `.proto` and all transitive imports from disk (lexing +
parsing each), returning a `ResolvedFileSet` (files in topological order — imports precede their
importer). `analyze(file_set)` then runs four in-place semantic passes and returns a `SymbolTable`. Both
return `Result<T>` and stop at the first error.

### Foundations

- **`range.hpp` (`Range<T>`).** A non-owning `(pointer, length)` view, used as parser input (`Range<char>`
  for the lexer, `Range<Token>` for the parser). Constructible from a `string_view` temporary but not from
  an owning rvalue (a deleted overload prevents dangling).
- **`result.hpp` (`Result<T>`, `Error`, `RP_TRY`).** The success-or-error return type of every front-end
  step. `Error` carries `{ SourceId source, byte_offset, message, fatal }`; `fatal` marks a committed
  error that backtracking combinators propagate instead of swallowing (set by `cut`). `RP_TRY(target,
  expr)` unwraps or returns the error.
- **`source_id.hpp` (`SourceId`).** A lightweight source-buffer handle; the registry that maps it to a
  filename/text lives in `source.hpp` (used for `file:line:col` rendering).

### Combinators (`combinators.hpp`, header-only)

A parser is any callable `Range<I> -> Result<Parsed<O, I>>` returning the produced value plus the
unconsumed remainder, or an `Error` whose offset is relative to its input. Sequential combinators lift
child offsets, so the outermost parser reports a whole-buffer offset. Toolkit: `one`, `tag`, `take_while`,
`take_while1`, `take_till`, `take_until`, `alt`, `seq`, `opt`, `many`, `many1`, `map`, `cut`, `recognize`,
`all_consuming`, `delimited`, `preceded`, `separated_list`.

- Combinators are generic lambdas templated on the input range, so the same toolkit serves the lexer
  (`Range<char>`) and the parser (`Range<Token>`).
- `alt` reports the **farthest** child error (best-match diagnostics) and stops early on a `fatal` error.
- `cut` marks the point a production is unambiguously entered, converting later failures into fatal errors
  so they propagate past `alt`/`opt`/`many` instead of backtracking.
- `many`/`many1`/`separated_list` have zero-width-progress guards (no infinite loops).
- **A hub's combinator type must not leak into enclosing instantiation names.** A grammar hub (a
  `many(alt(...))` over rich branches — `message_element`, `file_element`, the lexer's token
  alternates, …) and any deep-chain branch feeding one are plain functions with the signature
  `Range<I> -> Result<Parsed<O, I>>`, invoking their combinator expression internally (and
  parser-internal variants like `MessageElement` are wrapper structs, not `using` aliases, so they
  mangle by name). Passing a hub's combinator *type* upward re-spells the entire subtree into every
  enclosing instantiation name: measured on GCC 13, parser.cpp went from 65 s / 12.6 GB peak /
  422 MB of `.debug_str` (one instantiation name reached 34 MB of text) to 9 s / 0.9 GB / 17 MB
  purely by adding these boundaries, and lexer.cpp from 38 s / 10.7 GB to 3 s / 0.4 GB. The
  boundary is also why the dev presets can carry plain `-g`; they add `-gsplit-dwarf` besides
  (faster, lighter links; the `.dwo` files live beside their objects in the build tree).

### Lexer (`lexer.hpp`, `src/lexer.cpp`)

`lex(source)` turns text into a flat `vector<Token>`. Grammar-shaped scanning is combinator-based;
everything non-grammar — discarding whitespace/comments, keyword classification (43-keyword table),
int-vs-float literal classification, string-escape decoding, adjacent-string merging — is a plain
post-pass (`build_tokens`). Keyword classification is purely lexical: the parser still accepts a keyword
token where a name is expected (proto allows keywords as names). A `Token` is `{ kind, text (view into
source), str_value (decoded strings only), byte_offset }`; `LexResult` owns the source `std::string` so
the views stay valid across moves. `+` is lexed as a token (an extension over the 15 spec punctuation
tokens) so the option grammar can accept a leading `+`.

### AST (`ast.hpp`)

A single normalized model that abstracts away proto2/proto3/editions differences. Nodes are plain
copyable/movable structs; presence, enum openness, and repeated/message encoding are stored as resolved
semantic enums; everything decode-relevant is a typed field, everything else is retained raw under
`options`. Key nodes:

- **`FileNode`:** `syntax_level`, `edition`, `package`, `filename`, `imports`, `messages`, `enums`,
  file-level `extends`, file `options`. (No services field.)
- **`MessageNode`:** `fields`, `map_fields`, `oneofs`, `enums`, `nested_messages`, `reserved`,
  `extension_ranges`, nested `extends`, `options`, `fqn`.
- **`FieldNode`:** `name`, `type_name` (as written), `number`, and normalized attributes `presence`,
  `is_repeated`, `repeated_encoding`, `is_group`, `message_encoding`, `default_value` (proto2 `[default]`,
  stored raw), raw `options`, `fqn` (extension fields), and the type-resolution outputs
  `resolved_type_fqn` / `is_message_type` / `is_enum_type`.
- **`MapFieldNode`:** `key_type`, `value_type`, plus resolved `resolved_value_type_fqn` /
  `value_is_message` / `value_is_enum` (keys are always scalar).
- **`EnumNode` / `EnumValueNode`:** `openness`, `values`, `reserved`, `options`, `fqn`.
- **`ExtendNode`:** `extendee_type_name`, `fields`, `options`; valid at file and message scope.
- **`ExtensionRangeNode` / `ReservedNode`:** inclusive `NumberRange`s and reserved names. The `to max`
  sentinel is context-dependent: `kMaxMessageFieldNumber = 536870911` (2²⁹−1) for message/extension
  ranges, `kMaxEnumNumber = INT32_MAX` for enum ranges.

Option value tree: `OptionValue` is a `variant<bool, int64, uint64, double, Identifier, string,
MessageLiteral, ListLiteral>`. `MessageLiteral`/`ListLiteral` hold `vector<OptionValue>`, giving recursion
value semantics via `std::vector`'s incomplete-type support, with no heap indirection and fully
self-owning copies.

### Parser (`parser.hpp`, `src/parser.cpp`)

Recursive-descent over `Range<Token>`, combinator-centric: each production is a combinator expression with
its semantic action in `map()`; concrete functions appear only at recursion points (value ↔
message-literal/list ↔ field, nested message/group/extend bodies). `parse_file` is the entry; a
`ParseContext` threads the syntax level (defaulting to proto2 when no `syntax`/`edition` is declared).

- **Numeric interpretation is infallible** (trust-protoc). `make_int`/`make_float` never fail: a
  non-negative integer → `uint64`, a negative one fitting `int64` → `int64`, beyond 64-bit → `double`;
  float over/underflow saturates to ±inf or 0. `inf`/`nan` are stored as `double`.
- **Groups** synthesize a nested `MessageNode` (name as written) plus a `FieldNode` with `is_group =
  true`, a lowercased name, and `MessageEncoding::Delimited`. Groups in an `extend` hoist to the enclosing
  scope.
- **Maps** become first-class `MapFieldNode`s (codegen synthesizes the entry message later).
- **`service`/`rpc`** are consumed via balanced-brace skipping and dropped.
- Error offsets are token indices; the resolver maps them back to source byte offsets.

### Import resolver (`resolver.hpp`, `src/resolver.cpp`)

`resolve(entry_file, config)` does a DFS over the import graph, returning `ResolvedFileSet { files
(topological), file_index (canonical name → index) }`. The multi-entry overload unions several
entries into ONE set — a file reached twice (listed twice, or listed and also imported) resolves
once, keyed by its canonical name — which is what makes a `rapidprotoc` invocation a batch: one
parse per file, one topological order, and one field-modes resolution across everything generated
together.

- **Cycle detection** uses white/gray/black tri-coloring (gray = on the DFS stack); a back-edge to gray is
  reported with the full cycle path. Post-order collection gives topological order.
- **Path canonicalization** (`canonical_import_path`, via `lexically_normal`) folds different spellings of
  one file together; the entry is keyed by its include-relative name so a file importing it back resolves
  to the same node.
- **Import lookup** searches `config.include_paths` in order (first match wins), then falls back to the
  embedded well-known types unless `config.use_wellknown` is false. Lex/parse errors are tagged with the
  originating filename.

### Semantic passes (`resolve.cpp`, `features.cpp`, `interpret.cpp`)

`analyze(file_set)` composes four in-place passes, in order:

1. **Feature resolution** (`resolve_features`): a no-op for proto2/proto3. For editions it resolves the
   decode-relevant FeatureSet at each scope and writes results into `presence` / `openness` /
   `message_encoding` / `repeated_encoding`. The inheritance chain is edition-defaults → file → message →
   oneof → field/enum, accepting both dotted (`features.field_presence = X`) and aggregate forms.
   `utf8_validation` is read but not persisted; `json_format` is ignored.
2. **FQN computation** (`compute_fqns`): assigns the absolute FQN (leading `.`) of every message, enum,
   enum value, and extension field. Enum **values are sibling-scoped** (`.pkg.VALUE`, by the enum's
   enclosing scope, not the enum). Extension fields are qualified by their **declaration** scope, not the
   extendee.
3. **Type resolution** (`resolve_types`): resolves every type reference to its FQN and kind, writing
   `resolved_type_fqn` / `is_message_type` / `is_enum_type`, and builds the `SymbolTable`. It applies the
   post-resolution fixup the parser couldn't (a message-typed `Implicit` field → `Explicit`; a repeated
   message/enum field → `Expanded`). Lookup is progressive innermost-out scope-prefix search; a leading
   dot is absolute. Visibility is self + direct imports + the transitive **public**-import closure (weak
   visible; option imports excluded). Unresolved or not-visible → error.
4. **Option interpretation** (`interpret_options`): lifts the hardcoded decode-relevant options into
   typed fields: `[packed]` → `repeated_encoding` (gated on repeated non-message, non-enum so it never
   clobbers the type-resolution fixup), proto2 `[default]` → `default_value`. `option message_set_wire_
   format = true;` is rejected. All options remain in the raw `options` vectors.

The `SymbolTable` holds `symbols` (FQN → `Message`/`Enum`), `extensions` ((extendee FQN, number) → `const
FieldNode*`), and a **FQN → node index** (`messages`/`enums` → `const MessageNode*`/`const EnumNode*`),
populated during the same walk so an emitter that inspects a referenced type reuses it instead of
re-walking the AST (see [Shared emitter infrastructure](#shared-emitter-infrastructure)).

### Well-known types (`wellknown/`, `src/wellknown_generated.cpp`)

The 11 vendored Google WKT `.proto` sources (BSD-3-Clause, incl. `descriptor.proto`) are embedded into a
generated C++ TU by `wellknown/embed_wellknown.py`. `wellknown_source(path)` returns the embedded text;
the generated file is checked in (re-run the script when the sources change). A disk copy on an include
path shadows the embedded copy. WKTs are decoded as plain messages (no Timestamp/Any-specific semantics).

### Schema-inspection CLI (`src/main.cpp`)

A thin driver for *inspecting* a schema (distinct from `rapidprotoc`, which it shares `cli/driver.hpp`
with): parse `-I` / `--no-wellknown` / `<entry.proto>`, run `resolve()` then `analyze()`, and print a
summary (file/symbol/extension counts, per-file stats). Built strict, excluded from tidy/format.

---

## Wire reader

`runtime.hpp` reads the protobuf binary wire format directly from a byte buffer. It is deliberately
**type-agnostic**: it yields the ordered `(field_number, wire_type, raw_value)` sequence, leaving all
type-dependent interpretation (zigzag, float bit-cast, packed-array splitting, presence/merge) to the
caller, with **no AST dependency**. Its defining constraint is the inverse of the front-end's: **wire
input is untrusted**, so it is **fully validating** (every overflow, truncation, length overrun, reserved
wire type, and group mismatch → a `WireError`).

**Performance-shaped API.** Wire decoding is the hottest path in a decoder, so the primitives avoid the
heavyweight `Result`/`Error` and any stateful reader object:

- The wire primitives are **value-threaded free functions** in the `rapidproto::wire` namespace
  (`read_varint` with a 1-byte fast path, `read_tag`, `read_tag_or_end`, `read_fixed32/64`,
  `read_length_delimited`, `skip_value`, `scan_group_end`, `read_group`). Each takes the byte cursor as a
  `(cur, end, begin)` pointer triple **by value** and returns the advanced cursor (`nullptr` = failed),
  writing the decoded value and a payload-free `WireError` code + fail offset to caller-owned out-params.
  Because the cursor is passed and returned by value it stays in registers across the whole decode loop —
  no reader member whose address escapes to memory (measurably fewer retired instructions than a stateful
  cursor, most on GCC).
- The public view type is `std::string_view` (`ByteView`); the cursor is a raw `const uint8_t*` retyped
  from the view's `char` bytes, which is well-defined because `uint8_t` is `unsigned char` (a
  `static_assert` pins it).
- Header-only: the primitives (including the cold group walk) and the decode-dispatch machinery are all
  `inline`, so a generated decoder can vendor the whole runtime as that single file.

**API layers:** *wire primitives* (the `rapidproto::wire::read_*` / `skip_value` / `read_group` free
functions above); *interpretation helpers* (caller-applied, infallible: `zigzag_decode_32/64`,
`bit_cast_float/double` via `memcpy`, `varint_to_bool/int32/int64`).

**Single-level decode.** A LEN payload (string/bytes/sub-message/packed array — indistinguishable without
type info) and a group body (`read_group`) are returned as opaque `ByteView` spans the caller re-parses.
The only internal recursion is finding a group's matching `EGROUP`, bounded by `kMaxGroupDepth`.
Inline-hot-path throughput is ~1.7 GB/s (≈570 M fields/s, `-O3`).

---

## Streaming emitter

The `streamgen` emitter (`src/streamgen/`) turns the analyzed AST into C++ headers for **streaming,
callback-based decoders**, via inline emitters over a small `Printer` (`$placeholder$` substitution +
indentation, no template engine), one `<stem>.rp.stream.hpp` per `.proto`. (A user-facing guide to the
generated API is in [`README.md`](README.md).)

**Generated API.** For each message `Foo`, a `struct Foo` holding a borrowed `ByteView`, driven by
callbacks:

```cpp
Foo foo{bytes};                                   // non-owning view over the wire buffer
DecodeStatus s = foo.decode(                       // one pass over the wire, in wire order
    [&](Foo::id,   std::int32_t v)      { ... },
    [&](Foo::sub,  Bar b) -> DecodeStatus {        // sub-message: a nested sub-decoder to recurse
        return b.decode(...);                      // (return its status to propagate errors)
    });
```

Key properties:

- **Field-identity tags.** Each field gets a distinct empty tag type `Foo::fieldname` carrying `using
  Value` (+ `using Key` for maps), `kNumber`, and `kName` (the original proto name, even when the C++
  identifier was de-duplicated). Identity is tied to the field **name**, so referencing a removed/renamed
  field is a compile error.
- **One pass, wire order.** `decode()` reads each record once and dispatches to the matching callback;
  repeated/packed fire per element, maps per entry, unmatched/unknown are skipped. No aggregation,
  defaulting, or merging; wire data is forwarded 1:1.
- **Subset + catch-all.** Only fields you pass a callback for are delivered (rest skipped O(1)); a generic
  `[](auto, auto){}` catch-all matches every *known* field you didn't name. A one-arg
  `[](UnknownField){}` receives fields *not in the schema* (forward compat); the catch-all does not see
  those.
- **Exact-match, hard-to-misuse dispatch.** A callback claims a field only when its value type is *exactly*
  the field's `Value`; a wrong-but-convertible type or a duplicate is a **compile error**. The whole
  dispatch is compile-time: a `switch` (over the whole 1-byte tag for the common fields 1–15, else the
  field number), an `if constexpr` filter, and `static_assert`s that evaporate. There is no allocation, no
  `std::function`, and no virtual calls.
- **Open enums.** `enum class : std::int32_t` with `INT32_MIN`/`INT32_MAX` sentinels, forcing a `default:`
  in any consumer `switch`.
- **`decode()` is out-of-line.** The templated definition follows all struct shells, so a message-typed
  field (including forward and mutually-cyclic references) is a complete type where its decode compiles.

**Status.** `decode()` returns a lean `DecodeStatus` (a `WireError` + offset, or an `aborted` flag; no
allocation). A callback may return `void` (continue) or `DecodeStatus` (non-ok aborts the walk).

**Self-contained output.** The emitter depends only on libstdc++; `runtime.hpp` is embedded into the CLI
(a `runtime_embedded.cpp` generated at build from `runtime.hpp`, so it can never drift). One invocation
emits the whole resolved closure (entry + imports + WKTs) plus a copy of `runtime.hpp` under
`<out-dir>/rapidproto/`; every `#include` is include-root-relative, so the out-dir is vendorable.

---

## Arena emitter

The `arenagen` emitter (`src/arenagen/`) turns the AST into C++ headers (`<stem>.rp.hpp`) for
**materializing decoders**: a static `decode()` reads the whole message into a fully-allocated, **read-only
object tree inside a bump arena**, navigated by value/optional accessors. The goal is to beat `protoc`
+ `google::protobuf::Arena` on both decode time and peak memory. All variable-length data is **copied into
the arena**, so the input buffer is freeable after `decode()`; strings/bytes use SSO; repeated/maps are
arena arrays. The arena holds only **trivially-destructible** objects, so freeing or `reset()`-ing it is a
pointer rewind.

### The layout planner — the "brain" (`arenagen/layout.{hpp,cpp}`)

A pure analysis pass (no codegen), golden-tested on its own via a deterministic layout dump
(`tests/arena_layout_dump.hpp` → `tests/arena_layout_golden/*.txt`) so every decision is reviewable as text
before any C++ is emitted. Given a `MessageNode` + the FQN → node index, it computes a `MessageLayout`: a
**field kind** per field plus the byte layout. Field kinds:

- **InlineScalar / InlineEnum:** a fixed-width value inline; an optional one gets a presence bit.
- **SsoString:** a 16-byte `ArenaString` (≤ 15 bytes inline, else arena-copied).
- **InlineFixedSubMsg vs PointerSubMsg:** a *fixed-size* sub-message (recursively all-scalar; no
  strings/repeated/maps; not self-referential) is **inlined by value** when ≤ 16 bytes, else stored behind
  an arena pointer. The fixed-size analysis is recursive and cycle-aware (a self-reachable type is never
  fixed-size → pointer, which also terminates it).
- **BoolWrapperBits:** a single-`bool`-field message collapses to a presence bit + a value bit (the
  accessor still returns the wrapper type).
- **Repeated:** an `ArrayView<T>`. **Map:** a `MapView<Entry>` (insertion-order + last-wins linear
  `find`).

On top of the kinds it decides **member order** (alignment desc, size desc, field number asc, to minimize
padding), packs **presence and value bits** into the minimum mask words (`uint8/16/32/64`, or `uint64[]`
for > 64 bits), and places the mask to fill residual padding.

### Generated code shape

Per message: a `class` with the reordered storage + mask word(s) + inline storage / arena pointers /
views, the field accessors, nested enum/oneof/map types, and a static `decode(ByteView, Arena&,
ArenaDecodeError* = nullptr) → const Msg*` (plus an out-of-line `rp_decode_into` that fills an
already-allocated node, emitted after every class shell so forward/cyclic references are complete types).
Accessor conventions: scalars/enums by value (explicit-presence fields return `std::optional<T>`,
`std::nullopt` when absent); `std::string_view` for string/bytes (`std::optional<std::string_view>` if
explicit-presence); `const Sub*` for sub-messages (inline or pointer alike, `nullptr` when absent);
`ArrayView<T>` for repeated (a `StringArrayView` of `std::string_view` for repeated string/bytes); a
`MapView` with `find()` for maps; and a oneof *reader* `<oneof>(handlers…)` that dispatches the active
member to its typed handler (`std::monostate` handles unset).

**Presence and defaults.** An absent `Implicit` field reads back its zero default (0 / "" / first enum);
an absent `Explicit` field reads `std::nullopt` (the proto2 `[default = X]` is NOT materialized — a
consumer applies it via `value_or`). A missing proto2 `required` field makes `decode()` **fail** (`nullptr` +
`MissingRequired`), matching protoc — required-presence is tracked **transiently** during decode (a
stack-local bitmask validated at each message's end), so there is no resting presence bit for required
fields.

### Inside the emitter (`generator.cpp`)

The emitter (`generator.cpp`, the project's largest file) is a flat collection of small free `emit_*`
functions, each threaded with an `Emit` bundle — references to the `Printer`, the `CppNameTable`, the
`LayoutSet`, a per-message `SynthNames`, and the `SymbolTable`. Two facts orient a first read:

- **Two-layer naming.** The shared `CppNameTable` names and de-collides the schema's *own* identifiers;
  a per-message **`SynthNames`** pass names the identifiers arenagen *invents* (the `<Oneof>` visit-tag
  struct and the `<Map>Entry` type), which `CppNameTable` cannot dedup because they don't exist until the
  emitter conjures them (so a user nested type literally named `FooEntry` still can't collide).
- **Shells first, then decode bodies.** All struct shells are emitted before any out-of-line
  **`rp_decode_into`** body (for the complete-type reason given above). Each body is assembled from
  per-field *arms* (`emit_singular_arm` / `emit_repeated_arm` / `emit_map_arm` / `emit_oneof_arm`),
  wrapped by growable-array setup/finalize and the transient required-field bitmask.

### The arena runtime (`arena_runtime.hpp`)

A header-only, std-only support library the generated decoders depend on (vendored into the out-dir beside
`runtime.hpp`, which it builds on):

- **`Arena`:** a growable, single-threaded **bump allocator**. RAII-owned chunks (an inline head chunk +
  a vector of heap chunks), so a small message that fits the head, or a caller-seeded buffer, needs zero
  heap allocation. `reset()` rewinds for reuse without freeing. Chunk growth is geometric but **capped at
  96 KiB** (`kMaxChunk`): only the last chunk carries unfilled-tail waste, so capping its size bounds that
  waste at a constant (and 96 K stays under glibc's 128 K mmap threshold, keeping cold-arena chunks on the
  heap). `bytes_used()` / `bytes_reserved()` expose the footprint.
- **`ArenaString`:** a 16-byte SSO string (≤ 15 bytes inline; longer arena-copied as `{ptr, len}`, 32-bit
  `len`). Trivially copyable/destructible. A value ≥ 4 GiB can't be represented, so the decode fails with
  `StringTooLong` rather than truncating (a 64-bit-only concern).
- **`ArrayView<T>` / `StringArrayView` / `MapView<Entry>`:** the read-only views the accessors return.
  `StringArrayView` adapts a repeated `ArenaString` array to `std::string_view` elements; the map view
  does a last-wins linear `find` (protobuf map semantics).
- **`ArenaDecodeError`:** the failure detail (`Wire` / `OutOfMemory` / `RecursionTooDeep` /
  `MissingRequired` / `RepeatedSingularMessage` / `StringTooLong`), plus the depth guard
  (`kMaxDecodeDepth`, 100) decoders honor on untrusted nesting.

### Decode emission

The emitted `rp_decode_into` runs the value-threaded decode loop once: dispatch the field (a raw-byte peek switch
for single-byte tags, else the validating tag read + field-number switch) → decode the value into the
node, set presence/value bits, recurse into sub-messages. Strings are SSO'd /
arena-copied; **repeated fields accumulate single-pass into a growable arena array** (the benchmark-chosen
strategy, below); maps append (last-wins on read); groups use `read_group`; unknown fields are skipped
(with an opt-in "unknown present" bit under `--unknown-present`, or per message via
`--unknown=`/`unknown-fields`). Malformed input → `nullptr`
+ the error.

### Tuning (benchmark-driven knobs)

Three knobs were tuned against `tests/bench_arena.cpp` (see [Decoder performance](#decoder-performance))
and locked at their chosen values; each is a single constant with a rationale comment:

- **Repeated strategy: single-pass growable.** A two-pass *exact-size* variant (pre-count, then fill) cut
  memory hard (≈ 0.35× protoc) but cost ≈ +65 % time, because the pre-count is a second wire traversal
  (memory-bandwidth-bound, not removable). Speed was prioritized; the single-pass growable array
  (geometric realloc-and-copy) is kept. Its realloc waste is what the chunk cap and layout tuning bound.
- **Arena chunk cap = 96 KiB.** Validated across three payload shapes (mixed records, many small arrays, a
  few big arrays) up to 32 MB: held/used stays ≈ 1.0–1.2× vs ≈ 1.4–1.9× uncapped (the dominant held-memory
  lever), while warm decode time is cap-independent.
- **SSO inline = 15 chars** and **inline-sub-message cutoff = 16 bytes.** Both measured optimal: a wider
  SSO widens *every* string field (a net loss on mostly-short payloads), and with single-pass-growable
  arrays, inlining a sub-message of size S costs ≈ 2S of array memory vs ≈ 16 + S for a pointer, so
  inlining pays out exactly up to S = 16.

### Decode profiles (`arenagen/modes.{hpp,cpp}`)

Field modes are a **consumer property, not a schema property** — the same schema decodes differently
per consumer, so selection lives in generation flags/profile files (`--field-modes`, `--drop`, `--raw`),
never in the `.proto`. Resolution happens once against the resolved set (names → `FieldNode*`/
`MapFieldNode*` maps, field entry beats type entry; unknown names, same-level conflicts, drop-required,
oneof-member entries, and raw on anything but a message-typed field are hard errors; type-level entries
silently leave oneof members, drop+required, and raw-on-maps materialized). The planner consumes the
maps: `drop` removes the member (and its presence bit) from the layout entirely, recorded in
`MessageLayout::dropped` so the layout dump still shows the omission; `raw` plans a 16-byte payload
member — `ByteView` singular, `ArrayView<ByteView>` repeated — never fixed-size and with **no mask
bit**: a singular payload encodes absence as null *data* (the pointer-sub-message convention), so a
present-but-empty payload is a non-null empty view (`arena_detail::copy_payload`'s `kEmptyPayload`
sentinel). The emitter routes per plan — dropped fields get an explicit no-op `case` arm (validated
skip, without tripping `--unknown-present`); a raw arm is its materialized twin with the recursive
decode swapped for an arena copy of the payload (`wire::read_length_delimited`/`wire::read_group` both yield
exactly that), preserving stored-field semantics: wire-type-mismatch falls to the shared skip,
`RepeatedSingularMessage`, `required`'s transient bit. The stored view is exactly what the field
type's own `decode()` accepts — deferred decoding needs no new API and no streaming decoder.

The ODR story: a profile changes the generated types, so profiled headers wrap everything in
`inline namespace rp_modes_<id>` and stamp the profile into the banner. `<id>` is always
content-derived — an FNV-1a hash of the normalized entries, prefixed by the profile's `name` when one
is given (`rp_modes_lean_4ba94f51`) — so even two selections sharing a name hold distinct identities;
a name is readability, never trust. The unknown-fields selection folds into the SAME identity: each
`unknown-fields <msg>` / `--unknown=<msg>` contributes an `unknown .pkg.M` normalized line, and
`--unknown-present` contributes one stable `unknown *` line (so its id doesn't shift as the schema
gains messages, and it subsumes any redundant per-message entries) — closing the ODR gap for a flag
that changes a message's struct but used to leave its type name untouched. Qualified use stays
`pkg::Msg`; mixed-profile TUs hold distinct types and fail to **link** at any exchange point instead of
silently violating the ODR (`tests/arena_modes_link.sh` pins every direction in the default gate,
including `--unknown-present` with-vs-without). The common header (shared
enums) stays outside the namespace, so a profiled arena header still coexists with the streaming
header. A no-profile run is byte-identical to unprofiled output, and an all-excluded profile degrades
to exactly that. Known cut, deliberately deferred: no `materialize` directive to narrow a type-level
entry (additive when needed).

## Shared emitter infrastructure

Both emitters work over the same analyzed AST, so they share (rather than duplicate) the
generator-agnostic pieces (the `codegen/` module; neither emitter depends on the other):

- **`Printer` + the C++ naming layer** (`codegen/{printer,naming}.hpp`): the `$placeholder$` text emitter
  and `CppNameTable` (proto FQN → C++ name, identifier sanitization, `package a.b → namespace a::b`, the
  `--namespace-prefix` and `model_namespace` nesting; see [Coexistence design](#coexistence-design)).
- **The scalar/wire codegen table** (`codegen/wire.hpp`): the `scalar → {wire type, read call, value
  conversion}` table + packability predicate, which is literal wire-format knowledge, identical for both
  emitters, naming `::rapidproto::` runtime helpers. (Value-typed concerns, like streamgen's callback type
  and arena's storage type, stay each emitter's own.)
- **The FQN → node type index** in the front-end `SymbolTable`, built during the resolver's type walk, so
  neither emitter re-walks the AST to resolve a referenced type.
- **The CLI driver** (`cli/driver.hpp`): flag parsing (with a hook for model-specific flags like the arena
  model's `--unknown-present`), the resolve → analyze step, and header writing, shared by the CLI main.

**Targets.** `rapidproto_lib` (front-end + wire reader) underlies everything; `rapidproto_codegen_lib`
(the shared layer + the embedded `runtime.hpp` text) builds on it; `rapidproto_streamgen_lib` and
`rapidproto_arenagen_lib` are the two emitter libraries; the one `rapidprotoc` CLI links both, each
linking `rapidproto_codegen_lib`, neither depending on the other. Each emitter's runtime header is embedded
into the CLI at build time (`cmake/embed_runtime.cmake`) so it can never drift, and dropped into the
out-dir on each invocation.

---

## Coexistence design

`rapidprotoc` emits both decode models for one schema so they compile together in a single translation
unit. Three pieces make that work:

- **Model namespacing.** The arena model is the default and sits where a user expects, `pkg::Msg`; the
  streaming model nests one level deeper, at `pkg::stream::Msg`. The two `Msg` types are therefore distinct
  and never clash. This is driven by a `model_namespace` field on the C++ name table (`CppNameTable`) —
  empty for arena and `"stream"` for streaming, threaded through `build_cpp_names`. Only *messages* nest
  under it; enums do not (below). The nesting is each model's permanent shape, applied whether one model is
  generated or both, so adding the second never renames the first.
- **Shared enums in a common header.** A schema's top-level enums are emitted ONCE into
  `<stem>.rp.common.hpp` at package scope (`pkg::State`), by `codegen::emit_common_header`. Both decoders
  `#include` it (re-exported via an IWYU `export` pragma, so a TU that includes only the decoder still
  "directly provides" the enums). The streaming decoder additionally aliases each enum into its model
  namespace (`using ::pkg::State;` inside `namespace pkg::stream`), so `pkg::stream::State` resolves too.
  One proto enum is thus ONE C++ type shared across both models — no duplicate definition to collide.
  NESTED enums are not shared; they ride with their message inside each model's decoder (distinct
  fully-qualified names, no clash).
- **Parse once, emit per model.** `rapidprotoc` runs the front-end once, builds a name table per selected
  model, and emits the common header plus the selected decoders into one out-dir. The common header is
  model-agnostic, so it is byte-identical whichever model(s) are requested.

**Invariant.** Each model's generated output is byte-identical whether that model is generated alone or
together with the other — so a consumer adds the second model without touching the first. Both golden
suites assert this (regenerating with `rapidprotoc --arena` or `--stream` produces no change), and
`examples/consumer` decodes the same bytes with both models in one TU as an end-to-end check.

To coexist with `protoc`-generated headers instead, `--namespace-prefix=<ns>` nests everything under an
extra prefix (`ns::pkg::Msg`), keeping RapidProto's types clear of protoc's `pkg::Msg`.

---

## Decoder performance

Both emitters are measured with a **placement-controlled** discipline: standalone `-O3 -DNDEBUG`
benchmarks, pinned to one performance core, with a checksum cross-check so the work can't be optimized
away. Candidates run in **one binary** and are compared as cycles-per-byte ratios taken at one
instantaneous frequency, sampled adaptively until each ratio's significance is settled
(`tests/bench_harness.hpp`) — so a real few-percent win is separable from placement noise. Streaming is
compared against a hand-written value-threaded loop and mapbox/protozero (`tests/bench_streamgen.cpp` →
`rapidproto_bench`); arena against `protoc` + `google::protobuf::Arena` (`tests/bench_arena.cpp` →
`rapidproto_arena_bench`). Headline results — measured against libprotobuf 3.21 with gcc-13/clang-20,
pinned to one performance core; the bench prints its libprotobuf baseline version at startup, since the
baseline's version is half a ratio's meaning. One realistic payload; treat each number as a point, not a
constant, and reproduce with the benches:

- **Streaming is at or below a hand-written value-threaded loop, and near protozero.** The callback/dispatch
  abstraction is free, and on nested/message-heavy payloads the generated decoder actually *beats* a naive
  hand loop, because its loop is driven by a fused end-or-tag read (one bounds check per field, tag kept as
  a value) that a straightforward `while (!at_end()) { read_tag(); … }` does not use. It also validates
  *more* than protozero (whose wire-type checks are `assert`s that compile out under `NDEBUG`; ours never
  do). After the fused-loop work the generated streaming decoder is at or above protozero on nested-message
  decode on clang, and within ~15% on gcc.
- **Arena beats `protoc` on both axes:** decode time ≈ 0.4× protoc (≈ 2× like-for-like after accounting
  for protoc's per-`string` UTF-8 validation, which the arena skips), and peak memory ≈ ⅔ of protoc's —
  the same tree in roughly two-thirds the memory. "Memory" is allocator-reported arena accounting
  (`bytes_used`/`bytes_reserved` vs protobuf's `SpaceUsed`/`SpaceAllocated`), not process RSS. The
  memory ratio is deterministic (exact byte counts); the time multiple varies with payload shape and
  machine thermal state.
- **Real codegen wins, surfaced by same-binary A/B.** The headline metric is **GB/s** — measured decode
  throughput, which unlike retired instructions reflects everything the CPU pays for (branch
  mispredictions, cache/memory stalls; e.g. random-width packed varints run ~4× slower than fixed-width
  at *identical* ins/B, pure branch-mispredict cost). Cross-binary comparison buries genuine opportunities
  in placement noise, so the harness measures every arm back-to-back in one binary: its GB/s and
  cycle-ratio verdict are a real-time comparison at one placement. Across two separate *builds*, retired
  **instructions/byte** — deterministic and machine-independent — stays the cross-check that a change is
  genuine rather than a layout artifact (cycles/byte and GB/s are same-binary measures). Shipped so far: a fused 1-byte-tag fast path in `read_tag` (~10% on both compilers);
  driving both decode loops with a fused `read_tag_or_end` (one bounds check instead of `at_end()` +
  `read_tag()`, tag held as a value not `std::optional`), which closed most of the protozero gap on
  nested/message-heavy decode (≈2× nested-message throughput on gcc, at or above protozero on clang); a
  raw-byte peek switch that dispatches single-byte tags (fields 1–15) without splitting field from wire;
  `RP_FLATTEN` on generated `decode()`, which recovers the inlining GCC leaves on the table in a large
  translation unit (~30% more retired instructions on message/skip-heavy shapes, where Clang was already
  inlining); and, for the arena's packed scalars, pre-sizing the array from the wire length plus a single
  bulk copy of a packed **fixed-width** array on little-endian (≈2–2.5× packed varint, ≈5× packed fixed,
  ahead of protoc). That bulk copy moves a whole packed *array* in one `memcpy`; an earlier attempt to
  `memcpy` a *single* fixed-width field, by contrast, showed no effect under the same control — the
  discipline is what tells a real win from a placement artifact.

**The `protoc` baseline version matters — and is selectable without vendoring protobuf.** The arena
bench's `protoc` arm is whatever `find_package(Protobuf)` resolves; libprotobuf's own decoder has sped
up markedly across releases (measured here, **3.21 → 25.3 is ~10–40% fewer cycles/byte** on these
shapes, most on many-small-messages), so an old baseline flatters the arena. To benchmark against a
specific version, build it (plus **Abseil**, required by protobuf 22+) into a local prefix — nothing is
committed to the tree — and point CMake at it:

```sh
git clone --depth 1 --recurse-submodules -b v25.3 https://github.com/protocolbuffers/protobuf
cmake -S protobuf -B protobuf/_b -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF \
      -Dprotobuf_ABSL_PROVIDER=module -DCMAKE_INSTALL_PREFIX="$PWD/pb-25"
cmake --build protobuf/_b -j && cmake --install protobuf/_b
cmake --preset gcc -DCMAKE_PREFIX_PATH="$PWD/pb-25"    # find_package(Protobuf CONFIG) picks it up
```

The bench CMake prefers the protobuf **CONFIG** package (whose `protobuf::libprotobuf` target carries
the Abseil link deps 22+ needs) and falls back to the **FindProtobuf module** for a system 3.x install;
`protoc` and `libprotobuf` come matched from the same prefix. Against a current `protoc` (25.3) the
arena still wins on every shape and both compilers, by a smaller margin than against 3.21.

**The benchmarking caveat that matters most.** Decode hot loops run at ~1–8 GB/s
(1–2 ns/field), so throughput is dominated by **code placement**: which address a function lands at and
the resulting alignment / branch-predictor behavior. Two **byte-for-byte identical** decode functions in
one binary measure ~10% apart, purely from placement. Consequences for anyone profiling this code:

- **Compare structures at controlled placement.** A reliable A/B puts both variants in *one* binary,
  measured in both orders. Comparing across binaries, or a generated function against a hand-written one,
  measures placement, not the code. Under this control some plausible wins proved to be placement artifacts
  and were *not* adopted (an if-chain dispatch vs the `switch`, a `memcpy` of a *single* fixed-width field);
  others reproduced on both compilers and *were* shipped (the fused `read_tag` / `read_tag_or_end`, the
  peek-switch dispatch, and the packed-array bulk copy above).
- **Identical-function variance (~10%) is your noise floor.** A change whose effect falls within it is not
  a reliable win, however stable it looks in one binary.
- **Pin to one performance core** (`taskset -c <core> …`); unpinned hybrid-core runs swing 30%+, and even
  pinned, trust only same-binary ratios.

---

## Normalization truth tables

The rules the parser + feature pass + type-resolution fixup implement.

**Field presence**

| Syntax | singular scalar/enum | `optional` | message-typed singular | `required` | repeated |
|---|---|---|---|---|---|
| proto2 | Explicit | Explicit | Explicit | Required | (n/a) |
| proto3 | Implicit | Explicit | Explicit¹ | — | (n/a) |
| editions | from `field_presence` (default Explicit) | — | Explicit¹ | `LEGACY_REQUIRED` → Required | (n/a) |

¹ message-typed fields are forced to Explicit by the type-resolution fixup (the parser sets Implicit
before the type kind is known).

**Repeated encoding** (only for `is_repeated`)

| Type | proto2 | proto3 | editions |
|---|---|---|---|
| packable scalar | Expanded | Packed | from `repeated_field_encoding` (default Packed) |
| string/bytes/message/enum | Expanded | Expanded | Expanded² |

An explicit `[packed = ...]` overrides. ² A repeated message/enum is forced Expanded by the
type-resolution fixup; an editions `repeated_field_encoding` on a repeated *enum* is therefore not
reflected (a documented simplification; decoders accept both wire forms).

**Enum openness**

| proto2 | proto3 | editions |
|---|---|---|
| Closed | Open | from `enum_type` (default Open) |

---

## Build and test

- **Presets.** `cmake --preset gcc` / `--preset clang` configure dual-compiler builds (gcc-13, clang-20)
  with `-Werror`. Library sources compile strict + clang-tidy; the CLI is built strict but excluded from
  tidy/format; vendored Catch2 and generated sources are excluded.
- **`./check.sh`** is the single quality gate (see [Orientation](#orientation)):
  clang-format, build + test on both compilers, clang-tidy, the per-emitter compile-fail harnesses (each
  proves the generated API rejects misuse, e.g. assigning to a read-only arena accessor), and a
  dispatch-gate worst-case compile. `./check.sh fix` formats first; `./check.sh quick` is gcc-only.
- **Regenerating goldens.** After an intentional change to an emitter / AST dumper / wire dumper / layout
  dumper, run **`tests/regen_goldens.sh`** (then `./check.sh`, review the diff). It drives `rapidprotoc`
  directly so it works even when a change breaks the checked-in headers the tests `#include`.
- **Compile-time stress** of the dispatch gate (work is ~O(fields × callbacks)): run
  `tests/streamgen_compile_bench.sh` (optionally `N=128 …`) to time a large decoder's compile; do this
  when touching the gate metaprogramming. The gate runs it in `--check` (compile-only) mode.
- **Tests** are Catch2 unit tests per module plus integration over a 105-file corpus (gitignored under
  `build/schema`) and the embedded WKTs. The golden suites:
  - **AST golden** (`test_golden.cpp`): resolve + analyze the feature-complete `tests/corpus/` (proto2,
    proto3, editions 2023/2024, full-fidelity options, a multi-file import set) and assert the serialized
    syntax tree matches `tests/golden/*.txt` byte-for-byte. The dumper (`tests/ast_dump.hpp`) is a
    test-only debug serializer, not protobuf serialization.
  - **Wire** (`test_wire.cpp`, `[wire]`): every primitive and `WireError` with hand-authored buffers, plus
    structural decode of protoc-encoded fixtures. **Wire golden** (`[wire-golden]`) dumps each fixture's
    structure against `tests/wire_golden/*.txt`. Fixtures (`tests/wire_fixtures/*.bin`) are produced from
    textproto by `generate.py` via `protoc` and **checked in** (protoc is a dev/CI dependency only).
  - **Streamgen / arenagen goldens:** each generated `.rp.stream.hpp` / `.rp.hpp` compared byte-for-byte
    to `tests/streamgen_golden/` / `tests/arenagen_golden/` and compile-smoked; the **arena layout** dump
    golden-checked against `tests/arena_layout_golden/`; plus arena **runtime** unit tests
    (`test_arena_runtime.cpp`) and **decode** tests (`test_arena_decode.cpp`: real buffers incl. the
    protoc fixtures, asserting accessor values and the required/depth/malformed failure modes).
- **Decode benchmarks:** two standalone micro-benchmarks, built at `-O3 -DNDEBUG` and kept **out** of the
  test binary on purpose (measuring decoders inside the large binary is placement-sensitive). Run pinned:
  - `rapidproto_bench` (`bench_streamgen.cpp`): streaming decoder vs a hand-written value-threaded loop vs
    mapbox protozero across ~13 wire-path scenarios.
  - `rapidproto_arena_bench` (`bench_arena.cpp`, built only when `protobuf` is found): arena vs `protoc` +
    `Arena` vs streaming on a realistic payload, plus the chunk-cap shape/size sweep.

  See [Decoder performance](#decoder-performance) for how to read the numbers (and the placement noise floor).

---

## Known limitations and non-goals

Most of these follow from the trust-protoc / decode-only model: inputs are assumed valid, and anything not
decode-relevant may be approximated or rejected.

**Intentional non-goals:**

- No semantic validation, no serialization, no JSON (`json_format`/`json_name` are never interpreted).
- **gRPC is out of scope.** `service`/`rpc` are parsed past and dropped (no `ServiceNode`; a file's
  messages still decode).
- **`extend` / extension fields are retained but not emitted.** The AST keeps the extension registry (for a
  future typed-extension consumer), but neither emitter emits typed extension accessors today: an extension
  is "a field not in your schema", so streamgen surfaces it via the untyped unknown-field handler and
  arenagen drops it.
- **Options: raw except the decode-relevant set.** Option values are kept verbatim as a text-format value
  tree; only `packed`, the `features.*` set, and proto2 `default` are lifted into typed fields. No
  `descriptor.proto` dependency.
- The **schema** parser's recursion (over trusted nested `.proto` declarations) is unbounded, a deliberate
  non-goal. The **decoders** over untrusted *wire* input are bounded: the wire reader caps group nesting
  (`kMaxGroupDepth`) and the arena decoder caps sub-message nesting (`kMaxDecodeDepth`).
- The arena decoder **drops unknown fields** (no per-field channel like streamgen's); `--unknown-present`
  (every message) or `--unknown=<msg>` / `unknown-fields <msg>` (per message) reserves only a single
  "saw an unknown" flag, and the selection folds into the decode-profile identity.
- The arena decoder **rejects a singular sub-message that occurs more than once** (`RepeatedSingularMessage`)
  instead of protobuf's last-one-wins merge, an exotic case excluded for now (a clear error, not a silent
  mis-merge).
- Type resolution uses progressive scope search rather than protobuf's first-component-binds rule; for
  valid input the result is identical (shadowing edge cases `protoc` rejects are not diagnosed).
- The parser is over-permissive where `protoc` would reject (`optional`/`required` in editions, `extend` in
  proto3, `import option` in any syntax), which is harmless under trust-protoc.
- An editions `repeated_field_encoding` on a repeated *enum* is not reflected in `repeated_encoding` (forced
  Expanded); decoders accept both wire forms regardless.
- **Closed enums decode as open — intentionally.** The front-end resolves `EnumOpenness` (proto2 →
  Closed, editions `enum_type = CLOSED`), but neither emitter consumes it: an unrecognized value of a
  *closed* enum is delivered as its raw integer cast into the enum, where `protoc` would route it to
  unknown fields. Uniform open decoding keeps both models simple; consumers must not rely on
  closed-enum semantics (documented in README's enum bullet).

**Deferred** (worth fixing if the trust-protoc assumption is relaxed):

- A `group` inside a `oneof` is rejected (valid proto2; needs a group-hoisting channel on `OneofNode`).
- A missing `weak` import is a hard error (`protoc` treats weak as optional).
- A hex/octal integer literal overflowing `uint64` collapses to a wrong float value (decimal overflow is
  handled correctly); an octal string escape `> \377` truncates its high bit; text-format `-someident` is
  rejected.

A future `validate(FileSet)` pass is the natural home for the checks RapidProto intentionally skips.
