# The arena decoder

The arena decoder turns the input into a message in one pass. You get a pointer to a `const` message
and read its fields through accessors, in any order. The message is built in a `rapidproto::Arena`
you own. Its strings and bytes borrow from the input. Neither the arena nor the input may be
destroyed while you still read the message. If you'd rather handle each field as it's read and build
no message, use the [streaming decoder](streaming.md), or [combine the two](using-both-models.md).

The examples on this page use the schema from the [quick start](../README.md#quick-start) with a few
fields added:

```proto
syntax = "proto3";
package example;

message Person {
  string name = 1;
  uint32 id = 2;
  repeated string email = 3;
  Address address = 4;
  optional string nickname = 5;
  repeated Phone phones = 6;
  map<string, string> labels = 7;
  oneof workplace {
    Address office = 8;
    string timezone = 9;
  }
}

message Address {
  string city = 1;
  string country = 2;
}

message Phone {
  string number = 1;
}
```

```cpp
#include "person.rp.hpp"

#include <iostream>

namespace ex = rp::arena::example;

bool print_person(rapidproto::ByteView input) {
    rapidproto::Arena arena;
    const ex::Person* person = ex::Person::decode(input, arena);
    if (person == nullptr) {
        std::cerr << "not a valid Person\n";
        return false;
    }
    std::cout << person->name() << " (" << person->id() << ")\n";
    for (std::string_view email : person->email()) {
        std::cout << "email " << email << '\n';
    }
    if (const ex::Address* address = person->address()) {
        std::cout << "lives in " << address->city() << '\n';
    }
    for (const ex::Phone& phone : person->phones()) {
        std::cout << "phone " << phone.number() << '\n';
    }
    std::cout << "nickname " << person->nickname().value_or("(none)") << '\n';
    return true;
}
```

For protoc's encoding of
`name: "Ada" id: 7 email: "ada@x.org" address { city: "London" } phones { number: "555-0100" }`,
the function prints:

```text
Ada (7)
email ada@x.org
lives in London
phone 555-0100
nickname (none)
```

To print every field without writing code like this, use the [debug dumper](dumper.md), which turns
any decoded message into JSON-like text.

Every message class gets a `decode` like this one in `Person`:

```cpp
class Person {
  public:
    [[nodiscard]] static const Person* decode(rapidproto::ByteView input, rapidproto::Arena& arena,
                                              rapidproto::ArenaDecodeError* err = nullptr) noexcept;
};
```

`rapidproto::ByteView` is an alias for `std::string_view`. If you hold the input as a
`const std::uint8_t*` plus a length, `rapidproto::byte_view(ptr, size)` wraps it. On failure
`decode` returns `nullptr`. The optional `err` [records why](#errors).

Classes for `package a.b;` land in `rp::arena::a::b`, and a schema without a package puts them in
`rp::arena` itself. [`--namespace-prefix`](integration.md#the-rapidprotoc-cli) replaces the `rp`.
A message defined inside another one becomes a nested class of it. Accessor names match the field
names, case included. The generator appends an underscore to a name that would collide with C++ or
with its own code, such as a keyword, `std`, a macro like `EOF` or `linux`, or anything starting
with `rp_`. A message named `class` becomes `class_`.

## What each field kind returns

| Field | Accessor returns |
| --- | --- |
| number, `bool` | the value, such as `std::uint32_t` |
| enum | the generated enum type |
| `string`, `bytes` | `std::string_view` |
| message, group | `const T*`, null when absent |
| repeated number, `bool`, enum, message | `rapidproto::ArrayView<T>` |
| repeated `string`, `bytes` | `rapidproto::StringArrayView` |
| `map<K, V>` | `rapidproto::MapView<Entry>` |
| `oneof` | one method that takes handlers |

Enums are [open](semantics.md). A number the enum doesn't define comes through unchanged, and a
`switch` without a `default:` misses it. Nothing checks that strings are valid UTF-8.

A number, enum, string or bytes field with explicit presence returns a `std::optional`, as
`nickname()` does. Any field marked `optional` in proto2 or proto3 has explicit presence. An
editions field has it too, unless its `field_presence` works out to `IMPLICIT`, whether from its own
option or from the file's. Without explicit presence, an absent `id` reads as 0, just like an `id`
sent as 0. A `required` field (`LEGACY_REQUIRED` in editions) also returns the plain type, since a
decode that's missing it fails. `[default = X]` isn't applied: such a field reads as
`std::nullopt` when it's absent, not as X.

Sub-messages signal presence through the pointer alone. There are no `has_` accessors, and declaring
a message field `optional` leaves its accessor as it is. A `required` message field is never null.

String views cover their field and nothing more. No NUL is promised after one, and a `string` or
`bytes` value may hold zeros of its own. A C function given `.data()` can run off the end of the
field or stop at an inner zero.

`ArrayView` wraps a contiguous array and offers `size()`, `empty()`, `data()`, iterators and an
unchecked `operator[]`. A repeated message stores its elements in place. The loop above binds each
as `const ex::Phone&`; a `const auto*` loop variable won't compile. `StringArrayView`, which
`email()` returns, yields a `std::string_view` per element and has no `data()`.

For a map field you get a `MapView`, which you search with `find` or walk with a loop:

```cpp
const auto labels = person->labels();
if (auto it = labels.find("team"); it != labels.end()) {
    std::cout << "team " << it->value() << '\n';
}
for (const auto& label : labels) {
    std::cout << label.key() << '=' << label.value() << '\n';
}
```

Entries keep the order they arrived in. Each one is an `ex::Person::LabelsEntry` with `key()` and
`value()`. Structured bindings don't compile against an entry. Duplicate keys all stay in the map.
`find` walks the entries one by one and returns the last match, or `end()`. For many lookups in a
large map, build your own index once. When the values are messages, `value()` returns a pointer,
null for an entry sent without a value.

Oneofs have no accessor per member. You pass handlers to the oneof's own method, `workplace()` here,
which calls the one for the member that's set:

```cpp
person->workplace(
    [](ex::Person::Workplace::office, const ex::Address& office) {
        std::cout << "works in " << office.city() << '\n';
    },
    [](ex::Person::Workplace::timezone, std::string_view tz) {
        std::cout << "works remotely, " << tz << '\n';
    },
    [](std::monostate) { std::cout << "no workplace\n"; });
```

A handler's first parameter is a member tag type such as `ex::Person::Workplace::office`, and its
second is the member's value. For a message member that value comes as a reference, which can't be
null. Members without a handler are ignored unless a catch-all handler, such as
`[](auto, auto value) -> void { ... }`, picks them up. Leave out the `-> void` and a body that
prints the value or passes it to an overloaded function may not compile. The unset case reaches a
`std::monostate` handler, or a catch-all that ends in a `const auto&...` pack, but never a
two-parameter catch-all. Handlers return `void`.

A [decode profile](profiles.md) changes some of these types. Dropping a field removes its accessor.
A `raw` sub-message comes back still encoded: a `std::optional<rapidproto::ByteView>`, a plain
`rapidproto::ByteView` when the field is required, or a `StringArrayView` when it's repeated. The
decoder skips unknown fields and keeps nothing of them, but a profile can
[flag messages that had any](profiles.md#unknown-fields).

## The arena

```cpp
void serve(const std::vector<std::string>& requests) {
    rapidproto::Arena arena;
    arena.set_capacity_limit(64 << 20);  // 64 MiB
    for (const std::string& input : requests) {
        arena.reset();
        const ex::Person* person = ex::Person::decode(input, arena);
        if (person == nullptr) {
            continue;
        }
        std::cout << person->name() << '\n';
    }
}
```

An `Arena` takes memory from the heap a chunk at a time. Allocating inside a chunk only bumps a
pointer, and no chunk goes back to the heap before the arena is destroyed. `decode` allocates from
the arena and nowhere else. Since an arena can't be copied or moved, create it where you read the
messages, or use [`decode_owned`](#owned-messages) when that's awkward. Each thread needs its own
arena, though threads can share a decoded message for reading.

`reset()` lets the arena reuse its chunks from the start without freeing any. After the arena has
grown big enough for your largest message, the loop above stops allocating. A reset invalidates
message pointers and every `ArrayView`, `StringArrayView` or `MapView` taken from them. Copying a
message beforehand only goes so far. Messages copy like plain structs, but a copy shares the
original's sub-messages, repeated fields and maps, which all live in the arena. String views you
took out earlier point into the input and stay valid as long as it does. Reading through a stale
pointer still shows the old data until a later decode lands on the same memory.

The input can dangle too. A temporary `std::string` passed to `decode` compiles without a warning
and is destroyed at the end of the statement, leaving every string in the message pointing into
freed memory. On a `std::string`, `input.substr(n)` builds such a temporary, while slicing
`rapidproto::ByteView(input)` copies nothing.

To keep small messages off the heap, give the arena a buffer of your own:

```cpp
alignas(std::max_align_t) char buffer[16 * 1024];
rapidproto::Arena arena{buffer, sizeof buffer};
if (const ex::Person* person = ex::Person::decode(input, arena)) {
    std::cout << person->name() << " used " << arena.bytes_used() << " of "
              << arena.bytes_reserved() << " bytes\n";
}
```

The arena fills the buffer before it asks the heap for anything. Without the `alignas` it skips a
few bytes at the start to reach `std::max_align_t` alignment. Keep the buffer alive while any
message decoded into it is in use. `reset()` starts over at the buffer. `bytes_used()` counts the
memory handed to messages since the last reset, while `bytes_reserved()` counts everything the arena
holds, the buffer included.

## Limiting memory

An arena has no limit until you set one. With untrusted input, call `set_capacity_limit` before the
first decode, as `serve` does. The limit is compared with `bytes_reserved()`. When a decode would
need more, it stops with `OutOfMemory`. If the limit doesn't exceed the size of the arena's buffer,
the arena never touches the heap.

Messages can take much more memory than their input. In one test, 100,000 empty sub-messages, 2
bytes each on the wire, took 3.3 MB of arena. That's about 33 bytes apiece, 16 times the input.
Packed fields of varint-encoded types such as `int64`, `uint32` or an enum also peak while they
decode, because the arena reserves an element for each byte and trims the array afterwards. With
64-bit elements that's 8 bytes of arena per byte of the field. `double`, `fixed32` and the other
fixed-width types need no such slack.

To pick a limit, decode the largest message you accept and set the limit above what
`bytes_reserved()` reports. A known bug in reuse can currently grow the arena to several times that
over many decodes, so measure `bytes_reserved()` after a few thousand real messages instead.

## Owned messages

```cpp
std::shared_ptr<const ex::Person> load_person(std::string input) {
    auto person = rapidproto::decode_owned<ex::Person>(std::move(input));
    if (!person) {
        std::cerr << "not a valid Person\n";
    }
    return person;  // empty on failure
}
```

With `rapidproto::decode_owned`, the message comes back in a `std::shared_ptr` that also owns the
input and an arena of its own. Whoever holds a copy of that pointer keeps all three alive. Use it
when a message has to outlive the scope that decoded it. Move your input in, as the example does,
because the parameter is a `std::string` taken by value. An optional second argument takes an
`ArenaDecodeError*`. The private arena has no capacity limit. For untrusted input, use `decode`
with an arena you've limited.

A loop over a field read straight from the returned pointer, such as
`for (const auto& phone : rapidproto::decode_owned<ex::Person>(input)->phones())`, can use the
message after it's freed. The temporary pointer dies, taking the message with it, before the loop
body starts. Neither g++ nor clang warns. Bind the pointer to a variable first.

## Errors

```cpp
enum class Reply { Ok, TooLarge, BadRequest };

Reply handle(rapidproto::ByteView input, rapidproto::Arena& arena) {
    using Code = rapidproto::ArenaDecodeError::Code;
    rapidproto::ArenaDecodeError err;
    const ex::Person* person = ex::Person::decode(input, arena, &err);
    if (person == nullptr) {
        if (err.code == Code::OutOfMemory || err.code == Code::InputTooLarge) {
            return Reply::TooLarge;
        }
        return Reply::BadRequest;
    }
    std::cout << person->name() << '\n';
    return Reply::Ok;
}
```

`rapidproto::ArenaDecodeError` is a plain struct:

```cpp
struct ArenaDecodeError {
    enum class Code : std::uint8_t {
        None, Wire, OutOfMemory, RecursionTooDeep, MissingRequired,
        RepeatedSingularMessage, StringTooLong, InputTooLarge,
    };
    Code code = Code::None;
    WireError wire = WireError::None;
    std::size_t offset = 0;
    std::uint32_t field_number = 0;
    [[nodiscard]] constexpr bool ok() const noexcept { return code == Code::None; }
};
```

| `code` | Cause |
| --- | --- |
| `Wire` | the input breaks the wire format |
| `OutOfMemory` | capacity limit hit, or the heap allocation failed |
| `RecursionTooDeep` | messages nested too deep, see [depth limits](#depth-limits) |
| `MissingRequired` | a `required` field is absent |
| `RepeatedSingularMessage` | a non-repeated sub-message field occurs twice (protobuf merges) |
| `InputTooLarge` | `input.size()` over `UINT32_MAX` |
| `StringTooLong` | reserved, never reported |

For `Wire`, `wire` names the wire-format problem. `offset` is measured from where the innermost
message or packed field begins, and `err` doesn't say which one that was. `MissingRequired` and
`RepeatedSingularMessage` name the field in `field_number`.

Nothing touches `err` when a decode succeeds. If you reuse one, it can still hold an earlier failure
after a later decode succeeds. Test the returned pointer rather than `ok()`, and read `err` only
when the pointer is null. When a decode fails you get no partial message. The memory it had used
stays allocated in the arena until the next `reset()`. Messages decoded into the arena before it are
unaffected.

## Depth limits

Sub-messages can nest 100 deep under the top-level message (`rapidproto::kMaxDecodeDepth`). One
level more fails with `RecursionTooDeep`. Each group the schema declares adds a level as well. A
separate limit, `rapidproto::kMaxGroupDepth`, counts how deep groups sit directly inside one
another, unknown groups the decoder skips included. It is also 100 and is checked first. That's why
101 directly nested groups fail as `Wire` with `WireError::GroupTooDeep`, not `RecursionTooDeep`. On
Windows, a program that may see messages nested close to 100 levels needs a
[bigger stack](../README.md#compatibility).
