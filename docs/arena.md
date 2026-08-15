# The arena decoder

*The default model. Header: `<stem>.rp.hpp`. Back to the [README](../README.md); shared rules
(lifetimes, presence, enums) in [semantics.md](semantics.md).*

`decode()` reads the whole message into a read-only object tree in a single bump **arena**. Strings and
bytes are **borrowed** as `std::string_view`s into the input wire buffer (zero-copy); the structure
(nodes, arrays, maps) lives in the arena. The tree borrows **both** the arena and the input, so **both
must outlive it**. For a result that owns its input, use [`decode_owned`](#self-contained-decode-decode_owned). For each message `Foo` the generator
emits a `class Foo`:

```cpp
class Person {
 public:
  [[nodiscard]] static const Person* decode(rapidproto::ByteView input, rapidproto::Arena& arena,
                                            rapidproto::ArenaDecodeError* err = nullptr) noexcept;

  std::uint32_t id() const noexcept;        // one const getter per field
  std::string_view name() const noexcept;   // string: a view into the input buffer
  const Address* address() const noexcept;  // sub-message: a pointer, null when absent
};
```

## What each field kind returns

A [decode profile](profiles.md) changes these types where it applies — a `drop`ped field loses its
accessor, a `raw` one returns undecoded bytes.

| Construct | Accessor returns |
|---|---|
| scalar / `enum` | the value, by value (`std::int32_t`, `bool`, the generated `enum class`, …); absent reads as the zero default, indistinguishable from a written zero ([semantics](semantics.md)) |
| `string` / `bytes` | `std::string_view` into the **input buffer** (borrowed, zero-copy); `std::optional<std::string_view>` when marked `optional`. Not NUL-terminated, and a `bytes` value may contain NULs — compare with `==` on the view, never hand `.data()` to a C string API |
| sub-message | `const Sub*`, `nullptr` when absent — the pointer carries the presence. (A proto2 `required` field is never null: absence fails the decode as `MissingRequired`.) |
| `repeated T` | `rapidproto::ArrayView<T>`, a contiguous range with `size()`, `empty()`, `operator[]` and range-`for`. Elements are **values**, not pointers — a repeated field has no presence. Repeated `string`/`bytes` instead return `rapidproto::StringArrayView`, yielding `std::string_view` per element. |
| `map<K, V>` | `rapidproto::MapView<Entry>`: insertion-order entries with `.key()`/`.value()`. `find(key)` returns an iterator to compare against `end()`, as `std::map` does, and `it->value()` reads the entry. A message-valued `.value()` is itself `const V*`; test it before dereferencing. Duplicate keys are kept rather than collapsed ([duplicate fields](semantics.md)). |
| `oneof o` | a reader `o(handlers…)`: one typed handler per member, with the active member dispatched to its handler (see below) |
| `optional` on a scalar/`string`/`bytes`/`enum` | `std::optional<T>` (`std::nullopt` = absent). There is no `has_<field>()` accessor, and the keyword is a no-op on a message field — that pointer already carries presence. |

## Reading a generated schema

In a generated schema `pkg.Foo` becomes `pkg::Foo`, and each accessor is its proto field name. A
map's entry type is nested in its message: `Foo::LabelsEntry`. Any name that would clash with C++ or
with the generated API takes a trailing `_` — messages, enums and package components included
(`enum std` → `std_`). Reading a `Person` carrying one of each shape:

```cpp
// message Person { string name = 1; uint32 id = 2; Address address = 3;
//                  repeated Phone phones = 4; map<string, string> labels = 5;
//                  optional string nickname = 6; }

std::string describe(const example::Person* p) {
  std::string out(p->name());                                       // string: a view into the input
  out += std::to_string(p->id());                                   // scalar: by value
  if (const example::Address* a = p->address()) out += a->city();   // message: null when absent
  for (const example::Phone& ph : p->phones()) out += ph.number();  // repeated: elements are values
  const auto labels = p->labels();
  if (auto it = labels.find("env"); it != labels.end()) out += it->value();
  out += p->nickname().value_or("");                                // `optional`: std::optional<T>
  return out;
}
```

Two mistakes the view types invite on any schema, neither diagnosable from what the compiler prints
(gcc-13 below; clang-20 words both differently):

| If you write | you get | write instead |
|---|---|---|
| `for (const auto* ph : p->phones())` | `unable to deduce ‘const auto*’` (clang: `incompatible initializer of type ‘const Phone’`) | `for (const example::Phone& ph : …)` |
| `for (auto& [k, v] : p->labels())` | `cannot decompose inaccessible member … ‘rp_key’` (clang: `private member`) | `for (const auto& e : …)`, then `e.key()` / `e.value()` |

Enums decode open, so a `switch` over one needs a `default:` arm — see [semantics](semantics.md).

A `oneof` is read with a visitor, so an inactive member cannot be read:

```cpp
// oneof contact { string email = 1; Address work = 2; }
person->contact(
    [](example::Person::Contact::email, std::string_view e)      { use(e); },
    [](example::Person::Contact::work,  const example::Address& a) { use(a.city()); },  // const&, no null-check
    [](std::monostate)                                            { /* unset */ });      // optional
```

Handlers are matched by their tag type, so same-typed members stay distinct; members you omit are ignored, and a single `[](auto, auto){…}` catch-all takes the rest. Each handler returns `void` — the tree is already
decoded, so there is nothing to abort.

## Memory & lifetimes

`rapidproto::Arena` is a growable, single-threaded **bump allocator** that owns the whole decoded tree.

```cpp
rapidproto::Arena arena;                   // owns its chunks (RAII); frees the whole tree at scope exit
const Foo* a = Foo::decode(buf1, arena);   // tree #1
// … use a …
arena.reset();                             // rewinds for reuse — keeps the chunks, frees nothing
const Foo* b = Foo::decode(buf2, arena);   // tree #2 reuses the same memory (no malloc after warm-up)
```

- **Don't hand it a temporary.** `ByteView` is `std::string_view`, so a `std::string` temporary binds
  silently and dangles at the semicolon. `buf.substr(n)` returns a *new* `std::string`; slice the
  view instead — `ByteView(buf).substr(n)`.
- **`reset()` invalidates everything.** Every pointer **and view** from an earlier `decode()` dangles —
  `ArrayView`/`MapView`/`StringArrayView` and by-value copies of decoded nodes hold arena pointers
  too; only a `string_view` scalar is exempt, since it borrows the input. `reset()` frees nothing,
  so stale reads keep looking right until the next decode reuses the memory.
- **Seed buffer (optional).** `Arena arena{buffer, size}` starts from a caller-owned buffer (e.g. a
  stack array) and heap-allocates only if the tree outgrows it.
- **Bounding memory on untrusted input.** `arena.set_capacity_limit(max_bytes)` caps the total memory
  the arena will reserve; a decode that would grow past it fails with `ArenaDecodeError::OutOfMemory`.
  Default: unbounded. Set it before decoding, and at least as large as any seed buffer — a smaller
  cap leaves the arena unable to grow past the seed. The decoded tree can legitimately be larger than the
  wire bytes, and decoding can transiently reserve several times a field's final size before trimming —
  size the limit for that peak, not the final tree.
- **Stats.** `arena.bytes_used()` (payload handed out) and `arena.bytes_reserved()` (memory held).

## Self-contained decode (`decode_owned`)

`rapidproto::decode_owned<Foo>` takes the input **by value**, decodes into a default `Arena`, and
returns a `std::shared_ptr<const Foo>` that owns **both** the input bytes and the arena:

```cpp
std::string bytes = read_request();                 // input you own
std::shared_ptr<const example::Person> p =
    rapidproto::decode_owned<example::Person>(std::move(bytes));  // move in -> no copy
if (!p) { /* malformed input (pass &err for the reason) */ }
use(p->name());                                     // valid while any copy of `p` lives
```

Bind the handle to a named variable before reading through it: in
`for (const auto& a : decode_owned<Foo>(b)->items())` the handle dies at the end of the range-init,
before the body runs. And because the `Arena` is created inside, `set_capacity_limit` is not
available — use `decode(ByteView, Arena&)` for untrusted input, or when you hold a `string_view`
you'd rather not copy into a `std::string`.

## Error handling

`decode()` returns `nullptr` on any failure and, if you pass an `ArenaDecodeError*`, fills in why.
It writes `err` only on failure and never clears it, so test the returned pointer — a struct reused
across a loop still holds the previous failure:

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

| `Code` | Meaning |
|---|---|
| `Wire` | Malformed wire input (truncation, length overrun, group mismatch); `wire`/`offset` locate it |
| `MissingRequired` | A proto2 `required` field was absent (matches `protoc`); `field_number` names it |
| `RecursionTooDeep` | Message nesting exceeded the depth guard (`kMaxDecodeDepth`, 100). Nested *groups* hit their own guard and report `Wire` with `GroupTooDeep` |
| `OutOfMemory` | The arena could not satisfy an allocation |
| `RepeatedSingularMessage` | A singular sub-message appeared more than once, which protobuf merges and a read-only tree cannot; `field_number` names the field (for a map, the map itself). Covers four more shapes — see [duplicate fields](semantics.md) |
| `InputTooLarge` | The input exceeded `UINT32_MAX` bytes |
| `StringTooLong` | Reserved; never produced |

On any error the tree is incomplete; discard it (or `reset()` the arena) and don't read it.

## See also

- [Decode profiles & unknown fields](profiles.md) — tailor what the arena decoder materializes
  (`drop` / `raw` / `has_unknown_fields()`).
- [The debug dumper](dumper.md) — print a decoded arena tree as JSON-like text.
- [Using both models](using-both-models.md) — combine arena and streaming, even mid-decode.
- [Benchmarks](benchmarks.md) — how the arena compares to `protoc` + `google::protobuf::Arena`.
