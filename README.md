# RapidProto — fast, header-only Protobuf decoders for C++

RapidProto compiles a `.proto` schema into **header-only C++ decoders**. One CLI, `rapidprotoc`, turns
your schema into headers you `#include`. Nothing to link. A single schema gives you two
decode models, and you pick whichever fits the job:

- **Arena.** `decode()` materializes the whole message into a read-only object tree in a bump
  arena, which you navigate with accessors (`person->name()`) in any order, as many times as you like.
- **Streaming.** `decode()` walks the wire once and hands each field's typed value to a
  callback you supply. Nothing is materialized, and there's zero allocation.

A `--dump` flag adds a third, optional emitter: a **debug dumper** that prints a decoded arena tree
as human-readable, JSON-*like* text — an inspection aid for logging and debugging, not a spec-compliant
JSON codec (see [Debug dumper](#debug-dumper)).

Both decode models are **decode-only**: no serialization, no JSON codec. Both fully validate untrusted wire input
(truncation, length overruns, group nesting), and both trust the schema — they assume `protoc` already
accepted it, so field *values* aren't range-checked. They cover **proto2, proto3, and the newer
editions schema format (2023/2024)**, including groups, maps, and oneofs. The arena model beats `protoc` +
`google::protobuf::Arena` on decode time and peak memory (see [Benchmarks](#benchmarks)).

You can read the same schema with either model, and even use both **in one translation unit** (see
[Using both models](#using-both-models)).

> See [`architecture.md`](architecture.md) for the internals and design rationale: the layout planner,
> the compile-time dispatch, the arena, the coexistence design, and the benchmark methodology.

---

## Why RapidProto

RapidProto does one thing: decode Protobuf fast in C++, for the common case where you never serialize.
`protoc`'s C++ runtime is a linked library that builds a full mutable message object per decode;
zero-copy pull parsers like protozero drop that allocation but leave you to hand-write the read loop.
RapidProto generates the decoder — specialized to your schema at compile time, header-only — and gives
you both shapes.

- **Faster decode.** On a realistic mixed payload the arena decoder materializes a full object tree
  **~5× faster than `protoc` + `google::protobuf::Arena`**, and the streaming decoder — materializing
  nothing — is faster still, beating `protozero`, the zero-copy yardstick, on the realistic payload and
  most microbenchmark shapes. See [Benchmarks](#benchmarks).
- **Less memory.** The arena tree holds **~half** of protoc's (both payload bytes and total allocation):
  strings, bytes, and `raw` payloads are **borrowed** as views into the input rather than copied, so the
  arena carries only a read-only bump-allocated tree with no per-field object overhead.
- **Header-only, nothing to link.** One CLI turns your `.proto` into headers you `#include` — no runtime
  library, no `libprotobuf` on your link line.
- **Two models, one schema.** Materialize a navigable tree (arena) *or* stream each field to a typed
  callback with zero allocation — chosen per call site, even in one translation unit.
- **Safe on untrusted input.** Both models fully validate wire structure (truncation, length overruns,
  group nesting) and are built never to crash on malformed bytes; only field *values* are trusted to the
  schema.
- **Compile-time typed.** Dispatch is entirely compile-time — no `std::function`, no virtual calls — so
  misuse (wrong value type, renamed field) is a compile error, not a silent bug.

Know the trade-offs: RapidProto is **decode-only** (no serialization, no JSON codec, no
reflection) — the one exception being an opt-in [debug dumper](#debug-dumper) that emits JSON-*like*
inspection text — the arena tree is **read-only** (you navigate it, you don't build or mutate messages), it
decodes enums as **open** even when the schema declares them closed, and it does not validate string
UTF-8 (it accepts `string` bytes `protoc` would reject). If you also need to *produce* or mutate
messages, keep `protoc` for that side and use RapidProto for the hot decode path.

---

## Quick start

**Requirements:** C++17 and a recent GCC or Clang. Header-only — nothing to link.

Using CMake? The [`rapidproto_generate()` helper](#cmake-integration) wires generation into your build in
a few lines. This section drives the tool by hand so each step is visible. Build it once:

```sh
cmake --preset release                               # system compiler, optimized
cmake --build --preset release --target rapidprotoc
# binary: build/release/rapidprotoc
```

Given `person.proto`:

```proto
syntax = "proto3";
package example;

message Person {
  string name = 1;
  uint32 id = 2;
  repeated string email = 3;   // repeated: navigable array
  Address address = 4;         // sub-message
}

message Address {
  string city = 1;
  string country = 2;
}
```

**1. Generate** the arena decoder (the default model) and a self-contained copy of the runtime, into
`out/`:

```sh
./build/release/rapidprotoc -I. --out-dir=out person.proto   # add -v to log each written file
# out/person.rp.hpp + out/person.rp.common.hpp + out/rapidproto/{runtime,arena_runtime}.hpp
```

**2. Decode.** You supply the serialized message bytes (from a file, socket, database, …) as a
`rapidproto::ByteView` (an alias for `std::string_view`, so a **non-owning** view; for a
`std::uint8_t` buffer, `rapidproto::byte_view(ptr, size)` builds one without a manual cast). Create
an `Arena`, call `decode()`, then navigate the returned tree:

```cpp
#include "person.rp.hpp"

std::string buf = /* the serialized Person bytes */;

rapidproto::Arena arena;
rapidproto::ArenaDecodeError err;
const example::Person* p = example::Person::decode(rapidproto::ByteView(buf), arena, &err);
if (p == nullptr) { /* malformed input: see err.code / err.wire / err.offset */ }

std::uint32_t id = p->id();                        // scalar, by value
std::string_view name = p->name();                 // string, a view into the input buffer
if (const example::Address* a = p->address())      // sub-message: a pointer (nullptr if absent)
    std::string_view city = a->city();
```

> **Need test bytes?** Encode some with `protoc`: `protoc --encode=example.Person -I. person.proto < values.txt > person.bin`

**3. Compile** with only the output directory on the include path:

```sh
g++ -std=c++17 -Iout my_consumer.cpp -o my_consumer
```

That's the arena model. To stream instead, pass `--stream` (or `--arena --stream` for both) and use the
callback API below.

---

## Choosing a model

| | **Arena** | **Streaming** |
|---|---|---|
| What you get | a materialized object tree you read by accessor | a callback fired per field, in wire order |
| Allocation | one bump arena (you own it) | none |
| Random access / re-reading | yes: any field, any order, repeatedly | no: a single forward pass |
| Memory | the whole decoded tree | only what your callbacks keep |
| `#include` | `<stem>.rp.hpp` | `<stem>.rp.stream.hpp` |
| Best for | needing the message as a navigable object; a faster/lighter `protoc`+`Arena` | extracting a few fields, stream-processing, lowest overhead |

Use **arena** when you need the decoded message as an object to navigate — random access, multiple
passes, passing the tree around. Use **streaming** when you handle each field and move on: summing a
column, pulling two fields from a big message, transcoding, or anywhere you want zero allocation. You
can use both models for one schema in one translation unit; see [Using both models](#using-both-models).

---

## Arena decoder

`decode()` reads the whole message into a read-only object tree in a single bump **arena**. Strings and
bytes are **borrowed** as `std::string_view`s into the input wire buffer (zero-copy — no bytes are
copied); the tree's structure (nodes, arrays, maps) lives in the arena. The tree therefore borrows
**both** the arena and the input, so **both must outlive it**. For a self-contained result that owns its
input, use [`decode_owned`](#self-contained-decode-decode_owned). For each message `Foo` the generator
emits a `class Foo`:

```cpp
class Person {
 public:
  [[nodiscard]] static const Person* decode(rapidproto::ByteView input, rapidproto::Arena& arena,
                                            rapidproto::ArenaDecodeError* err = nullptr) noexcept;

  std::uint32_t id() const noexcept;                            // scalar, by value
  std::string_view name() const noexcept;                       // string/bytes, view into the input
  rapidproto::StringArrayView email() const noexcept;           // repeated string (string_view elems)
  const Address* address() const noexcept;                      // sub-message (nullptr if absent)
  // a scalar/string/enum field with EXPLICIT presence returns std::optional<T> (std::nullopt
  // when absent), e.g. `std::optional<std::uint32_t> id() const noexcept;` (no has_<field>() accessor).
};
```

`decode()` returns the root `const Person*` or `nullptr` on malformed input. How each construct is read:

| Construct | Accessor returns |
|---|---|
| scalar / `enum` | the value, by value (`std::int32_t`, `bool`, the generated `enum class`, …); a field with explicit presence instead returns `std::optional<T>` (`std::nullopt` when absent) |
| `string` / `bytes` | `std::string_view` into the **input buffer** (borrowed, zero-copy; valid while both the input and the arena live); `std::optional<std::string_view>` if explicit-presence |
| sub-message | `const Sub*`, a pointer (`nullptr` when absent) |
| `repeated T` | `rapidproto::ArrayView<T>`, a contiguous `{data, size}` range (iterable, indexable). Repeated `string`/`bytes` instead return `rapidproto::StringArrayView`, which yields `std::string_view` per element; for repeated sub-messages, `T` is the value. |
| `map<K, V>` | `rapidproto::MapView<Entry>`: insertion-order entries with `.key()`/`.value()` and a last-wins `find(key)` |
| `oneof o` | a reader `o(handlers…)`: one typed handler per member, with the active member dispatched to its handler (see below) |
| presence | explicit-presence scalar/`string`/`enum` fields carry presence in their `std::optional<T>` return (`std::nullopt` = absent); a message field's presence is its `const T*` accessor returning `nullptr` |

A `oneof` is read with a small visitor, so you can't read an inactive member, and a sub-message member arrives ready to use:

```cpp
// oneof contact { string email = 1; Address work = 2; }
person->contact(
    [](example::Person::Contact::email, std::string_view e)      { use(e); },
    [](example::Person::Contact::work,  const example::Address& a) { use(a.city()); },  // const&, no null-check
    [](std::monostate)                                            { /* unset */ });      // optional
```

Handlers are matched by their tag type, so same-typed members stay distinct; members you omit are ignored, and a single `[](auto, auto){…}` catch-all takes the rest. Each handler returns `void`.

### The arena

`rapidproto::Arena` is a growable, single-threaded **bump allocator** that owns the whole decoded tree.

```cpp
rapidproto::Arena arena;                   // owns its chunks (RAII); frees the whole tree at scope exit
const Foo* a = Foo::decode(buf1, arena);   // tree #1
// … use a …
arena.reset();                             // rewinds for reuse — keeps the chunks, frees nothing
const Foo* b = Foo::decode(buf2, arena);   // tree #2 reuses the same memory (no malloc after warm-up)
```

- **`reset()` for reuse.** Decoding in a loop? `reset()` rewinds the arena (a pointer rewind that keeps
  the chunks), so a steady-state server pays no allocation after the first few decodes. Pointers from a
  previous `decode()` are invalidated by `reset()`.
- **Seed buffer (optional).** `Arena arena{buffer, size}` starts from a caller-owned buffer (e.g. a
  stack array) and only heap-allocates if the tree outgrows it, so small messages need no heap at all.
  (A seed of `alignof(std::max_align_t)` bytes or fewer is too small to be usable and is silently
  ignored.)
- **Bounding memory on untrusted input.** `arena.set_capacity_limit(max_bytes)` caps the total memory
  the arena will reserve; a decode that would grow past it fails cleanly with
  `ArenaDecodeError::OutOfMemory` instead of letting adversarial input allocate without bound
  (the decoded tree can legitimately be larger than the wire bytes). Default: unbounded. Set it
  before decoding, at least as large as any seed buffer. Decoding can transiently reserve several times
  a field's final size before trimming, so size the limit for that peak, not just the final tree.
- **Stats.** `arena.bytes_used()` (payload handed out) and `arena.bytes_reserved()` (memory held).

### Self-contained decode (`decode_owned`)

The low-level `decode()` borrows the input, so you must keep the input buffer alive alongside the
`Arena`. When you'd rather have a result that manages its own lifetime,
`rapidproto::decode_owned<Foo>` takes the input **by value**, decodes into a default `Arena`, and
returns a `std::shared_ptr<const Foo>` that owns **both** the input bytes and the arena:

```cpp
std::string bytes = read_request();                 // input you own
std::shared_ptr<const example::Person> p =
    rapidproto::decode_owned<example::Person>(std::move(bytes));  // move in -> no copy
if (!p) { /* malformed input (pass &err for the reason) */ }
use(p->name());                                     // valid while any copy of `p` lives
```

The handle is freely copyable and shareable; the input and arena are freed together when the last copy
goes away. Reach for the low-level `decode(ByteView, Arena&)` instead when you want to supply your own
`Arena`, or when you hold a `string_view` you'd rather not copy into a `std::string` — then you keep the
input alive yourself.

### Error handling (arena)

`decode()` returns `nullptr` on any failure and, if you pass an `ArenaDecodeError*`, fills in why:

```cpp
struct ArenaDecodeError {
    enum class Code { None, Wire, OutOfMemory, RecursionTooDeep, MissingRequired,
                      RepeatedSingularMessage, StringTooLong, InputTooLarge };
    Code code;
    rapidproto::WireError wire;     // valid when code == Wire
    std::size_t offset;             // byte offset of a wire failure
    std::uint32_t field_number;     // the offending field (MissingRequired / RepeatedSingularMessage)
};
```

- **Wire.** Malformed wire input (truncation, length overrun, group mismatch); `wire`/`offset` locate it.
- **MissingRequired.** A proto2 `required` field was absent (matches `protoc`); `field_number` names it.
- **RecursionTooDeep.** Message nesting exceeded the depth guard (`kMaxDecodeDepth`, 100), which protects
  against adversarial input.
- **OutOfMemory.** The arena could not satisfy an allocation.
- **RepeatedSingularMessage.** A singular (non-repeated) sub-message field appeared more than once
  (rejected, not merged); `field_number` names it.
- **InputTooLarge.** The input exceeded 4 GiB (`UINT32_MAX`), the size at which a repeated/map element
  count or a string length stays representable in the 32-bit view fields.

On any error the tree is incomplete; discard it (or `reset()` the arena) and don't read it.

---

## Streaming decoder

A streaming decoder forwards wire data 1:1, with no aggregation, defaulting, or merging; you decide
what to do with each value. For each message `Foo` the generator emits a `struct Foo` holding a
non-owning `ByteView`, plus a **field-identity tag** type per field:

```cpp
struct Person {
  explicit Person(rapidproto::ByteView bytes) noexcept;

  struct name    { using Value = std::string_view;     /* kNumber=1, kName="name"    */ };
  struct id      { using Value = std::uint32_t;        /* kNumber=2, kName="id"      */ };
  struct email   { using Value = std::string_view;     /* kNumber=3, kName="email"   */ };
  struct address { using Value = ::example::stream::Address; /* kNumber=4, kName="address" */ };

  template <class... Callbacks>
  [[nodiscard]] rapidproto::DecodeStatus decode(Callbacks&&... callbacks) const;
};
```

A callback is `[](Foo::field, Value v){ … }`. The **tag type** names the field (tied to its proto name,
so referencing a removed or renamed field is a compile error), and `Value` is the field's type. Each
tag also carries `static constexpr std::uint32_t kNumber` and `std::string_view kName` (the proto
name). Callbacks fire in **wire order**, once per occurrence (repeated/packed fire per element; maps
per entry). The decoder never materializes the whole message.

> **Absent fields fire nothing, and no defaults are delivered.** If a field isn't on the wire, its
> callback isn't called (and proto3 scalars equal to their default aren't on the wire at all).
> Initialize your own destination variables.

### Three ways to consume fields

All snippets decode a `Person` buffer `wire` (a `rapidproto::ByteView`). `decode()` is `[[nodiscard]]`
and returns a `DecodeStatus`, so **always check it** (see [Error handling](#error-handling-streaming)).

**1. A subset.** Pass callbacks only for the fields you want; the rest are skipped cheaply, so
extracting a few fields from a large message stays fast:

```cpp
std::string name; std::uint32_t id = 0;
example::stream::Person{wire}.decode(
    [&](example::stream::Person::name, std::string_view v) { name = std::string(v); },
    [&](example::stream::Person::id,   std::uint32_t v)    { id = v; });
// email and address are never decoded.
```

**2. A catch-all.** A generic `[](auto tag, auto&& value)` matches every known field you didn't give a
specific callback (logging, generic processing). The tag's `kName`/`kNumber` identify it, and you can
mix a catch-all with specific callbacks (the specific one wins). For a sub-message field, `value` is an
undecoded sub-decoder; a catch-all does **not** recurse, so call `value.decode(...)` yourself.

```cpp
example::stream::Person{wire}.decode([&](auto tag, auto&& value) {
    log("field %s (#%u)", tag.kName.data(), tag.kNumber);
});
```

**3. Known fields *and* unknown ones.** Give specific callbacks, and add a one-argument
`[](rapidproto::UnknownField uf)` that fires for fields whose number is **not in your schema** (a newer
producer's field). This is the forward-compatibility pattern:

```cpp
example::stream::Person{wire}.decode(
    [&](example::stream::Person::name,  std::string_view v) { name = std::string(v); },
    [&](example::stream::Person::email, std::string_view v) { emails.push_back(std::string(v)); }, // per element
    [&](example::stream::Person::address, example::stream::Address a) -> rapidproto::DecodeStatus { // recurse
        return a.decode([&](example::stream::Address::city, std::string_view v) { city = std::string(v); });
    },
    [&](rapidproto::UnknownField uf) {                                  // a field not in our schema
        log("unknown #%u (wire type %d, %zu bytes)", uf.field_number, int(uf.wire_type), uf.bytes.size());
    });
```

`UnknownField` carries `{ std::uint32_t field_number; rapidproto::WireType wire_type; rapidproto::ByteView
bytes; }` (the raw value bytes after the tag). Only field numbers *not in the schema* reach this
handler; a known field you simply didn't handle is not "unknown" (use a catch-all for those). Proto2
`extend` fields are not decoded; an extension on the wire arrives here as a raw `UnknownField`.

### Field kinds

- **Scalars, `string`, `bytes`.** Delivered by value; `string`/`bytes` both arrive as
  `std::string_view` (no UTF-8 validation). The value types match the arena model's scalar mapping.
- **`repeated`.** Fires **once per element**, in wire order (packed or expanded).
- **Sub-messages and groups.** Delivered as a **sub-decoder**; recurse with its `decode(...)`. It
  doesn't decode until you do. Groups behave like sub-messages. `rp_bytes()` exposes the
  sub-decoder's exact undecoded span (a group body arrives without its framing) — see
  [Using both models](#using-both-models) for feeding it to the arena decoder.
- **`map<K, V>`.** The callback takes **`(Tag, K, V)`** and fires once per entry:
  `[&](Person::labels, std::string_view key, std::string_view value) { … }`.
- **`oneof`.** Each member is an ordinary field tag. The member present on the wire fires its callback
  and the others don't, so *the callback that fires is the discriminator*. There's no oneof-level type.

### Error handling (streaming)

`decode()` returns a `rapidproto::DecodeStatus`:

```cpp
struct DecodeStatus {
    rapidproto::WireError wire;    // a wire-format error (None when ok or aborted)
    bool                  aborted; // a callback asked to stop
    std::size_t           offset;  // byte offset of a wire error
    bool ok() const noexcept;      // true unless a wire error or an abort
};
```

- **Wire errors** stop the walk; `status.wire`/`status.offset` say what and where.
- **Partial delivery is not rolled back.** Callbacks fire *as the message is decoded*; if a wire error
  or abort stops the walk part-way, the callbacks that already fired are **not** undone. On a non-ok
  `decode()`, discard whatever your variables hold.
- **Aborting.** A callback may return `void` (continue) **or** `rapidproto::DecodeStatus`, and you can
  mix the two freely. Return `rapidproto::DecodeStatus::abort()` (or propagate a sub-decode's status) to
  stop early.

### Mistakes are compile errors

Dispatch is entirely compile-time (no allocation, no `std::function`, no virtual calls), so misuse is a
**compile error**, not a silent bug:

- **Wrong value type.** `[](Person::id, std::int64_t)` for a `uint32` field → error.
- **Wrapper type.** `[](Person::id, std::optional<std::uint32_t>)` → error.
- **Duplicate.** Two callbacks for the same field → error.
- **Wrong arity.** `[](Person::id)`, or a map callback missing its value → error.
- **Removed/renamed field.** Referencing `Person::nonexistent` → error (the tag type doesn't exist).
- **Another message's field.** Passing `[](Address::city, …)` to `Person`'s `decode()` (say, pasted
  between the nesting levels of the recursion pattern above) → error — it could never fire.

---

## Debug dumper

`--dump` emits a third header, `<stem>.rp.dump.hpp`, that prints a decoded arena tree as
human-readable, JSON-*like* text — a **debugging and logging aid**, not a spec-compliant JSON codec and
not a wire serializer. It reads the arena decoder's public accessors (no reflection, no
`descriptor.proto`), so `--dump` **implies `--arena`** and dumps whatever the arena header exposes. For
each message `Foo` in namespace `example` it emits two free functions:

```cpp
void        example::rp_dump_write(std::ostream& os, const example::Foo& m, std::size_t width = 120);
std::string example::rp_dump_string(const example::Foo& m, std::size_t width = 120);
```

```sh
./build/release/rapidprotoc --dump -I. --out-dir=out person.proto
# out/person.rp.dump.hpp + the arena header + out/rapidproto/dump_runtime.hpp
```

```cpp
#include "person.rp.hpp"
#include "person.rp.dump.hpp"

const example::Person* p = example::Person::decode(rapidproto::ByteView(buf), arena);
std::cout << example::rp_dump_string(*p) << '\n';         // or: rp_dump_write(std::cout, *p, 120);
```

What it renders: scalars, `string`, `bytes` (as lowercase hex), enums by their prefix-stripped name
(`UNKNOWN(<n>)` for an open-enum value outside the schema's range), nested sub-messages, repeated fields
(arrays), maps (objects), and the active member of a oneof; groups print through the identical
nested-message accessor. Default-valued implicit (proto3 singular) fields and empty repeated/maps are
omitted; explicit-presence fields print when set; a `required` field always prints. A message that
reserves the [unknown-fields](#unknown-fields-arena) bit shows `"has_unknown_fields": true` when set —
a bit only, since the arena retains no unknown-field *data*. The output is **width-adaptive**: each
object or array renders on one line if it fits the budget (`width`, default 120 columns), otherwise one
entry per line. Well-known types (`Timestamp`, etc.) print as their nested fields, with no special JSON
form.

---

## Schema features & semantics

These apply to both models and affect how you write correct consumer code.

- **Lifetimes.** Both models borrow the input. Streaming: the input `ByteView` must outlive the decoder
  and every `string_view` it hands a callback. Arena: the tree's structure lives in the `Arena`, but its
  strings/bytes are `string_view`s into the input, so the tree stays valid only while **both** the
  `Arena` and the input buffer live. Use `decode_owned` for a `shared_ptr` that owns both.
- **Untrusted input is validated; values are not.** Wire input is fully checked for **wire-format
  integrity** (structure, lengths, group nesting), so a malformed buffer fails cleanly and never
  triggers UB. Field *values* are not range-checked: RapidProto trusts the schema, not the bytes.
- **Defaults & presence.** Arena: an *implicit*-presence field (plain proto3 scalars) reads back its
  zero default (`0` / `""` / the first enum value) when absent; an *explicit*-presence scalar/string/
  enum field returns `std::optional<T>` (`std::nullopt` when absent — apply a proto2 `[default=X]`
  yourself via `value_or`); a sub-message's presence is its `const T*` accessor returning `nullptr`.
  Streaming: an absent field simply fires no callback, and no defaults are delivered.
- **Enums are open** and **shared between the models.** A proto enum becomes one `enum class :
  std::int32_t` (e.g. `example::Status`) used by *both* decoders. An unrecognized wire value arrives as
  its raw integer cast into the enum; `INT32_MIN`/`INT32_MAX` sentinels force a `default:` arm under
  `-Wswitch`, and `rp_known_min`/`rp_known_max` carry the schema's declared value range (e.g.
  `if (v <= Status::rp_known_max)`). (The generator places the enums in a shared
  `<stem>.rp.common.hpp` that each decoder `#include`s for you, so you never include it directly.) This applies to **closed** enums too
  (proto2, or editions `enum_type = CLOSED`): RapidProto intentionally decodes every enum as open —
  where protoc would route an unrecognized closed-enum value to unknown fields, RapidProto delivers
  the raw value — so do not rely on closed-enum semantics.
- **Well-known types** (`google.protobuf.Timestamp`, etc.) decode as plain messages (their `seconds`/
  `nanos` fields), with no special Timestamp/Duration/Any semantics.
- **Thread-safety.** A streaming `decode()` is `const` and holds no mutable state, so decoders over one
  buffer run concurrently as long as the buffer isn't mutated. An arena `decode()` mutates its `Arena`,
  so give each thread its own arena; the resulting read-only tree can then be shared.

---

## Using both models

The two models live in **different C++ namespaces** for the same schema: arena at `pkg::Msg`, streaming
at `pkg::stream::Msg`, with the schema's enums as a single shared type, so they coexist in
one translation unit. Generate both (`--arena --stream`, or `GENERATOR both` in CMake) and use each
where it fits:

```cpp
#include "person.rp.hpp"         // arena:     example::Person
#include "person.rp.stream.hpp"  // streaming: example::stream::Person  (both pull in the shared enums)

const example::Person* tree = example::Person::decode(bytes, arena);   // materialize when you need an object
example::stream::Person{bytes}.decode( /* … */ );                       // or stream when you don't
// example::Status is the same enum type in both.
```

The models also combine **mid-decode**: stream a large outer message and materialize just the
sub-messages you keep. A streaming sub-decoder's `rp_bytes()` is exactly the sub-message's field
bytes, which the arena `decode()` accepts directly. The materialized tree borrows those bytes (a slice
of the input `wire`), so `wire` must outlive every tree you keep:

```cpp
rapidproto::Arena arena;
example::stream::Person{wire}.decode(
    [&](example::stream::Person::address, example::stream::Address a) {
        const example::Address* tree = example::Address::decode(a.rp_bytes(), arena);
        // keep `tree` -- valid while both `wire` and `arena` live
    });
```

Every tree materialized this way accumulates in the arena until you `reset()` it. If a tree is a
per-element **temporary** (use, then discard), reset between uses — even from inside a callback,
mid-walk: the streaming side borrows the *input buffer*, never the arena, so rewinding it there is
safe and keeps a long stream's memory flat.

A runnable end-to-end example (one schema, both models in one TU, including the mid-decode
hybrid) is in [`examples/consumer`](examples/consumer).

### Coexisting with protoc

By default a proto `package a.b` maps to C++ `namespace a::b`, the same as protoc, so you can't include
both protoc's `.pb.h` and a RapidProto header for the same message in one TU (they'd define `a::b::Msg`
twice). If you need both (protoc for serialization, RapidProto for fast decoding), nest the generated
code under a prefix with `--namespace-prefix`:

```sh
rapidprotoc --namespace-prefix=rp -I. --out-dir=out person.proto
# -> namespace rp::example { class Person … }
```

Now `rp::example::Person` (RapidProto) and `example::Person` (protoc) coexist.

---

## Advanced: decode profiles & unknown fields

These arena-only features tailor decoding to what *this* consumer needs, without touching
the schema.

### Unknown fields (arena)

By default, fields not in your schema (a newer producer's field, or a proto2 extension) are **skipped
and dropped**. To *detect* (not recover) that unknowns were present, reserve a per-message "saw an
unknown field" flag, exposed as `has_unknown_fields()`:

- `--unknown-present` reserves it on **every** message.
- `--unknown=<pkg.Msg>` (repeatable), or an `unknown-fields <pkg.Msg>` line in a decode profile,
  reserves it on **one** message — so you pay the bit only where you check it.

The selection is part of the [decode profile](#decode-profiles-drop-raw-and-unknown-fields-arena): it
folds into the profile identity, so two TUs that disagree about which messages carry the flag **fail to
link** rather than silently holding mismatched layouts of the same type.

### Decode profiles: `drop`, `raw`, and `unknown-fields` (arena)

The schema says what *can* be on the wire; a **decode profile** says what *this consumer* does with
it — without touching the schema. Choose, per field (`pkg.Msg.field`), per type (`pkg.Msg`, covering
every field of that message type), or per message (for `unknown-fields`):

- **`drop`** — no storage, no accessor, no decode work beyond wire-validated skipping. Reading a
  dropped field is a **compile error**, not a silent default. (Dropping a `required` field is
  rejected.)
- **`unknown-fields`** — reserve that **message**'s `has_unknown_fields()` bit (see
  [Unknown fields](#unknown-fields-arena)). Unlike `drop`/`raw` it names a message directly, not a
  field or a field's type; an enum or a field name is an error.
- **`raw`** (message-typed fields, groups included) — the sub-message's **payload** is borrowed as a
  `ByteView` view into the input instead of a materialized tree; repeated fields become a
  `StringArrayView`, one payload per element. Each view is exactly what the field type's own
  `decode()` accepts, so the tree is built only when — and if — you ask: keep a huge or
  rarely-read sub-message (or a million-element repeated field) as bytes, and decode single
  elements on demand. Decode semantics are otherwise unchanged (presence, `required` validation,
  duplicate-singular rejection). Scalars, strings, and enums can't go raw (no payload a later
  `decode()` could consume — they're cheap to materialize or drop), nor can maps (their entry
  type is generated internals). To defer a huge *packed scalar* array, wrap it in a sub-message
  schema-side, or walk it with the streaming decoder.

Profiles come from a file (one `drop <name>` / `raw <name>` / `unknown-fields <message>` per line, `#`
comments, an optional `name <identifier>` line) via `--field-modes=<file>`, or inline via
`--drop=<name>` / `--raw=<name>` / `--unknown=<message>` (and `--unknown-present` for every message). A
field-level entry beats a type-level entry; unknown names are hard errors; field modes do not
apply inside a oneof. The profile resolves against *everything* the invocation generates — the
entries resolve as one batch — so a global profile works by listing (or `PROTOS`-listing, in
CMake) every schema it spans in one generation; a name unknown across the whole batch is still a
hard error.

```
# lean.modes — this consumer never reads sides, and reads origin only on demand
name lean
drop demo.Shape.sides
raw  demo.Shape.origin
```

```cpp
const demo::Shape* s = demo::Shape::decode(bytes, arena);   // s->sides() does not compile
if (s->origin()) {                                          // the Point payload (borrowed from the input)
    const demo::Point* p = demo::Point::decode(*s->origin(), arena);  // deferred: only now
}
```

A profile **changes the generated types**, so you still write `demo::Shape`, but two TUs generated
under different profiles (including differing only in which messages reserve the unknown-fields bit)
hold distinct types and **fail to link** rather than silently exchanging mismatched layouts. One
practical consequence: don't forward-declare generated types yourself — under a profile, a plain
`namespace demo { class Shape; }` declares a *different* class. See
`examples/consumer/lean_main.cpp` for the full pattern.

---

## The `rapidprotoc` CLI

```
rapidprotoc [options] <entry.proto>...
```

| Flag | Meaning |
|---|---|
| `--arena` | Emit the arena decoder (`<stem>.rp.hpp`). **The default** if neither model flag is given. |
| `--stream` | Emit the streaming decoder (`<stem>.rp.stream.hpp`). Combine with `--arena` to emit both. |
| `--dump` | Emit the [debug dumper](#debug-dumper) (`<stem>.rp.dump.hpp`), a JSON-like text dumper over the arena tree. Implies `--arena`. |
| `--unknown-present` | Arena: reserve the "unknown fields present" bit (`has_unknown_fields()`) on **every** message. |
| `--unknown=<message>` | Arena: reserve that bit on **one** message (repeatable; a one-line `unknown-fields` profile entry). |
| `--field-modes=<file>` | Arena: apply a decode profile file (repeatable; see [Decode profiles](#decode-profiles-drop-raw-and-unknown-fields-arena)). |
| `--drop=<name>` | Arena: drop one field or type inline (as a one-line profile entry). |
| `--raw=<name>` | Arena: keep a message field's or type's payloads for deferred `decode()`s, inline. |
| `-I <dir>` | Add an import search path (repeatable). |
| `--out-dir <dir>` | Where to write the headers (and `rapidproto/runtime.hpp`, plus `arena_runtime.hpp` for `--arena` and `dump_runtime.hpp` for `--dump`). Default: the current directory. |
| `--namespace-prefix <ns>` | Dot-separated prefix prepended to every C++ namespace (see [Coexisting with protoc](#coexisting-with-protoc)). |
| `--no-wellknown` | Don't load the bundled well-known-type definitions. |
| `--depfile <path>` | Write a Make/Ninja depfile (the entries' headers depend on **every** input `.proto` and profile file) so a build regenerates when any input changes. Used by the CMake helper; harmless otherwise. |
| `-v`, `--verbose` | Log each written file (`wrote <path>`); output is otherwise silent on success. |
| `-h`, `--help` | Print the full flag table and exit. |
| `--version` | Print the tool version and exit. |

Multiple entries resolve as **one batch**: shared imports parse once, every file in the union
gets its decoder exactly once, and a decode profile resolves against all of them together. Each
generated file covers the entry **and** its transitive imports **and** the well-known types it
uses, plus the shared `<stem>.rp.common.hpp` and the runtime, so the output directory is
self-contained.

---

## CMake integration

RapidProto ships a `rapidproto_generate()` helper that turns a `.proto` into a linkable, header-only
target: it runs `rapidprotoc` at build time, tracks the whole import closure for correct **incremental
rebuilds** (a touched import re-triggers generation, via a depfile), and puts the output directory on
your include path. Link the target and `#include` the generated header; there's nothing else to wire up.

```cmake
rapidproto_generate(my_schema
  GENERATOR   both                  # arena | stream | both           (default: arena)
  PROTOS      proto/person.proto    # one or more entry .proto files
  IMPORT_DIRS proto)                # -I roots your schema imports against
  # also: NAMESPACE_PREFIX <ns>, OUT_DIR <dir>, UNKNOWN_PRESENT (arena), NO_WELLKNOWN, DEBUG (arena dumper),
  #       FIELD_MODES <file>... / DROP <name>... / RAW <name>... / UNKNOWN <message>...  (arena profiles)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE my_schema)   # generates before `app` compiles, adds the include dir
```

Then `#include "person.rp.hpp"` (arena) and/or `"person.rp.stream.hpp"` (streaming); each is the
entry's stem under its import-relative path. `GENERATOR both` writes both decoders from one `rapidprotoc`
invocation, so a single TU can use both models for one schema (see [Using both models](#using-both-models)).

Get the helper and the `rapidproto::rapidprotoc` tool it drives, either way:

```cmake
# Build from source within your build:
include(FetchContent)
FetchContent_Declare(rapidproto GIT_REPOSITORY <url> GIT_TAG <tag>)
FetchContent_MakeAvailable(rapidproto)         # defines rapidproto_generate() + rapidproto::rapidprotoc

# …or use an installed RapidProto (cmake --install <build> --prefix <prefix>):
find_package(rapidproto REQUIRED)              # same helper + tool, imported
```

Both expose the identical `rapidproto::rapidprotoc` target, so one `rapidproto_generate()` call is
source-agnostic.

**CMake version.** Incremental import-tracking uses `add_custom_command(DEPFILE)`: supported on Ninja at
any version, and on the Makefile generators with CMake ≥ 3.20 (Xcode / Visual Studio ≥ 3.21). On an
older CMake with those generators the helper still generates correctly but won't auto-retrigger on an
import edit (it warns); re-run CMake or clean-build after editing an imported `.proto`.

**Cross-compiling.** `rapidprotoc` must run on the **build host**, not the target, so it must be a
**host build**. Build/install RapidProto for the host and bring that host tool in (e.g. a host-prefixed
`find_package`). `rapidproto_generate()` rejects the in-tree (target-built) tool when
`CMAKE_CROSSCOMPILING` is set; ensure the imported `rapidproto::rapidprotoc` it sees is a host binary.

---

## Benchmarks

The numbers below come from the in-repo harness (`tests/bench.py`), decoding a realistic `Dataset`
payload — 2000 mixed records with strings, nested and repeated messages, and packed scalar arrays —
that `protoc` serializes and every decoder then parses. Built `-O3 -DNDEBUG` and measured on **g++-13**
and **clang++-20** against **protobuf 4.25.3**. Reproduce with `python3 tests/bench.py run` (the full
methodology is in [architecture.md](architecture.md)). The multipliers are decode **throughput** (GB/s)
ratios — the number that matters, since a schema-specialized decoder wins as much from branch prediction,
pipelining, and superscalar execution as from fewer instructions. Every decoder is measured in the same
run under identical conditions, so the ratios are stable; treat them as a box-specific rule of thumb, not
a cross-machine constant.

**Arena vs `protoc` + `google::protobuf::Arena`.** Both materialize a full object tree, so this is a
like-for-like comparison of decode speed and peak arena memory. RapidProto **borrows** strings, bytes,
and `raw` payloads as views into the input instead of copying them, so the arena holds only tree
structure — which is where most of the memory win comes from:

| Metric | RapidProto arena | protoc + Arena |
|---|---|---|
| Decode throughput | **~5× faster** | baseline |
| Peak memory, payload (arena `bytes_used` vs protoc `SpaceUsed`) | **0.49×** | 1× |
| Peak memory, total held (arena `bytes_reserved` vs protoc `SpaceAllocated`) | **0.56×** | 1× |

That's the g++-13 figure; clang++-20 measures ~6×. The decoded tree borrows the input, so it stays valid
only while both the input and the `Arena` outlive it (or use
[`decode_owned`](#self-contained-decode-decode_owned) for a self-contained handle).

**Streaming vs `protozero`.** Both are zero-materialization pull parsers. On the realistic `Dataset` the
streaming decoder is **~2× faster than protozero** — and **~13× faster than `protoc` + `Arena`**, since
it materializes nothing. Across the per-field microbenchmarks it's faster on most shapes (repeated
fields, nested messages, skip-heavy records), about even on single fixed-width scalars, and slower only
on large **packed** arrays, which it decodes one element per callback — decode those with the arena model
(below).

**Arena vs streaming (the two RapidProto models).** The streaming decoder is **~2.7× faster** than the
arena decoder on the `Dataset`, since it builds no object tree. Use the arena model when you want a
navigable, random-access object; stream when you only extract or forward fields.

**Large packed scalar arrays: prefer the arena model.** The arena decoder decodes a packed varint array
a word at a time straight into its array, while the streaming decoder decodes it one element per callback
(a streaming callback takes a single value and can't batch its store). So if your hot path is dominated
by large packed scalar fields, the **arena** decoder is the faster choice.

Speedups vary with payload shape, and part of the arena/protoc gap is a feature gap — protoc validates
UTF-8 on every proto3 string and RapidProto does not. The harness ships every scenario (and a memory
report), so measure your own payloads rather than trusting one ratio as universal.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for building, the `./check.sh` quality gate, and how the golden
tests work. The design and the invariants a change must preserve are in
[architecture.md](architecture.md).

## Security

RapidProto decodes **untrusted** wire input and is built never to crash on it. See
[SECURITY.md](SECURITY.md) to report a vulnerability or read the threat model.

---

## License

RapidProto is licensed under the **Apache License 2.0**; see [`LICENSE`](LICENSE), with attributions in
[`NOTICE`](NOTICE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

The vendored runtimes (`rapidproto/runtime.hpp`, `rapidproto/arena_runtime.hpp`, and
`rapidproto/dump_runtime.hpp`) carry the same Apache-2.0 license, so the headers `rapidprotoc` drops
into your out-dir are usable under those terms.
The decoder code generated *from your schema* is your own work product, and RapidProto claims no rights
over it. The embedded Protocol Buffers well-known-type definitions are Copyright 2008 Google Inc.,
licensed 3-Clause BSD. Catch2 and protozero are development-time dependencies and are not distributed.
