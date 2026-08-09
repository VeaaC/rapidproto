# The arena decoder

*The default model. Header: `<stem>.rp.hpp`. Back to the [README](../README.md); shared rules
(lifetimes, presence, enums) in [semantics.md](semantics.md).*

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

## The arena

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

## Self-contained decode (`decode_owned`)

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

## Error handling

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
- **RepeatedSingularMessage.** A singular (non-repeated) sub-message appeared more than once, which
  protobuf merges and a read-only tree cannot; `field_number` names the field you wrote (for a map,
  the map itself). Also covers a sub-message oneof member repeating while the oneof still holds it,
  and a map entry repeating its `value` — see [duplicate fields](semantics.md).
- **InputTooLarge.** The input exceeded 4 GiB (`UINT32_MAX`), the size at which a repeated/map element
  count or a string length stays representable in the 32-bit view fields. (`StringTooLong` is reserved
  and never produced.)

On any error the tree is incomplete; discard it (or `reset()` the arena) and don't read it.

## See also

- [Decode profiles & unknown fields](profiles.md) — tailor what the arena decoder materializes
  (`drop` / `raw` / `has_unknown_fields()`), per consumer, without touching the schema.
- [The debug dumper](dumper.md) — print a decoded arena tree as JSON-like text.
- [Using both models](using-both-models.md) — combine arena and streaming, even mid-decode.
- [Benchmarks](benchmarks.md) — how the arena compares to `protoc` + `google::protobuf::Arena`.
