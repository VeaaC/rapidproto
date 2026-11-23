# The debug dumper

The debug dumper prints a message from the [arena decoder](arena.md) as JSON-like text, to look at
while debugging or to put in a log line. Pass `--dump` to
[`rapidprotoc`](integration.md#the-rapidprotoc-cli) and it writes one more header,
`<stem>.rp.dump.hpp`, which provides `rapidproto::dump`. The dumper reads each field with the
generated accessor, as your own code would. Adding `--dump` leaves the arena decoder's header
unchanged.

```sh
rapidprotoc --dump -I. --out-dir=out person.proto
```

`--dump` implies `--arena`, so on top of the usual arena output this writes `person.rp.dump.hpp`
and `rapidproto/dump_runtime.hpp`, the dumper's part of the [runtime](integration.md). The dump
header includes `person.rp.hpp` and is the only header you need. In CMake, add `DUMP` to
[`rapidproto_generate`](integration.md#cmake-integration). In Rust, `{:?}` on a decoded message
gives the same kind of view through its
[`Debug`](https://docs.rs/rapidproto/latest/rapidproto/#debug-output) implementation.

```cpp
#include "person.rp.dump.hpp"

#include <iostream>

namespace ex = rp::arena::example;

bool dump_person(rapidproto::ByteView input) {
    rapidproto::Arena arena;
    const ex::Person* person = ex::Person::decode(input, arena);
    if (person == nullptr) {
        std::cerr << "not a valid Person\n";
        return false;
    }
    std::cout << rapidproto::dump(*person) << '\n';
    return true;
}
```

For Ada's message from the arena decoder's page, `dump_person` prints a single line:

```text
{"name": "Ada", "id": 7, "email": ["ada@x.org"], "address": {"city": "London"}, "phones": [{"number": "555-0100"}]}
```

`rapidproto::dump` returns the text as a `std::string` without a final newline. It takes the
message itself, `*person` here. Hand it the pointer, a type from the
[streaming decoder](streaming.md), or a message whose `.rp.dump.hpp` you haven't included, and the
build stops at a `static_assert` that begins `rapidproto::dump: no generated dumper for this type`.

## What it prints

For a `Person` with more fields set, the same function prints:

```text
{
  "name": "Ada",
  "id": 7,
  "email": ["ada@x.org", "ada.lovelace@example.com"],
  "address": {"city": "London", "country": "UK"},
  "nickname": "",
  "phones": [{"number": "555-0100"}, {"number": "555-0199"}],
  "labels": {"team": "compilers", "floor": "3"},
  "timezone": "Europe/London"
}
```

| Field | Printed as |
| --- | --- |
| integer | a number |
| `bool` | `true` or `false` |
| `float`, `double` | a number that reads back unchanged, or `"NaN"`, `"Infinity"`, `"-Infinity"` |
| `string` | in quotes, escaping `"`, `\` and any byte under 0x20 |
| `bytes` | quoted hex digits, lowercase, such as `"0102ff"` |
| enum | its C++ [name](semantics.md#value-names) in quotes, or `"UNKNOWN(7)"` for an unlisted 7 |
| message, group | an object, `{}` when empty |
| repeated field | an array, elements in the order received |
| map | an object, every key in quotes |
| oneof | the member that's set, under the member's name |

Which fields appear depends on [presence](arena.md#what-each-field-kind-returns). A field without
explicit presence prints only when it isn't 0, empty or `false`. One with explicit presence shows
up whenever the input set it, as `"nickname": ""` above does. `required` fields are always there.
A sub-message appears whenever the input has it, even an empty one, and a repeated field or map
once it has an element. Fields come in the order the schema declares them, except that maps follow
the other fields. The set members of oneofs come last, in the order the oneofs are declared.

The text is JSON as long as every string in the message is valid UTF-8. The decoder doesn't
[check that](semantics.md#strings-and-utf-8). The dumper copies a string's bytes through apart from
the escapes, so one bad string makes the whole dump invalid UTF-8. It isn't protobuf's JSON format
either. Keys are the field names as the schema spells them, `bytes` are hex rather than base64,
64-bit integers aren't quoted, and a map key sent in two entries
[shows up twice](semantics.md#fields-that-appear-more-than-once). A
[well-known type](semantics.md#well-known-types) prints like any other message. A `Timestamp`
shows `seconds` and `nanos` where protobuf's JSON writes a date string, and an `Any` shows
`type_url` and its packed `value` in hex.

A [decode profile](profiles.md) changes what there is to print. Dropped fields never appear. The
hex string a `raw` field prints is its payload, still encoded. A message with
[unknown-field tracking](profiles.md#unknown-fields) gets a `"has_unknown_fields": true` entry
first when its input had fields the schema doesn't declare, and no such entry when it had none.
What those fields contained isn't printed in either case. With the header generated from the
profiles page's `lean.modes`, `dump_person` prints this for Ada's message plus an undeclared
field 10:

```text
{"has_unknown_fields": true, "name": "Ada", "id": 7, "address": "0a064c6f6e646f6e", "phones": ["0a083535352d30313030"]}
```

## Indentation and line width

```cpp
void print_request(const ex::Person& person) {
    rapidproto::DumpOptions opts;
    opts.indent = 1;  // the dump sits one level inside the object around it
    std::cout << "{\n  \"request_id\": 42,\n  \"person\": ";
    rapidproto::dump(std::cout, person, opts);
    std::cout << "\n}\n";
}
```

With the fuller `Person` from above, this prints:

```text
{
  "request_id": 42,
  "person": {
    "name": "Ada",
    "id": 7,
    "email": ["ada@x.org", "ada.lovelace@example.com"],
    "address": {"city": "London", "country": "UK"},
    "nickname": "",
    "phones": [{"number": "555-0100"}, {"number": "555-0199"}],
    "labels": {"team": "compilers", "floor": "3"},
    "timezone": "Europe/London"
  }
}
```

`indent` tells the dumper how deep it starts, at two spaces per level. The first brace goes
wherever the stream is. The lines after it are indented to match, the closing brace included.
Whatever you printed before the dump on that first line, like `"person":` here, doesn't count
toward the width. `print_request` uses the overload that writes to a stream instead of returning a
string. The stream's locale and flags such as `std::hex` have no effect on the text and are still
set afterwards.

The dumper puts an object or array on one line when it fits within `DumpOptions::width`, which is
120 columns unless you change it. The indentation counts toward that. One that doesn't fit gets a
line per entry, indented two more spaces, and each of those entries gets the same test. Before an
array goes to one element per line, it tries rows of aligned columns, as many as fit. A width of 0
breaks every object and array that isn't empty. A long string stays on its line even when it runs
past the width. Where only the width matters, pass an integer in place of the options, as in
`rapidproto::dump(*person, 60)`.

## Leaving fields out

```cpp
void print_short(const ex::Person& person) {
    rapidproto::DumpOptions opts;
    opts.width = 40;
    opts.skip = {"email", "address.country", "phones.number"};
    std::cout << rapidproto::dump(person, opts) << '\n';
}
```

For the fuller `Person`, `print_short` prints:

```text
{
  "name": "Ada",
  "id": 7,
  "address": {"city": "London"},
  "nickname": "",
  "phones": [{}, {}],
  "labels": {
    "team": "compilers",
    "floor": "3"
  },
  "timezone": "Europe/London"
}
```

Each entry in `skip` names a field by its path from the message you dump, with dots between the
names. A bare name such as `"email"` matches a top-level field only. Skipping `"address"` would drop
the whole address, and with `"phones.number"` every phone loses its `number`. A oneof member is
named directly, as `"timezone"`. The oneof's name, `"workplace"`, matches nothing, and neither does
a map key such as `"labels.team"`. Like a misspelled path, they're ignored without an error. The
paths are `std::string_view`s that must stay valid until `dump` returns, which string literals like
these always do.
