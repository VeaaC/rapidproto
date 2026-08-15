# The streaming decoder

*Generated with `--stream`. Header: `<stem>.rp.stream.hpp`, types at `pkg::stream::Msg`. Back to the
[README](../README.md); shared rules (lifetimes, presence, enums) in [semantics.md](semantics.md).*

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

  template <class... rp_Callbacks>
  [[nodiscard]] rapidproto::DecodeStatus decode(rp_Callbacks&&... rp_callbacks) const;
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

A schema with a **top-level enum named `stream`** cannot use this model: the enum takes package
scope in the shared common header, which is the name this header needs for its own namespace. Rename
the enum. (Nested enums, messages, and fields of that name are fine — as is a top-level *message*,
until you [pair the two models](using-both-models.md) in one translation unit.)

## Three ways to consume fields

All snippets decode a `Person` buffer `wire` (a `rapidproto::ByteView`). `decode()` is `[[nodiscard]]`
and returns a `DecodeStatus`, so **always check it** (see [Error handling](#error-handling)).

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
bytes; }` — the field's bytes **as they appear on the wire after the tag**, so a LEN field's view
starts with its length prefix and a group's ends with its closing end-group tag. That framing is why
these bytes are **not** what another decoder's `decode()` takes — a sub-decoder's `rp_bytes()` is
(see [using both models](using-both-models.md)); strip the prefix yourself if you want to decode an
unknown field. Only field numbers
*not in the schema* reach this handler; a known field you simply didn't handle is not "unknown" (use
a catch-all for those). Proto2
`extend` fields are not decoded; an extension on the wire arrives here as a raw `UnknownField`.

## Field kinds

- **Scalars, `string`, `bytes`.** Delivered by value; `string`/`bytes` both arrive as
  `std::string_view` (no UTF-8 validation). The value types match the arena model's scalar mapping.
- **`repeated`.** Fires **once per element**, in wire order (packed or expanded).
- **Sub-messages and groups.** Delivered as a **sub-decoder**; recurse with its `decode(...)`. It
  doesn't decode until you do. Groups behave like sub-messages. `rp_bytes()` exposes the
  sub-decoder's exact undecoded span (a group body arrives without its framing) — see
  [Using both models](using-both-models.md) for feeding it to the arena decoder.
- **`map<K, V>`.** The callback takes **`(Tag, K, V)`** and fires once per entry:
  `[&](Person::labels, std::string_view key, std::string_view value) { … }`.
- **`oneof`.** Each member is an ordinary field tag. The member present on the wire fires its callback
  and the others don't, so *the callback that fires is the discriminator*. There's no oneof-level type.
  On a buffer carrying more than one member — two serialized messages concatenated, say — each fires
  in wire order, and the **last** is the one protobuf would call set (see
  [duplicate fields](semantics.md)).

Enums decode open, so a `switch` over one needs a `default:` arm — see [semantics](semantics.md).

## Error handling

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

## Mistakes are compile errors

Dispatch is entirely compile-time (no allocation, no `std::function`, no virtual calls), so misuse is a
**compile error**, not a silent bug:

- **Wrong value type.** `[](Person::id, std::int64_t)` for a `uint32` field → error.
- **Wrapper type.** `[](Person::id, std::optional<std::uint32_t>)` → error.
- **Duplicate.** Two callbacks for the same field → error.
- **Wrong arity.** `[](Person::id)`, or a map callback missing its value → error.
- **Removed/renamed field.** Referencing `Person::nonexistent` → error (the tag type doesn't exist).
- **Another message's field.** Passing `[](Address::city, …)` to `Person`'s `decode()` (say, pasted
  between the nesting levels of the recursion pattern above) → error — it could never fire.

## See also

- [Using both models](using-both-models.md) — stream the outer message, materialize chosen
  sub-messages with the arena decoder.
- [Benchmarks](benchmarks.md) — how the streaming decoder compares to protozero (and when the arena
  model is faster, e.g. large packed arrays).
