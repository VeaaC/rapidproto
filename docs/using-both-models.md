# Using both models

You can generate the [arena decoder](arena.md) and the [streaming decoder](streaming.md) for one
schema and use both in the same program, even in the same source file. Inside a streaming handler,
the arena decoder can take over one sub-message and return a full message with accessors for that
part of a large input, while the rest is only streamed. The generated code can also share a source
file with the classes protoc generates for the schema.

Generate both with a single `rapidprotoc` run:

```sh
rapidprotoc --arena --stream -I. --out-dir=out person.proto
```

For the `Person` schema on the arena decoder's page, this writes `person.rp.hpp`,
`person.rp.stream.hpp` and `person.rp.common.hpp` into `out/`, with one copy of the
[runtime](integration.md) in `out/rapidproto/`. In CMake, one
[`rapidproto_generate`](integration.md#cmake-integration) target with `GENERATOR both` does the
same, as in the [consumer example](../examples/consumer). Keep both headers in that one output
directory. Two targets, one with `GENERATOR arena` and one with `GENERATOR stream`, would write two
directories, each holding a runtime of its own. A source file that includes headers from both then
has two copies of the runtime. Under clang++ the build stops at `redefinition of 'WireType'`. With
g++ it compiles only while the two copies have the same contents and timestamp.

The arena decoder's `Person` is `rp::arena::example::Person`, and the streaming decoder's is
`rp::stream::example::Person`. Each enum in the schema is defined once, under
`rp::common::example`. Both decoders' headers only name that definition, whether the enum is
top-level or nested in a message. An enum value from an arena accessor and one a streaming
handler receives have the same C++ type, and a function written for one takes the other. These
definitions live in `<stem>.rp.common.hpp`, the common header. You don't include it yourself; both
decoder headers do.

## Passing a sub-message to the arena decoder

```cpp
#include "person.rp.hpp"
#include "person.rp.stream.hpp"

#include <iostream>

namespace ex = rp::arena::example;
namespace exs = rp::stream::example;

bool print_city(rapidproto::ByteView input) {
    rapidproto::Arena arena;
    const ex::Address* address = nullptr;
    const rapidproto::DecodeStatus status = exs::Person{input}.decode(
        [&](exs::Person::address, exs::Address encoded) {
            address = ex::Address::decode(encoded.rp_bytes(), arena);
            return address != nullptr ? rapidproto::DecodeStatus::success()
                                      : rapidproto::DecodeStatus::abort();
        });
    if (!status.ok()) {
        std::cerr << "not a valid Person\n";
        return false;
    }
    if (address != nullptr) {
        std::cout << "lives in " << address->city() << '\n';
    }
    return true;
}
```

Given protoc's encoding of the arena page's example input, `print_city` prints `lives in London`.

The `address` handler receives `exs::Address`, the sub-message's decoder. Its
[`rp_bytes()`](streaming.md#sub-messages) returns the address as encoded, which
`ex::Address::decode` takes like any other input. The address it returns still borrows from the
input, so keep `input` and `arena` alive while you use it. In Rust, the sub-message's decoder hands
over the same bytes through
[`bytes()`](https://docs.rs/rapidproto/latest/rapidproto/example/stream/weather/struct.Station.html#method.bytes).

All the streaming decoder checks about the address is that it fits in the input
([what a decode checks](streaming.md#what-a-decode-checks)). Everything inside it is left to
`ex::Address::decode`. A failure there can end the outer decode only through what the handler
returns. `print_city`'s handler returns
[`DecodeStatus::abort()`](streaming.md#errors-and-stopping-early) for a null address, which makes
the outer decode fail. A handler that left out the check would let `decode` finish with `ok()`
true, and `print_city` would return true for a broken address without printing anything.

## Resetting the arena between sub-messages

```cpp
bool print_phones(rapidproto::ByteView input) {
    rapidproto::Arena arena;
    const rapidproto::DecodeStatus status = exs::Person{input}.decode(
        [&](exs::Person::phones, exs::Phone encoded) {
            arena.reset();
            const ex::Phone* phone = ex::Phone::decode(encoded.rp_bytes(), arena);
            if (phone == nullptr) {
                return rapidproto::DecodeStatus::abort();
            }
            std::cout << "phone " << phone->number() << '\n';
            return rapidproto::DecodeStatus::success();
        });
    if (!status.ok()) {
        std::cerr << "not a valid Person\n";
        return false;
    }
    return true;
}
```

For a `Person` with three phones, `print_phones` prints:

```text
phone 555-0100
phone 555-0199
phone 555-0123
```

Without the `reset()`, every phone would stay in the arena until `print_phones` returns. The handler
can reset the arena while the outer decode is still running, because nothing in the streaming
decoder touches the arena. Each [reset](arena.md#the-arena) invalidates every message the arena
holds, the phone from the previous call included. Finish with each phone inside the call that
decoded it. With a reset per phone, the arena only has to grow as far as the largest phone needs.
For now, a [bug in how a reset arena reuses its memory](arena.md#limiting-memory) can make it grow
past that.

## Next to protoc-generated code

The `person.pb.h` that protoc writes can go in the same source file as `person.rp.hpp` and
`person.rp.stream.hpp` without any extra flag, even for a schema that imports a well-known type
such as `google.protobuf.Timestamp`. Everything RapidProto generates sits under `rp`, and the
runtime under `rapidproto`, away from protoc's classes in `example`. The two sides share no types,
enums included. Messages pass between them as encoded bytes and enum values as plain numbers.

The combination breaks where protoc's header declares a name at a spot RapidProto needs for a
namespace: `rp` at global scope, `arena`, `stream` or `common` inside it, or the runtime's
`rapidproto`. In package `rp`, that happens when a top-level message, enum, enum value or extension
is called `arena`, `stream` or `common`. In a schema with no package, the names to avoid are `rp`
and `rapidproto`, and a schema in package `rapidproto` can collide with runtime types such as
`Arena`. Your own `namespace rp { struct arena {}; }` breaks it the same way. For a
message `stream` in package `rp`, clang++ reports:

```text
error: redefinition of 'stream' as different kind of symbol
```

[`--namespace-prefix`](integration.md#the-rapidprotoc-cli) fixes the `rp` cases by putting `arena`,
`stream` and `common` under a root namespace you name (`NAMESPACE_PREFIX` in CMake). With
`--namespace-prefix=myrp`, the arena decoder's `Person` is `myrp::arena::example::Person`, so
aliases such as `ex` and `exs` have to name the new namespaces. The runtime stays in `rapidproto`
whatever the prefix, so a clash with that name goes away only by renaming in the schema.
