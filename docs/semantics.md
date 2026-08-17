# Schema features & semantics

*The shared rules — they apply to both decode models and affect how you write correct consumer code.
This page is the single home for them; the model pages ([arena](arena.md), [streaming](streaming.md))
link here instead of restating.*

- **Lifetimes.** Both models borrow the input. Streaming: the input `ByteView` must outlive the decoder
  and every `string_view` it hands a callback. Arena: the tree's structure lives in the `Arena`, but its
  strings/bytes are `string_view`s into the input, so the tree stays valid only while **both** the
  `Arena` and the input buffer live. Use [`decode_owned`](arena.md#self-contained-decode-decode_owned)
  for a `shared_ptr` that owns both.
- **Untrusted input is validated; values are not.** Wire input is fully checked for **wire-format
  integrity** (structure, lengths, group nesting), so a malformed buffer fails cleanly and never
  triggers UB. Field *values* are not range-checked: RapidProto trusts the schema, not the bytes. A
  `string` in particular is handed back unvalidated, so it may carry bytes `protoc` would reject as
  invalid UTF-8.
- **Defaults & presence.** Arena: an *implicit*-presence field (plain proto3 scalars) reads back its
  zero default (`0` / `""` / the first enum value) when absent; an *explicit*-presence scalar/string/
  enum field returns `std::optional<T>` (`std::nullopt` when absent — apply a proto2 `[default=X]`
  yourself via `value_or`); a sub-message's presence is its `const T*` accessor returning `nullptr`.
  Streaming: an absent field simply fires no callback, and no defaults are delivered.
- **Enums are open** and **shared between the models.** A proto enum becomes one `enum class :
  std::int32_t`, defined once under `rp::enums::<pkg>` and aliased into both decoders, so
  `rp::arena::<pkg>::Status` and `rp::stream::<pkg>::Status` are the same type. Nested enums too: a
  `Msg::Kind` mirrors to `rp::enums::<pkg>::Msg::Kind`, one type both models alias. An unrecognized wire value arrives as
  its raw integer cast into the enum; `INT32_MIN`/`INT32_MAX` sentinels force a `default:` arm under
  `-Wswitch`, and `rp_known_min`/`rp_known_max` carry the schema's declared value range (e.g.
  `if (v <= Status::rp_known_max)`). (They live in a shared
  `<stem>.rp.common.hpp` that each decoder `#include`s for you.) This applies to **closed** enums too
  (proto2, or editions `enum_type = CLOSED`): RapidProto intentionally decodes every enum as open —
  where protoc would route an unrecognized closed-enum value to unknown fields, RapidProto delivers
  the raw value — so do not rely on closed-enum semantics.
- **A field occurring more than once on the wire.** A conformant encoder writes each singular field
  once, but a buffer can still repeat one — most often because two serialized messages were
  **concatenated**, which protobuf defines as merging them. **Streaming applies no policy:** it
  delivers what is on the wire on the [schedule](streaming.md) it always uses — per element for
  repeated fields, per entry for maps, otherwise per occurrence — so last-wins / concatenation /
  de-duplication are yours to implement. The one case a duplicate is *not* handed to you is inside a
  map entry: that entry's `(key, value)` callback fires once, with the last `key` and last `value`
  the entry carried. **Arena** materializes
  a tree and so must choose: singular scalars, `string`, `bytes` and enums take the **last**
  occurrence (values are never joined — `"AAA"` then `"BBB"` reads back `"BBB"`); repeated fields
  **concatenate**, in any mix of packed and expanded; a oneof keeps the **last** member set. Two of
  arena's choices differ from protobuf. A **map** keeps every entry, where protobuf overwrites:
  `find()` returns the last — protobuf's value — but `size()` and iteration also see the duplicates
  protobuf collapses. And a duplicate **singular sub-message**, which protobuf merges, is instead
  **rejected** (`ArenaDecodeError::Code::RepeatedSingularMessage`, carrying the field number — for a
  map, the map's own number, since the entry is a synthetic type you never wrote). The rejection is
  unconditional rather than an attempt to tell apart the occurrences whose merge a plain overwrite
  would have matched anyway, so some rejected buffers would in fact have decoded identically; telling
  those apart needs the merge machinery itself. Besides a plain sub-message field, it covers a group,
  a `required` message field, a [`raw`](profiles.md) one, a sub-message oneof member repeating while
  the oneof still holds it, and a map entry repeating its `value`. It does *not* fire for a oneof
  whose members alternate — a different member clears the oneof, and protobuf likewise starts the
  later occurrence fresh — nor for a field a [profile](profiles.md) `drop`s, which is skipped
  unexamined. If you need merge semantics, merge upstream, or decode
  with the streaming model and combine the occurrences yourself.
- **Well-known types** (`google.protobuf.Timestamp`, etc.) decode as plain messages (their `seconds`/
  `nanos` fields), with no special Timestamp/Duration/Any semantics.
- **Extensions are not decoded**, so an extension on the wire arrives as an unknown field. A message
  marked `option message_set_wire_format = true` (a proto1-era container holding only extensions)
  is accepted with a warning and decodes as unknown fields — its schema no longer fails generation,
  but its contents are not readable.
- **Thread-safety.** A streaming `decode()` is `const` and holds no mutable state, so decoders over one
  buffer run concurrently as long as the buffer isn't mutated. An arena `decode()` mutates its `Arena`,
  so give each thread its own arena; the resulting read-only tree can then be shared.

The full list of intentional non-goals and known limitations (what is deliberately *not* supported,
and why) is in [architecture.md](../architecture.md#known-limitations-and-non-goals).
