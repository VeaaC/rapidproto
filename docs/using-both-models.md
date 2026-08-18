# Using both models

*One schema, both decoders — in one translation unit, even mid-decode. Back to the
[README](../README.md).*

Each model has its **own root namespace**: arena at `rp::arena::<pkg>::Msg`, streaming at
`rp::stream::<pkg>::Msg`, with the schema's enums as a single shared type at `rp::common::<pkg>`,
aliased into both. Nothing the generator invents lands inside your package, so any schema coexists.
Generate both (`--arena --stream`, or `GENERATOR both` in CMake) and use each where it fits:

```cpp
#include "person.rp.hpp"         // arena:     rp::arena::example::Person
#include "person.rp.stream.hpp"  // streaming: rp::stream::example::Person  (both pull in the shared enums)

namespace ex  = rp::arena::example;   // alias each model once and the code below stays short
namespace ex_s = rp::stream::example;

const ex::Person* tree = ex::Person::decode(bytes, arena);   // materialize when you need an object
const auto st = ex_s::Person{bytes}.decode( /* … */ );        // or stream when you don't
if (!st.ok()) { /* malformed input */ }                       // decode() is [[nodiscard]]
// ex::Status and ex_s::Status are the same enum type.
```

The models also combine **mid-decode**: stream a large outer message and materialize just the
sub-messages you keep. A streaming sub-decoder's `rp_bytes()` is exactly the sub-message's field
bytes, which the arena `decode()` accepts directly. The materialized tree borrows those bytes (a slice
of the input `wire`), so `wire` must outlive every tree you keep:

```cpp
rapidproto::Arena arena;
ex_s::Person{wire}.decode(
    [&](ex_s::Person::address, ex_s::Address a) {
        const ex::Address* tree = ex::Address::decode(a.rp_bytes(), arena);
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

Nothing to do: generated code lives under `rp::`, protoc's under your package, so `person.pb.h` and
`person.rp.hpp` coexist in one TU — including for schemas importing a well-known type, where protoc
also defines `google::protobuf::Timestamp`. Use protoc for serialization and RapidProto for the hot
decode path. If your codebase already declares a *type* at `rp::arena` (a plain `namespace rp` is fine —
namespaces merge), rename the root with [`--namespace-prefix`](integration.md).
