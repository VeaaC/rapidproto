# The debug dumper

*Generated with `--dump` (implies `--arena`). Header: `<stem>.rp.dump.hpp`. Back to the
[README](../README.md).*

`--dump` emits a third header that prints a decoded arena tree as human-readable, JSON-*like* text — a
**debugging and logging aid**, not a spec-compliant JSON codec and not a wire serializer. It reads the
[arena decoder](arena.md)'s public accessors (no reflection, no `descriptor.proto`), so `--dump`
**implies `--arena`** and dumps whatever the arena header exposes. For each message `Foo` in namespace
`example` it emits two free functions:

```cpp
void        ex::rp_dump_write(std::ostream& os, const ex::Foo& m,
                                   const rapidproto::dump::DumpOptions& opts = {});
std::string ex::rp_dump_string(const ex::Foo& m,
                                   const rapidproto::dump::DumpOptions& opts = {});
```

```sh
./build/release/rapidprotoc --dump -I. --out-dir=out person.proto
# out/person.rp.dump.hpp + the arena header + out/rapidproto/dump_runtime.hpp
```

```cpp
#include "person.rp.hpp"
#include "person.rp.dump.hpp"

namespace ex = rp::arena::example;   // the dump functions sit beside the arena types
const ex::Person* p = ex::Person::decode(rapidproto::ByteView(buf), arena);
std::cout << ex::rp_dump_string(*p) << '\n';         // or: rp_dump_write(std::cout, *p, 120);
```

`DumpOptions` tunes a dump. Every field has a default and an integer converts to a width, so
`rp_dump_string(m, 120)` and `rp_dump_write(std::cout, *p, 120)` above are whole-options calls:

```cpp
rapidproto::dump::DumpOptions opts;
opts.width  = 100;                              // line-width budget (compact vs one-entry-per-line)
opts.indent = 2;                                // start two nesting levels in, to nest under other output
opts.skip   = {"email", "address.zip"};         // omit these fields by qualified path (subtree and all)
std::cout << ex::rp_dump_string(*p, opts);
```

- **`skip`** names fields by their **dotted path** from the message root (`"address.zip"`, not just
  `"zip"`), so the same leaf name is hidden only where you mean it; naming a sub-message path
  (`"address"`) drops its whole subtree. The field is still decoded — just not printed. Paths carry no
  index, so a path *through* a repeated or map field applies to every element (`"orders.total"` hides
  `total` in every order); a map's keys are not themselves path-addressable. The paths are
  `string_view`s, so whatever they point at must outlive the dump call.
- **`indent`** starts the output at a nesting level (each level = 2 columns): the opening brace stays at
  the cursor, continuation lines indent that much deeper, and the width budget shrinks accordingly.

What it renders: scalars, `string`, `bytes` (as lowercase hex), enums by their prefix-stripped name
(`UNKNOWN(<n>)` for an open-enum value outside the schema's range), nested sub-messages, repeated fields
(arrays), maps (objects), and the active member of a oneof; groups print through the identical
nested-message accessor. A `bool` prints as `true`/`false`, including as a `map<bool, …>` key.
`float`/`double` print with enough digits to read back to the same value, without padding out to the
type's maximum, and the non-finite ones as the quoted strings `"NaN"` / `"Infinity"` / `"-Infinity"` —
JSON has no number syntax for those. Every value is formatted by the dumper itself, so the text does
not vary with the locale or format flags of the stream you write to (and the dump leaves both alone).
Default-valued implicit (proto3 singular) fields and empty repeated/maps are omitted;
explicit-presence fields print when set; a `required` field always prints. A message that
reserves the [unknown-fields](profiles.md#unknown-fields) bit shows `"has_unknown_fields": true` when
set — a bit only, since the arena retains no unknown-field *data*. The output is **width-adaptive**:
each object or array renders on one line if it fits the budget (`width`, default 120 columns),
otherwise one entry per line — with a wide array filling as many aligned columns as fit. Well-known
types (`Timestamp`, etc.) print as their nested fields, with no special JSON form.
