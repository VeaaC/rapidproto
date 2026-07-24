# Using both models

*One schema, both decoders — in one translation unit, even mid-decode. Back to the
[README](../README.md).*

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
hybrid) is in [`examples/consumer`](../examples/consumer).

## Coexisting with protoc

By default a proto `package a.b` maps to C++ `namespace a::b`, the same as protoc, so you can't include
both protoc's `.pb.h` and a RapidProto header for the same message in one TU (they'd define `a::b::Msg`
twice). If you need both (protoc for serialization, RapidProto for fast decoding), nest the generated
code under a prefix with `--namespace-prefix`:

```sh
rapidprotoc --namespace-prefix=rp -I. --out-dir=out person.proto
# -> namespace rp::example { class Person … }
```

Now `rp::example::Person` (RapidProto) and `example::Person` (protoc) coexist.
