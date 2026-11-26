# The streaming decoder

Use the streaming decoder in place of the [arena decoder](arena.md) when you want a few fields out
of large messages and don't need the message itself. You pass `decode` a handler for each field
you're after, and it calls them in a single pass over the input without allocating memory. The whole
input has to be in memory before you start.

The examples decode a `Person`, from the same schema the arena decoder's page uses:

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

`rapidprotoc --stream person.proto` writes `person.rp.stream.hpp` (other flags are in the
[CLI reference](integration.md#the-rapidprotoc-cli)). The package gives the namespace,
`rp::stream::example` here, or plain `rp::stream` when the schema has none. A message declared
inside another becomes a struct nested in it.

```cpp
#include "person.rp.stream.hpp"

#include <iostream>

namespace ex = rp::stream::example;

bool print_person(rapidproto::ByteView input) {
    const rapidproto::DecodeStatus status = ex::Person{input}.decode(
        [](ex::Person::name, std::string_view name) { std::cout << name << '\n'; },
        [](ex::Person::email, std::string_view email) { std::cout << "email " << email << '\n'; },
        [](ex::Person::address, ex::Address address) {
            return address.decode([](ex::Address::city, std::string_view city) {
                std::cout << "lives in " << city << '\n';
            });
        });
    if (!status.ok()) {
        std::cerr << "not a valid Person\n";
        return false;
    }
    return true;
}
```

Given the bytes protoc writes for
`name: "Ada" id: 7 email: "ada@x.org" address { city: "London" } phones { number: "555-0100" }`,
it prints:

```text
Ada
email ada@x.org
lives in London
```

`ex::Person{input}` is the decoder, holding nothing but a view of the input. `rapidproto::ByteView`
is `std::string_view` under another name. For raw bytes behind a `const std::uint8_t*`,
`rapidproto::byte_view(ptr, size)` builds one.

Each handler picks its field by the type of its first parameter, so their order in the call doesn't
matter. With no handler for `id` or `phones`, the decoder skips both. It gets past `phones` in one
jump over its length, without looking inside.

## Handlers

For each field, the generator writes a field tag type nested in the message's decoder. This is
`ex::Person::name`:

```cpp
struct name {
    using Value = std::string_view;
    static constexpr std::uint32_t kNumber = 1;
    static constexpr std::string_view kName = "name";
};
```

A handler takes the field tag type and then a `Value` (for a map field, a `Key` and then a `Value`).
Any of them may be a `const` reference. Besides lambdas, any function object with a single
`operator()` works as a handler. Field tag types keep their fields' names, case included. A name
that would clash with C++ or with the generated code gets a trailing `_`, as in `decode_` for a
field called `decode`. `kNumber` holds the field number and `kName` the name as the schema spells
it. They're there mostly for [catch-all handlers](#catch-all-handlers).

Handlers are called in the order their fields appear in the input. Each element of a repeated field
gets its own call, whether the elements came packed or one per tag. A field that's missing from the
input doesn't reach its handler at all, not even with a default value. That includes a proto3 `id`
of 0, which protoc doesn't write. Give your variables their starting values before the decode.

## Field kinds

| Field | The handler then takes |
| --- | --- |
| number, `bool` | the value, such as `std::uint32_t` or `double` |
| enum | the generated enum type |
| `string`, `bytes` | `std::string_view` |
| message, group | the sub-message's decoder, such as `ex::Address` |
| `map<K, V>` | the key, then the value |

A repeated field or a oneof member takes the same parameters as a single field of its type. On
packed arrays of a thousand or more multi-byte numbers the arena decoder is
[faster](benchmarks.md#arena-or-streaming), because the streaming decoder makes a handler call for
every element.

```cpp
rapidproto::DecodeStatus print_labels(rapidproto::ByteView input) {
    return ex::Person{input}.decode(
        [](ex::Person::labels, std::string_view key, std::string_view value) {
            std::cout << key << '=' << value << '\n';
        });
}
```

The `labels` handler runs once per entry. An entry sent without its key or its value still reaches
it, with an empty view, a 0 or an empty decoder in place of the missing part.

The members of `Person`'s oneof, `office` and `timezone`, are ordinary fields here, each with its
own field tag type. The handler that runs tells you which one the sender chose. Should both turn up
in the input, both handlers run, in input order. Protobuf would keep the later one.

Two `address` fields in one input mean two calls to its handler, where protobuf would
[merge](semantics.md#fields-that-appear-more-than-once) them. Nothing checks for a missing
`required` field, an enum value the schema doesn't define, or a string that isn't valid UTF-8.

## Sub-messages

The `address` handler in the first example gets an `ex::Address`, the sub-message's decoder.
`Person`'s decoder doesn't look inside the address unless that handler calls `address.decode`.
Return the status of that call, as `print_person` does. A malformed address, or an abort from a
handler inside it, then ends the outer decode. If the handler ignores that status instead, a
malformed address goes unnoticed and the outer decode carries on.

Groups work differently. When the decoder meets a group, it first walks the group's fields until it
reaches the end-group tag, validating each tag and length as it goes. It does this before any
handler sees the group, and even when none will. Unlike `phones`, a group is never skipped in a
single jump. If more than `rapidproto::kMaxGroupDepth` (100) groups are nested directly inside one
another, the decode fails with `WireError::GroupTooDeep`.

There's no depth limit for sub-messages, unlike in the [arena decoder](arena.md#depth-limits). Each
level is one of your handlers calling `decode`, with the stack growing as it goes. A recursive
handler that may see untrusted input should count levels and stop the decode past a limit you
choose.

Each decoder has an `rp_bytes()` method that returns the bytes it would read. In the `address`
handler, `address.rp_bytes()` is the address as encoded, starting after the length prefix. A group's
decoder returns the group's body, without the tags that open and close it. You can pass these bytes
to the arena decoder's `decode` to [build a message](using-both-models.md) for just that part of the
input.

## Catch-all handlers

```cpp
rapidproto::DecodeStatus print_name(rapidproto::ByteView input) {
    return ex::Person{input}.decode(
        [](ex::Person::name, std::string_view name) { std::cout << name << '\n'; },
        [](auto field, const auto&...) -> void {
            std::cout << "skipped " << field.kName << " (field " << field.kNumber << ")\n";
        });
}
```

The second lambda is a catch-all handler, a generic lambda that accepts any field tag type and the
values that come with it. Every declared field you wrote no handler for goes to it, with the same
arguments its own handler would have had, while `name` still reaches the first lambda.

Write the values as a `const auto&...` pack, as here, which also gives map fields their key and
value. A catch-all with exactly two parameters, such as `[](auto field, auto value)`, compiles but
never receives map fields, whose handlers take three arguments. Written with `auto&&...`, the
catch-all takes over every field whose own handler takes its value by `const` reference, and a
`mutable` catch-all takes over even the fields of by-value handlers; the form shown avoids both. A
generic lambda with a single parameter receives only [unknown fields](#unknown-fields).

Write the `-> void` as the example does. Without it, `decode` compiles the body with test arguments
of its own while it sorts out which handler takes which field. A body that reads `kName`, or that
passes a value to `<<` or another overloaded function, doesn't compile against those arguments.

## Unknown fields

```cpp
rapidproto::DecodeStatus list_unknown(rapidproto::ByteView input) {
    return ex::Person{input}.decode([](rapidproto::UnknownField field) {
        std::cout << "field " << field.field_number << ", wire type "
                  << static_cast<int>(field.wire_type) << ", " << field.bytes.size()
                  << " bytes\n";
    });
}
```

A handler taking a single `rapidproto::UnknownField` receives every field whose number `Person`
doesn't declare, such as one added in a newer version of the schema. Extensions arrive here too.

`bytes` holds the field as it was encoded, minus its tag. The length that starts a length-delimited
field is included, and so is the tag that ends a group. Write the tag back in front, as the varint
`(field.field_number << 3) | static_cast<std::uint32_t>(field.wire_type)`, and you have the original
field byte for byte. When an unknown length-delimited field holds a message, the message starts
right after that length prefix.

## Errors and stopping early

```cpp
std::optional<std::string_view> first_email(rapidproto::ByteView input) {
    std::optional<std::string_view> found;
    const rapidproto::DecodeStatus status = ex::Person{input}.decode(
        [&](ex::Person::email, std::string_view email) {
            found = email;
            return rapidproto::DecodeStatus::abort();
        });
    if (status.wire != rapidproto::WireError::None) {
        std::cerr << "malformed Person\n";
        return std::nullopt;
    }
    return found;  // points into input
}
```

`decode` returns a `rapidproto::DecodeStatus`:

```cpp
struct [[nodiscard]] DecodeStatus {
    WireError wire = WireError::None;
    bool aborted = false;
    std::size_t offset = 0;

    [[nodiscard]] constexpr bool ok() const noexcept { return wire == WireError::None && !aborted; }
    explicit constexpr operator bool() const noexcept { return ok(); }

    static constexpr DecodeStatus success() noexcept { return {}; }
    static constexpr DecodeStatus abort() noexcept { return {WireError::None, true, 0}; }
};
```

A handler returns `void` or a `DecodeStatus`. Both kinds can go to the same `decode` call, as in
`print_person`. A handler that returns `DecodeStatus::success()` lets the decode carry on. One that
returns a status whose `ok()` is false, usually `DecodeStatus::abort()`, ends the decode on the
spot, and `decode` returns that same status. `first_email` stops at the first email this way. That's
also why it checks `wire` rather than `ok()`.

Malformed input ends the decode too, with `wire` set to what was wrong. `offset` gives the error's
position in bytes. It counts from the beginning of the input, or of the sub-message, packed field or
map entry the error is in.

Nothing a handler did before a failure is undone. Given only the first 20 bytes of the input above,
`print_person` prints `Ada` and `email ada@x.org` before it reports the error.

## What a decode checks

At the level you call `decode` on, the decoder checks every tag, value and length. It looks inside a
packed field or a map entry only when the field has a handler. Malformed bytes in anything it skips,
such as a sub-message no handler decoded, don't fail the decode. To have such bytes rejected, decode
those parts yourself. With no handlers, `return address.decode();` checks the address's own fields
without calling anything. Where the decoder does read, malformed input fails the decode and never
crashes it. The streaming decoder's
[fuzz tests](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md) run under AddressSanitizer
and UndefinedBehaviorSanitizer.

## Compile errors

Most handler mistakes stop the build. Taking `id` as a `std::int64_t`, which its `std::uint32_t`
would convert to, fails with "a callback for field 'Person::id' has the wrong value type (expected
id::Value)". An `int` for an enum fails the same way. The build also fails on two handlers for one
field, or on a handler that names a field of some other message, such as `ex::Address::city` passed
to `Person`'s `decode`. Once a handler names its field, its value parameter must be concrete too,
which rules out `[](ex::Person::id, auto)`. A handler for a field that has since been renamed or
removed from the schema fails with the compiler's own error about a missing type.

## Lifetimes and threads

Like the decoder itself, the `std::string_view`s your handlers receive
[borrow from the input](arena.md#the-arena). They remain usable after `decode` returns, up to the
point where the input is destroyed or changed. Say `load()` returns a `std::string`. In
`ex::Person{load()}.decode(...)` that string lives until the statement ends. The handlers can read
their views during the decode but mustn't keep any. `ex::Person person{load()};` stores a decoder
whose string is gone by the next line, and compilers don't warn about it.

`decode` keeps its own instance of each handler for the call, copied from a named lambda or moved
from a temporary. Capture by reference anything you want to read after the call. A `mutable` lambda
that changes a by-value capture changes only the instance `decode` holds. Several threads can decode
the same input at the same time, provided nothing modifies it.
