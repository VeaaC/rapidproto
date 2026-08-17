# Decode profiles & unknown fields

*Arena-only features that tailor decoding to what **this** consumer needs, without touching the
schema. Back to the [README](../README.md); the arena model itself is in [arena.md](arena.md).*

## Unknown fields

By default, fields not in your schema (a newer producer's field, or a proto2 extension) are **skipped
and dropped**. To *detect* (not recover) that unknowns were present, reserve a per-message "saw an
unknown field" flag, exposed as `has_unknown_fields()`:

- `--unknown-present` reserves it on **every** message.
- `--unknown=<pkg.Msg>` (repeatable), or an `unknown-fields <pkg.Msg>` line in a decode profile,
  reserves it on **one** message — so you pay the bit only where you check it.

The selection is part of the [decode profile](#decode-profiles-drop-raw-and-unknown-fields): it
folds into the profile identity, so two TUs that disagree about which messages carry the flag **fail to
link** rather than silently holding mismatched layouts of the same type.

## Decode profiles: `drop`, `raw`, and `unknown-fields`

The schema says what *can* be on the wire; a **decode profile** says what *this consumer* does with
it — without touching the schema. Choose, per field (`pkg.Msg.field`), per type (`pkg.Msg`, covering
every field of that message type), or per message (for `unknown-fields`):

- **`drop`** — no storage, no accessor, no decode work beyond wire-validated skipping. Reading a
  dropped field is a **compile error**, not a silent default. (Dropping a `required` field is
  rejected.)
- **`unknown-fields`** — reserve that **message**'s `has_unknown_fields()` bit (see
  [Unknown fields](#unknown-fields)). Unlike `drop`/`raw` it names a message directly, not a
  field or a field's type; an enum or a field name is an error.
- **`raw`** (message-typed fields, groups included) — the sub-message's **payload** is borrowed as a
  `ByteView` view into the input instead of a materialized tree; repeated fields become a
  `StringArrayView`, one payload per element. Each view is exactly what the field type's own
  `decode()` accepts, so the tree is built only when — and if — you ask: keep a huge or
  rarely-read sub-message (or a million-element repeated field) as bytes, and decode single
  elements on demand. A singular `raw` accessor returns `std::optional<ByteView>` and carries presence
  just as the `const T*` does; a `required` field (proto2, or editions `LEGACY_REQUIRED`) has no
  presence to carry, so it returns a bare `ByteView`. Decode semantics are otherwise unchanged
  (`required` validation, duplicate-singular rejection). Scalars, strings, and enums can't go raw
  (no payload a later `decode()` could consume — they're cheap to materialize or drop), nor can maps
  (their entry type is generated internals). To defer a huge *packed scalar* array, wrap it in a sub-message
  schema-side, or walk it with the streaming decoder.

Profiles come from a file (one `drop <name>` / `raw <name>` / `unknown-fields <message>` per line, `#`
comments, an optional `name <identifier>` line) via `--field-modes=<file>`, or inline via
`--drop=<name>` / `--raw=<name>` / `--unknown=<message>` (and `--unknown-present` for every message). A
field-level entry beats a type-level entry; unknown names are hard errors; field modes do not
apply inside a oneof. The profile resolves against *everything* the invocation generates — the
entries resolve as one batch — so a global profile works by listing (or `PROTOS`-listing, in
CMake) every schema it spans in one generation; a name unknown across the whole batch is still a
hard error.

```
# lean.modes — this consumer never reads sides, and reads origin only on demand
name lean
drop demo.Shape.sides
raw  demo.Shape.origin
```

```cpp
namespace demo = rp::arena::demo;                           // arena types live under rp::arena
const demo::Shape* s = demo::Shape::decode(bytes, arena);   // s->sides() does not compile
if (s->origin()) {                                          // the Point payload (borrowed from the input)
    const demo::Point* p = demo::Point::decode(*s->origin(), arena);  // deferred: only now
}
```

A profile **changes the generated types**, so you still write `rp::arena::demo::Shape`, but two TUs generated
under different profiles (including differing only in which messages reserve the unknown-fields bit)
hold distinct types and **fail to link** rather than silently exchanging mismatched layouts. One
practical consequence: don't forward-declare generated types yourself — under a profile,
`namespace rp::arena::demo { class Shape; }` declares a *different* class. See
[`examples/consumer/lean_main.cpp`](../examples/consumer/lean_main.cpp) for the full pattern.
