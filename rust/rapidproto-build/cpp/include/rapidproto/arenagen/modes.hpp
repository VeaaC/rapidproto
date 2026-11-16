// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Per-field arena materialization modes: the consumer-side selection behind `--field-modes` /
// `--drop` / `--raw`. A field decodes as one of
//   - Materialize (the default): the normal arena object tree,
//   - Raw (message-typed fields only, groups included): the field's PAYLOAD, borrowed as a view into
//     the input (stored as an ArenaString, exactly like a string/bytes field), exposed as a ByteView
//     the consumer hands straight to the field type's own decode() when (and if) it wants the tree --
//     deferred decoding without materializing up front. Repeated raw is ArrayView<ArenaString>, one
//     payload per element. Decode semantics match a stored field
//     (presence, RepeatedSingularMessage, required, wire-type-mismatch skip); only the
//     representation differs. Scalars/strings/enums are cheap to materialize (or drop) and maps'
//     entry types are generated internals, so raw on those is a hard error,
//   - Drop: no storage and no accessor; records are still skip-validated.
// Selection is a CONSUMER property, not a schema property (two binaries legitimately want
// different profiles of one schema), which is why it lives in flags/profile files rather than
// schema annotations. The resolved profile also carries a content-hashed identity that the
// generator bakes into an inline namespace, so mixing profiles across TUs is a link error
// instead of a silent ODR violation.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {
struct ResolvedFileSet;  // rapidproto/resolver.hpp
struct SymbolTable;      // rapidproto/resolve.hpp
}  // namespace rapidproto

namespace rapidproto::arenagen {

enum class FieldMode : std::uint8_t { Materialize, Raw, Drop };

// One requested selection, before resolution. `name` is a dotted FQN as the user wrote it (a
// field `pkg.Msg.field` or a type `pkg.Type`; field-vs-type is auto-detected -- the two share one
// namespace per message scope, so a name is never ambiguous). `origin` labels errors
// ("modes.txt:7", "--drop").
struct ModeEntry {
    FieldMode mode = FieldMode::Materialize;
    std::string name;
    std::string origin;
};

// One requested unknown-fields selection: a message whose "unknown fields present" bit + accessor
// to reserve. `name` is a dotted FQN as the user wrote it (a message type only; unknown-field
// detection is a message-level property, so an enum or a field name is an error). `origin` labels
// errors like ModeEntry.
struct UnknownEntry {
    std::string name;
    std::string origin;
};

// The parsed, unresolved selection: every entry from every file and direct flag, in order.
struct FieldModesSpec {
    std::string profile_name;  // optional `name` directive (else the profile id is a hash)
    std::vector<ModeEntry> entries;
    std::vector<UnknownEntry> unknowns;  // `unknown-fields <msg>` / `--unknown=<msg>` selections
    bool unknown_all = false;  // `--unknown-present`: the unknown bit on EVERY message (folds into
                               // the profile id as one stable `unknown *` line -- see resolve)
};

// Parse one profile file's text into `spec` (appending): `#` comments, blank lines, and one
// directive per line -- `name <identifier>`, `drop <dotted-name>`, `raw <dotted-name>`,
// `unknown-fields <dotted-message>`. `filename` labels errors and entry origins.
[[nodiscard]] Result<std::monostate> parse_modes_file(std::string_view text,
                                                      const std::string& filename,
                                                      FieldModesSpec& spec);

// The resolved, validated selection the layout planner and generator consume.
struct FieldModes {
    std::unordered_map<const FieldNode*, FieldMode> fields;
    std::unordered_map<const MapFieldNode*, FieldMode> maps;
    // Messages whose "unknown fields present" bit + has_unknown_fields() accessor to reserve
    // (`unknown-fields <msg>`, or every message for `--unknown-present`). A message property, not
    // a field one, so it is kept separately from `fields`/`maps`.
    std::unordered_set<const MessageNode*> unknown_messages;
    // Profile identity: `<name>_<8 hex>` when the `name` directive is present, else 16 hex chars
    // -- the hex always an FNV-1a-64 over `normalized`, so the id verifies CONTENT even when
    // named (two selections sharing a name must not link). Non-empty iff active(): a profile
    // whose entries match nothing (a no-op) produces fully default output -- no identity, no
    // inline namespace.
    std::string profile_id;
    // Canonical "drop .pkg.M.f" / "raw .pkg.T" / "unknown .pkg.M" (or "unknown *") lines, sorted
    // and deduplicated -- the hash input and the generated banner's content.
    std::vector<std::string> normalized;

    [[nodiscard]] bool active() const noexcept {
        return !fields.empty() || !maps.empty() || !unknown_messages.empty();
    }
    // Whether `message` should reserve the unknown-fields bit + accessor.
    [[nodiscard]] bool wants_unknown(const MessageNode* message) const noexcept {
        return unknown_messages.count(message) != 0;
    }
};

// Resolve and validate `spec` against an analyzed file set. Errors (each naming the entry's
// origin): an unknown name; conflicting modes for one field or one type (a field-level entry
// OVERRIDES a type-level one, that is precedence, not a conflict); `drop` of a proto2 `required`
// field (its presence check would be meaningless); any mode on a oneof member (the discriminant
// owns that storage); `raw` on anything that is not a message-typed field (no payload for a
// later decode()). A type-level entry applies to every field whose resolved type matches,
// including repeated fields and (for drop) map values (the MAP field goes as a whole) -- EXCEPT
// oneof members, drop-on-required combinations, and raw-on-maps, which it silently leaves
// materialized: with no `materialize` directive to narrow a type entry per field, a hard error
// there would make type-level selection unusable on such schemas. (Naming such a field
// EXPLICITLY is still the hard error above -- the silent exclusion is a type-entry courtesy,
// not an override path.)
//
// `unknown-fields` entries resolve independently: each names a MESSAGE (an enum or a field name is
// an error -- unknown-field detection is a message property), and `spec.unknown_all` reserves the
// bit on every message. Explicit per-message entries contribute one `unknown .pkg.M` line each to
// the profile id; `unknown_all` instead contributes a single stable `unknown *` line AND subsumes any
// explicit per-message lines (they are redundant once every message is flagged), so the global flag's
// id is one value that does not shift as the schema gains messages.
[[nodiscard]] Result<FieldModes> resolve_field_modes(const FieldModesSpec& spec,
                                                     const ResolvedFileSet& set,
                                                     const SymbolTable& symbols);

}  // namespace rapidproto::arenagen
