# Decode profiles

A decode profile is a short list of directives, usually kept in a text file, that adjusts the
[arena decoder](arena.md) generated for a schema. It's meant for programs that need only a handful
of fields from large messages. Such a program can drop the fields it never reads, leaving the
generated classes without storage or accessors for them. Sub-messages it seldom needs can stay
encoded until the program decodes them itself. Unknown-field tracking works the other way round. It
adds an accessor that tells you whether the input held fields the schema doesn't declare.

You give the profile to [`rapidprotoc`](integration.md#the-rapidprotoc-cli) along with the schema.
The schema stays untouched; another program can generate from it with a different profile. The
[streaming decoder](streaming.md) comes out the same with or without one.

Here's a profile for a program that prints a person's name, id and city and counts the phone
numbers. It's written for the [`Person` schema](arena.md) from the arena decoder's examples.

```text
# lean.modes: what print_person needs from a Person
name lean
drop example.Person.email
drop example.Person.labels
raw example.Person.address    # decoded only when present
raw example.Person.phones     # counted, never decoded
unknown-fields example.Person
```

Generate the header with `rapidprotoc --field-modes=lean.modes person.proto`. It keeps its usual
name, `person.rp.hpp`.

```cpp
#include "person.rp.hpp"

#include <iostream>
#include <optional>

namespace ex = rp::arena::example;

bool print_person(rapidproto::ByteView input) {
    rapidproto::Arena arena;
    const ex::Person* person = ex::Person::decode(input, arena);
    if (person == nullptr) {
        std::cerr << "not a valid Person\n";
        return false;
    }
    std::cout << person->name() << " (" << person->id() << ")\n";
    if (std::optional<rapidproto::ByteView> encoded = person->address()) {
        const ex::Address* address = ex::Address::decode(*encoded, arena);
        if (address == nullptr) {
            std::cerr << "not a valid Address\n";
            return false;
        }
        std::cout << "lives in " << address->city() << '\n';
    }
    std::cout << person->phones().size() << " phone number(s), still encoded\n";
    if (person->has_unknown_fields()) {
        std::cout << "has fields this schema doesn't declare\n";
    }
    return true;
}
```

`print_person` was given protoc's encoding of Ada's message,
`name: "Ada" id: 7 email: "ada@x.org" address { city: "London" } phones { number: "555-0100" }`,
and printed three lines.

```text
Ada (7)
lives in London
1 phone number(s), still encoded
```

Say the sender's schema has gained `string pronouns = 10;` and Ada's message sets it. Then the
function prints a fourth line, `has fields this schema doesn't declare`.

On Ada's message, `arena.bytes_used()` after `ex::Person::decode` is 120 bytes with the profile and
216 without. Decoding the address raises it to 144.

## Dropping fields

A dropped field has neither storage nor an accessor in the generated class, so code that still
reads it won't compile. Under `lean.modes`, clang++ rejects `person->email()` with:

```text
error: no member named 'email' in 'rp::arena::example::Person'
```

The decoder still reads the tag of a dropped field and makes sure the field doesn't run past the end
of the input. Malformed bytes inside a dropped sub-message, map or packed field go through, because
the decoder jumps over it without looking in. A group has no length to jump by, so the decoder reads
through a dropped one to find its end: a broken tag or length in there, or a missing end tag, still
fails the decode, but a sub-message inside the group isn't opened. Neither kind gets its
`required` fields checked. A non-repeated sub-message or group that arrives twice isn't reported as
[`RepeatedSingularMessage`](arena.md#errors) either.

## Raw sub-messages

`raw` works on message and group fields only. It keeps the sub-message encoded and hands you the
bytes:

| Field | Accessor returns |
| --- | --- |
| singular | `std::optional<rapidproto::ByteView>`, `std::nullopt` when absent |
| `required` | `rapidproto::ByteView` |
| repeated | `rapidproto::StringArrayView`, one encoded message per element |

To read a raw field, pass its bytes to the `decode` of the field's message type, as `print_person`
does with `ex::Address::decode`. Like strings, the bytes
[borrow from the input](arena.md#the-arena), and so does the message you decode from them.

The outer decode checks each raw field's length against the input. It still fails when a `required`
raw field is missing or a non-repeated one arrives twice. Anything wrong inside the bytes, from
malformed input to a missing `required` field, fails only when you decode them. Suppose Ada's
address holds only `0a 05`, a `city` tag and a length of 5 with no bytes after it.
`ex::Person::decode` accepts that message. The failure comes later, after `print_person` has
printed `Ada (7)` and called `ex::Address::decode`.

## Unknown fields

The arena decoder skips fields the schema doesn't declare. To learn whether a message had any,
track it with an `unknown-fields` line, as `lean.modes` does for `Person`, or with
`--unknown=example.Person`. `--unknown-present` tracks every message generated alongside it,
including nested messages and those from imported files. A tracked message gets one more
accessor, `has_unknown_fields()`. It returns true when the input held at least one field number the
message doesn't declare, and that's all it records. For the field numbers and their bytes, use the
streaming decoder's [unknown-field handler](streaming.md#unknown-fields).

[Extensions](semantics.md#unknown-fields) count as unknown fields. A field the schema declares
never does, even when the profile drops it or it arrives with an unexpected wire type and gets
skipped. Fields inside a sub-message are that sub-message's own business. If its type is tracked
too, it reports them itself (for a raw field, once you decode the payload).

Tracking costs one bit per message. In `Person` it shares a byte with the bit that records whether
`nickname` is set. `Address` has no such byte. Tracking it adds one, and padding rounds the class up
from 24 bytes to 28. The decoder only touches the bit when it meets an unknown field.

## Writing a profile

A profile holds one directive per line, each followed by a single argument: `drop` or `raw` with a
field or a type, `unknown-fields` with a message, or `name` with an identifier. Comments start with
`#` and may follow a directive on the same line. Write names in full, starting with the package, as
in `example.Person.email`.

Each `drop` and `raw` line in `lean.modes` names a single field. Such a line can name a type
instead. It then applies to every field of that type that the same `rapidprotoc` run generates (in
CMake, the same target), in nested messages and imported files too. For `drop` the type can be an
enum, and fields declared with that enum go. With a type `drop`, maps whose values have that type
are dropped too.

Some fields can't take a directive: oneof members, `required` fields under `drop`, and maps under
`raw`. Naming one in a field entry is an error, but a type entry passes over them without a word.
`raw example.Address`, for instance, turns `address()` into bytes and leaves `office`, the `Address`
in `Person`'s oneof, decoded as before. Where a field entry and a type entry both reach a field, the
field entry wins.

Pass the file with `--field-modes=<file>`, or single entries with `--drop=<name>` and
`--raw=<name>`. You can repeat these flags and combine them with the `--unknown` ones into one
profile. Those that take a value need the `=`. Written with a space, `--field-modes lean.modes`
stops `rapidprotoc` with `error: unknown flag '--field-modes'`. In CMake,
[`rapidproto_generate`](integration.md#cmake-integration) takes `FIELD_MODES`, `DROP`, `RAW`,
`UNKNOWN` and `UNKNOWN_PRESENT` for the same jobs. In Rust,
[`Config::field_modes`](https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.field_modes)
takes the profile's contents as a string instead of a path.

Each name has to match a field or type somewhere in the run, in the schemas you list or in the files
they import. To use one profile for several schemas, generate them in one run, or list them all
under one target's `PROTOS` in CMake. If nothing in the run matches a name, `rapidprotoc` stops
before writing any file. Here line 3 of `lean.modes` has lost its package and reads
`drop Person.email`:

```text
error: lean.modes:3: unknown field or type 'Person.email' (a silently ignored selection would be a footgun)
```

## Several profiles in one program

When a profile changes anything, `rapidprotoc` puts every arena class it generates into an inline
namespace inside the package namespace. The namespace's name is `rp_modes_` followed by the
profile's id. For `lean.modes` that's `rp_modes_lean_ad07e720`. Your code still writes
`ex::Person`.

Because of the inline namespace, don't forward-declare generated classes; include their header. A
declaration such as `namespace rp::arena::example { class Person; }` adds a second `Person` beside
the generated one. In a file that also includes the header, every use of `ex::Person` becomes
ambiguous.

The id's hex digits are a hash of the profile's entries, including those that turn on tracking.
Listing the entries in another order gives the same id. A `name` line puts the name in front of the
first 8 digits, as in `lean_ad07e720`. Without one, the id is all 16 digits. The same entries under
`name other` give `rp_modes_other_ad07e720` and a second, separate set of types. Two builds share
generated types only when their profiles agree on the name and on the entries.

Code built with different profiles usually fails to link where a generated type passes between
them. Take `void print_name(const ex::Person&)`, compiled with `lean.modes` and called from a file
built without a profile. The caller needs a `print_name` taking the unprofiled `Person`, which
doesn't exist:

```text
undefined reference to `print_name(rp::arena::example::Person const&)'
```

Some mix-ups get past the linker, because a function's mangled name includes neither its return
type nor the members of your own structs. Don't let generated classes travel between code built
with different profiles. Send the encoded message across instead and let the receiving side decode
it.

Because generated headers have the same names under every profile, each profile of a schema needs
its own output directory (in CMake, its own `rapidproto_generate` target). The
[consumer example](../examples/consumer/CMakeLists.txt) builds its schema twice like this, and
[`lean_main.cpp`](../examples/consumer/lean_main.cpp) uses the profiled copy. Don't mix the two
directories in one source file. Each output directory carries its own copy of the runtime headers,
and a file that includes generated headers from two of them can fail to compile with
`rapidproto::Arena` defined twice, even when neither directory uses a profile.
