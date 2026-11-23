# Decoding rules

RapidProto's [arena decoder](arena.md) and [streaming decoder](streaming.md) follow fixed rules when
the input doesn't line up with the schema. A field may be missing or arrive twice, a field number or
an enum value may be one the schema doesn't declare, and the bytes may break the wire format. C++
and Rust behave the same except where a section says otherwise. In several places the result differs
from what Google's own library gives, which matters most for code ported from it.

The examples use the schema of the Rust crate's
[`example`](https://docs.rs/rapidproto/latest/rapidproto/example/index.html) module:

```proto
syntax = "proto3";

package weather;

enum Unit {
  UNIT_UNSPECIFIED = 0;
  UNIT_CELSIUS = 1;
  UNIT_FAHRENHEIT = 2;
}

message Location {
  double latitude = 1;
  double longitude = 2;
}

message Station {
  string name = 1;
  Location location = 2;
  Unit unit = 3;
  repeated float temperatures = 4;
  map<string, string> labels = 5;
  oneof status {
    string error = 6;
    uint64 uptime_seconds = 7;
  }
  optional string note = 8;
}

message Report {
  repeated Station stations = 1;
}
```

## Malformed input

A decode fails on bytes that break the wire format, for example a tag with field number 0, a varint
longer than ten bytes or a length that runs past the end of the input. The arena decoder reads the
whole input and fails on malformed bytes anywhere in it, in sub-messages, packed fields and map
entries as well. It only steps over the contents of fields that a
[decode profile](profiles.md#dropping-fields) drops or keeps `raw`, though. Malformed bytes in those
can go unnoticed.

In both languages, the streaming decoder leaves some bytes unchecked: those inside a packed field or
map entry without a handler and those inside a sub-message that no handler decodes. It checks
everything else as it reads, every group included
([what a decode checks](streaming.md#what-a-decode-checks)).

An arena decode fails once sub-messages and groups nest more than 100 levels deep, a limit named
[`kMaxDecodeDepth`](arena.md#depth-limits) in C++ and
[`MAX_DECODE_DEPTH`](https://docs.rs/rapidproto/latest/rapidproto/constant.MAX_DECODE_DEPTH.html) in
Rust. Groups nested directly inside groups stop at 100 in the streaming decoder too. Its
sub-messages have none. A handler that recurses through untrusted input has to count the levels
itself ([C++](streaming.md#sub-messages),
[Rust](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#untrusted-input)).

With enough stack for the depth limit, malformed input never crashes a decode. On
Windows a C++ program needs more than the default 1 MB for that. In an unoptimized Rust build, a
spawned thread's 2 MiB may not be enough. The C++ decoders and the Rust arena decoders are
[fuzzed](https://github.com/VeaaC/rapidproto/blob/main/SECURITY.md) in CI; the Rust streaming
decoder isn't.

A decode checks the wire format and not the values it carries. An `int32` or `uint32` field sent a
varint that doesn't fit keeps the low 32 bits, so an `int32` sent 2^32 + 5 reads 5. A `bool` sent 2
reads true. Both read the same way in protobuf.

## Strings and UTF-8

Since values go unchecked, a `string` field can hold bytes that aren't valid UTF-8. C++ hands you a
`std::string_view` to check yourself. Rust gives you a
[`ProtoStr`](https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoStr.html), whose
[`to_str`](https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoStr.html#method.to_str) checks
them when you call it and returns a `Result<&str, Utf8Error>`. A proto3 `string` field that isn't
valid UTF-8 makes protobuf's parser fail.

## Absent fields

A field the input leaves out reads as 0 or empty, unless it has explicit presence, like `note`, or
is a message, like `location`. Those tell you they're absent:

```cpp
#include "weather.rp.hpp"

#include <iostream>
#include <string>

namespace w = rp::arena::weather;

void print_station(const w::Station& station) {
    std::cout << "name '" << station.name() << "'\n";                   // "" when absent
    std::cout << "note " << station.note().value_or("(none)") << '\n';  // std::nullopt when absent
    if (const w::Location* location = station.location()) {           // nullptr when absent
        std::cout << "at " << location->latitude() << '\n';
    }
}
```

```rust
use example::arena::weather::Station;
use example::common::weather::Unit;
use rapidproto::{Arena, DecodeError};

fn empty_station() -> Result<(), DecodeError> {
    let arena = Arena::new();
    let station = Station::decode(b"", &arena)?; // every field absent
    assert_eq!(station.name(), "");
    assert_eq!(station.unit(), Unit::UNSPECIFIED);
    assert_eq!(station.note(), None);
    assert!(station.location().is_none());
    Ok(())
}
```

For a `Station` decoded from an empty input, `print_station` prints `name ''` and `note (none)`.

`name` and `unit` have implicit presence. Absent, they read as 0, as empty, or as the enum value 0,
here `UNSPECIFIED`. A field sent with that value reads the same. In proto3, every singular field
that isn't a message, isn't in a oneof and isn't marked `optional` has implicit presence. In
editions, so does any field whose `field_presence` resolves to `IMPLICIT`.

`note` has explicit presence because it's marked `optional`. Its accessor returns a
`std::optional<std::string_view>` in C++ and an `Option<ProtoStr>` in Rust. The accessor tables for
[C++](arena.md#what-each-field-kind-returns) and
[Rust](https://docs.rs/rapidproto/latest/rapidproto/example/index.html#accessors) cover every kind
of field.

A message field's accessor returns a pointer in C++ and an `Option` such as `Option<&Location>` in
Rust, null or `None` when the field is absent. Neither language generates a `has_` method for a
field.

A `[default = X]` option in a proto2 or editions schema isn't applied. When the field is absent it
reads `std::nullopt` or `None`, where protobuf's accessor would return X. `value_or` in C++ and
`unwrap_or` in Rust put it back; for a Rust string field the fallback is a
`ProtoStr::new(b"...")`.

The streaming decoder calls no handler for a field that isn't in the input. A map entry that lacks
its key or its value still reaches the map handler, with 0, an empty view or an empty sub-message in
the missing place. The arena decoder fills the same gaps, except that a missing message value reads
as null in C++ and as an empty message in Rust.

## Required fields

An arena decode fails when the input lacks a `required` field, or an editions field whose
`field_presence` is `LEGACY_REQUIRED`. The error is
[`ArenaDecodeError::Code::MissingRequired`](arena.md#errors) in C++ and
[`ErrorKind::MissingRequired`](https://docs.rs/rapidproto/latest/rapidproto/enum.ErrorKind.html#variant.MissingRequired)
in Rust. The field's number is in `field_number` in C++ and comes from
[`DecodeError::field_number`](https://docs.rs/rapidproto/latest/rapidproto/struct.DecodeError.html#method.field_number)
as `Some(n)` in Rust. It's the number within the field's own message, and the error doesn't name
that message. When several fields are missing, the error reports the one declared first. Each
message is checked when the decoder reaches its end, including sub-messages but not the inside of a
field that a profile drops or keeps `raw`.

The streaming decoder doesn't check required fields at all; even an empty input decodes without
error. In libprotobuf for C++, `ParseFromString` rejects such input and `ParsePartialFromString`
accepts it.

## Enums

Every enum is open. An enum field holds whatever number arrived, including numbers the schema
doesn't list, whether the field is singular, repeated or packed. A `switch` or `match` on an enum
needs an arm for the numbers it doesn't know:

```cpp
const char* unit_name(w::Unit unit) {
    switch (unit) {
        case w::Unit::UNSPECIFIED: return "unspecified";
        case w::Unit::CELSIUS: return "celsius";
        case w::Unit::FAHRENHEIT: return "fahrenheit";
        default: return "a unit this schema doesn't list";
    }
}
```

```rust
use example::common::weather::{Unit, UnitClass};

fn unit_name(unit: Unit) -> &'static str {
    match unit.classify() {
        UnitClass::Unspecified => "unspecified",
        UnitClass::Celsius => "celsius",
        UnitClass::Fahrenheit => "fahrenheit",
        UnitClass::Unknown(_) => "a unit this schema doesn't list",
    }
}
```

For a `Station` whose input sets `unit` to 7, both functions return the last string.

The C++ enum is an `enum class` over `std::int32_t`, one type that
[both decoders share](using-both-models.md). Without the `default:`, `-Wall` warns that
`rp_non_exhaustive_min` and `rp_non_exhaustive_max` aren't handled. The generator adds those two
enumerators at `INT32_MIN` and `INT32_MAX` so that a `switch` over the schema's values alone draws
the warning. `rp_known_min` and `rp_known_max` are the lowest and highest numbers the schema lists,
useful for a range check, though a number between them may still be undeclared.

In Rust,
[`Unit`](https://docs.rs/rapidproto/latest/rapidproto/example/common/weather/struct.Unit.html) is a
struct around an `i32`, kept in the `common` module that both decoders use. Each value is a constant
such as `Unit::CELSIUS`, with the number in `.0`. `classify()` turns it into
[`UnitClass`](https://docs.rs/rapidproto/latest/rapidproto/example/common/weather/enum.UnitClass.html),
whose variants mirror the schema's values plus `Unknown(i32)` for every number it doesn't list.

Closed enums, meaning proto2 enums and editions enums with `enum_type = CLOSED`, decode the same way
and can hold any number. In protobuf, a number a closed enum doesn't list goes to the message's
unknown fields instead. A singular field then keeps the value it had, while a repeated field doesn't
get the element. Code ported from protobuf may never have needed the `default:` or `Unknown` arm for
such a number.

### Value names

`Unit::CELSIUS` is `UNIT_CELSIUS` without the enum's name in front. The generator shortens the names
only when every value of the enum starts with that prefix and no shortened name would begin with a
digit. C++ also leaves them whole when a short name would clash with a keyword, a macro such as
`EOF`, or a name the generator reserves. Rust shortens those anyway and escapes the result where it
has to, so a schema with `STATUS_OK` and `STATUS_EOF` gives `Status::EOF` in Rust but
`Status::STATUS_EOF` in C++.

A new value can change the names of the old ones. Append `KELVIN = 3` to `Unit` and every value gets
its full name back, such as `Unit::UNIT_CELSIUS` in both languages and `UnitClass::UnitCelsius` from
Rust's `classify()`. The numbers in the input stay the same, but code that names the old constants
stops compiling.

## Fields that appear more than once

A field can arrive twice when someone concatenates two encoded messages, which
[protobuf decodes](https://protobuf.dev/programming-guides/encoding/) as one merged message. What
RapidProto's decoders do depends on the kind of field:

| Sent twice | Arena decoder | Streaming decoder | protobuf |
| --- | --- | --- | --- |
| singular number, enum, `string`, `bytes` | last one | a call each time | last one |
| repeated field | all, in input order | a call per element | all |
| oneof, a different member | last member | a call per member | last member |
| map key, in two entries | both entries | a call per entry | later entry |
| map key or scalar value, in one entry | last of each | one call, last of each | last of each |
| singular message or group | fails | a call each time | merged |
| oneof, the same message member twice in a row | fails | a call each time | merged |
| map message value, in one entry | fails | one call, last value | merged |

A `string` sent as `"AAA"` and then as `"BBB"` reads `"BBB"`. A repeated field takes its elements
whether they come packed, one per tag or mixed, whatever packing the schema declares.

In a map where one key appears in two entries, C++'s `MapView::size()` counts both, a loop visits
them in input order, and `find` returns the later one. Rust's
[`MapView`](https://docs.rs/rapidproto/latest/rapidproto/struct.MapView.html) does the same through
`len()`, `iter()` and `get()`.

For the cases that fail, the arena decoder's error is `RepeatedSingularMessage`. Concatenating two
encoded messages that each set `location` is enough:

```cpp
bool decode_both(const std::string& a, const std::string& b) {
    const std::string input = a + b;  // a and b each hold a Station with a location
    rapidproto::Arena arena;
    rapidproto::ArenaDecodeError err;
    const w::Station* station = w::Station::decode(input, arena, &err);
    if (station == nullptr) {
        if (err.code == rapidproto::ArenaDecodeError::Code::RepeatedSingularMessage) {
            std::cerr << "field " << err.field_number << " appears twice\n";  // field 2
        }
        return false;
    }
    print_station(*station);
    return true;
}
```

In Rust the error's `kind()` is `ErrorKind::RepeatedSingularMessage` and its `field_number()` is
`Some(2)`. For a map value it's the map field's number. A repeat deep inside a sub-message fails the
whole decode, with no partial message. As with `MissingRequired`, the number is the field's number
within its own message. The rule also covers a field that a profile keeps `raw`, but not one it
drops.

The arena decoder rejects such input. If you produce it, merge the messages before
encoding them. If you only receive it, the streaming decoder can still read it, calling your handler
for every occurrence.

## Unknown fields

An unknown field carries a number that the message's schema has no field for, as happens when the
sender uses a newer version of the schema. The arena decoder passes over unknown fields. A decoded
message can't give them back, but a decode profile can [record](profiles.md#unknown-fields) whether
it had any, through an added `has_unknown_fields()`. To turn that on for every message, pass
`--unknown-present` to [`rapidprotoc`](integration.md#the-rapidprotoc-cli) or call
[`Config::unknown_present`](https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.unknown_present)
in Rust.

With an unknown-field handler, the streaming decoder passes every unknown field to it. Without one,
it skips them silently. In C++ that handler takes a
[`rapidproto::UnknownField`](streaming.md#unknown-fields). In Rust it's an `unknown => |field| ...`
entry in
[`station_stream!`](https://docs.rs/rapidproto/latest/rapidproto/example/stream/weather/macro.station_stream.html),
where `field` is a
[`rapidproto::stream::UnknownField`](https://docs.rs/rapidproto/latest/rapidproto/stream/struct.UnknownField.html).

Sometimes a field the schema declares comes with a wire type it can't have, such as `name` sent as a
varint. Every decoder skips it, but not as an unknown field: `has_unknown_fields()` stays false and
no unknown-field handler is called. In protobuf it lands among the message's unknown fields and is
written back out when the message is encoded again.

Extensions aren't decoded. A field from a message's `extensions` range arrives as an unknown field,
even where protobuf would decode it through a registered extension. Since a MessageSet
(`option message_set_wire_format = true`) holds nothing but extensions, all of it decodes as unknown
fields. `rapidprotoc` prints a warning for each MessageSet it generates.

## Well-known types

The well-known types, such as `google.protobuf.Timestamp`, are ordinary messages to the decoders. A
`Timestamp` field gives you a message with `seconds()` and `nanos()` accessors. Nothing converts it
to a time type or unpacks an `Any`.
