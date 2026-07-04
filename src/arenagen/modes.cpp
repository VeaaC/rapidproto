// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#include "rapidproto/arenagen/modes.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto::arenagen {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Split off the first whitespace-delimited word; `rest` gets the trimmed remainder.
std::string_view first_word(std::string_view line, std::string_view& rest) {
    const std::size_t space = line.find_first_of(" \t");
    if (space == std::string_view::npos) {
        rest = {};
        return line;
    }
    rest = trim(line.substr(space + 1));
    return line.substr(0, space);
}

bool is_identifier(std::string_view s) {
    if (s.empty() || (std::isalpha(static_cast<unsigned char>(s[0])) == 0 && s[0] != '_')) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    });
}

const char* mode_word(FieldMode mode) {
    return mode == FieldMode::Raw ? "raw" : "drop";
}

// A dotted name normalized to the FQN spelling the symbol table uses (leading dot).
std::string normalize_name(std::string_view name) {
    return name.front() == '.' ? std::string(name) : "." + std::string(name);
}

// FNV-1a 64 over the normalized entry lines: the default profile identity. Stable across
// platforms, order-independent (the input is sorted), and short enough for a namespace.
std::string hash_id(const std::vector<std::string>& normalized) {
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): the FNV-1a-64
    // offset basis and prime are the algorithm's definition, not tunables.
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
        h ^= static_cast<std::uint8_t>('\n');
        h *= 0x100000001b3ULL;
    };
    for (const std::string& line : normalized) {
        mix(line);
    }
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): masked to 0-15
        out[static_cast<std::size_t>(i)] = "0123456789abcdef"[h & 0xFU];
        h >>= 4U;
    }
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    return out;
}

// Search `message` (not its nested types) for a field or map named `name`; a oneof member is
// found but flagged, so the caller can reject it with a specific error.
struct FieldHit {
    const FieldNode* field = nullptr;
    const MapFieldNode* map_field = nullptr;
    bool in_oneof = false;
};
FieldHit find_field(const MessageNode& message, std::string_view name) {
    for (const FieldNode& field : message.fields) {
        if (field.name == name) {
            return {&field, nullptr, false};
        }
    }
    for (const MapFieldNode& map : message.map_fields) {
        if (map.name == name) {
            return {nullptr, &map, false};
        }
    }
    for (const OneofNode& oneof : message.oneofs) {
        for (const FieldNode& field : oneof.fields) {
            if (field.name == name) {
                return {&field, nullptr, true};
            }
        }
    }
    return {};
}

// Pass-1 state: the entry lists split by level, plus the canonical lines they came from
// (appended at insert time -- the pointer-keyed maps are never iterated, keeping every
// derived artifact deterministic).
struct Selection {
    std::map<std::string, FieldMode> type_modes;  // type FQN -> mode (ordered: iterated in pass 2)
    std::unordered_map<const FieldNode*, FieldMode> field_modes;
    std::unordered_map<const MapFieldNode*, FieldMode> map_modes;
    std::map<std::string, std::string> origins;  // canonical line -> first origin (for conflicts)
    std::vector<std::string> normalized;
};

// The hard errors an explicit FIELD entry can hit (a type entry has its own checks above its
// insertion; type-level fan-out silently excludes these cases instead -- see apply_to_message).
std::optional<Error> field_entry_error(const ModeEntry& entry, const FieldHit& hit) {
    if (hit.in_oneof) {
        return Error{0, entry.origin + ": '" + entry.name +
                            "' is a oneof member; field modes do not apply inside a oneof"};
    }
    if (entry.mode == FieldMode::Drop && hit.field != nullptr &&
        hit.field->presence == FieldPresence::Required) {
        return Error{0, entry.origin + ": cannot drop required field '" + entry.name +
                            "' (its presence check would be meaningless)"};
    }
    // Raw stores the payload for the field type's own decode(); only message-typed fields
    // (groups included) have one. Scalars/strings/enums are cheap to materialize (or drop);
    // a map's entry type is generated internals no consumer could decode.
    if (entry.mode == FieldMode::Raw &&
        (hit.map_field != nullptr || (hit.field != nullptr && !hit.field->is_message_type))) {
        return Error{0, entry.origin + ": cannot keep '" + entry.name +
                            "' raw (raw applies to message-typed fields, whose payload the "
                            "field type's decode() accepts; materialize or drop this field)"};
    }
    return std::nullopt;
}

// Resolve one entry into `sel`; nullopt on success.
std::optional<Error> apply_entry(const ModeEntry& entry, const SymbolTable& symbols,
                                 Selection& sel) {
    if (entry.name.empty()) {
        return Error{0, entry.origin + ": empty name"};
    }
    if (entry.mode == FieldMode::Materialize) {
        return Error{0, entry.origin + ": materialize is the default, not a directive"};
    }
    const std::string fqn = normalize_name(entry.name);
    const auto conflict = [&](FieldMode existing) {
        return Error{0, entry.origin + ": '" + entry.name + "' already has mode " +
                            mode_word(existing) + " (from " +
                            sel.origins[std::string(mode_word(existing)) + " " + fqn] + ")"};
    };
    const auto note = [&](FieldMode mode) {
        const std::string line = std::string(mode_word(mode)) + " " + fqn;
        sel.origins.emplace(line, entry.origin);
        sel.normalized.push_back(line);
    };
    // A type? (message or enum -- one namespace per message scope, so a dotted name is never
    // ambiguous between a type and a field.)
    if (symbols.messages.count(fqn) != 0 || symbols.enums.count(fqn) != 0) {
        if (entry.mode == FieldMode::Raw && symbols.messages.count(fqn) == 0) {
            return Error{0, entry.origin + ": cannot keep enum '" + entry.name +
                                "' raw (raw stores a message field's payload for a later "
                                "decode(); an enum has no payload -- materialize or drop it)"};
        }
        const auto [it, inserted] = sel.type_modes.emplace(fqn, entry.mode);
        if (!inserted && it->second != entry.mode) {
            return conflict(it->second);
        }
        if (inserted) {
            note(entry.mode);
        }
        return std::nullopt;
    }
    // A field: parent FQN + field name.
    const std::size_t dot = fqn.rfind('.');
    const std::string parent = dot == 0 ? std::string{} : fqn.substr(0, dot);
    const std::string_view leaf = std::string_view(fqn).substr(dot + 1);
    const auto parent_it = symbols.messages.find(parent);
    const FieldHit hit =
        parent_it != symbols.messages.end() ? find_field(*parent_it->second, leaf) : FieldHit{};
    if (hit.field == nullptr && hit.map_field == nullptr) {
        return Error{0, entry.origin + ": unknown field or type '" + entry.name +
                            "' (a silently ignored selection would be a footgun)"};
    }
    if (auto err = field_entry_error(entry, hit)) {
        return err;
    }
    const auto record = [&](auto& map, const auto* node) -> std::optional<FieldMode> {
        const auto [it, inserted] = map.emplace(node, entry.mode);
        if (!inserted && it->second != entry.mode) {
            return it->second;  // the conflicting pre-existing mode
        }
        if (inserted) {
            note(entry.mode);
        }
        return std::nullopt;
    };
    const std::optional<FieldMode> clash = hit.field != nullptr
                                               ? record(sel.field_modes, hit.field)
                                               : record(sel.map_modes, hit.map_field);
    if (clash) {
        return conflict(*clash);
    }
    return std::nullopt;
}

// Pass 2: fan type-level modes out to every matching field, with explicit entries winning.
// Type-level modes silently leave oneof members and required+drop combinations materialized:
// with no `materialize` directive to narrow a type entry per field, a hard error there would
// make type-level selection unusable on such schemas.
void apply_to_message(const Selection& sel, const MessageNode& message, FieldModes& out) {
    const auto type_mode_for = [&](const std::string& type_fqn) -> const FieldMode* {
        const auto it = sel.type_modes.find(type_fqn);
        return it != sel.type_modes.end() ? &it->second : nullptr;
    };
    for (const FieldNode& field : message.fields) {
        const auto ex = sel.field_modes.find(&field);
        if (ex != sel.field_modes.end()) {
            out.fields[&field] = ex->second;
            continue;
        }
        if (field.resolved_type_fqn.empty()) {
            continue;
        }
        const FieldMode* tm = type_mode_for(field.resolved_type_fqn);
        if (tm != nullptr &&
            (*tm != FieldMode::Drop || field.presence != FieldPresence::Required)) {
            out.fields[&field] = *tm;
        }
    }
    for (const MapFieldNode& map : message.map_fields) {
        const auto ex = sel.map_modes.find(&map);
        if (ex != sel.map_modes.end()) {
            out.maps[&map] = ex->second;
            continue;
        }
        if (map.resolved_value_type_fqn.empty()) {
            continue;
        }
        // Raw never applies to maps (their entry type is generated internals); a type-level raw
        // on the VALUE type silently leaves the map materialized, like the other type-entry
        // exclusions. Only drop fans out to maps.
        const FieldMode* tm = type_mode_for(map.resolved_value_type_fqn);
        if (tm != nullptr && *tm != FieldMode::Raw) {
            out.maps[&map] = *tm;
        }
    }
}

void apply_selection(const Selection& sel, const ResolvedFileSet& set, FieldModes& out) {
    for (const FileNode& file : set.files) {
        std::vector<const MessageNode*> stack;
        stack.reserve(file.messages.size());
        for (const MessageNode& message : file.messages) {
            stack.push_back(&message);
        }
        while (!stack.empty()) {
            const MessageNode& message = *stack.back();
            stack.pop_back();
            for (const MessageNode& nested : message.nested_messages) {
                stack.push_back(&nested);
            }
            apply_to_message(sel, message, out);
        }
    }
}

}  // namespace

Result<std::monostate> parse_modes_file(std::string_view text, const std::string& filename,
                                        FieldModesSpec& spec) {
    int line_no = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++line_no;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        const std::string origin = filename + ":" + std::to_string(line_no);
        std::string_view arg;
        const std::string_view directive = first_word(line, arg);
        if (arg.empty() || arg.find_first_of(" \t") != std::string_view::npos) {
            return Error{0, origin + ": expected `name|drop|raw <single-argument>`, got '" +
                                std::string(line) + "'"};
        }
        if (directive == "name") {
            if (!is_identifier(arg)) {
                return Error{0, origin + ": profile name '" + std::string(arg) +
                                    "' must be a C++ identifier (it becomes part of a namespace)"};
            }
            if (!spec.profile_name.empty() && spec.profile_name != arg) {
                return Error{0, origin + ": profile already named '" + spec.profile_name + "'"};
            }
            spec.profile_name = std::string(arg);
        } else if (directive == "drop") {
            spec.entries.push_back({FieldMode::Drop, std::string(arg), origin});
        } else if (directive == "raw") {
            spec.entries.push_back({FieldMode::Raw, std::string(arg), origin});
        } else {
            return Error{0, origin + ": unknown directive '" + std::string(directive) +
                                "' (expected name, drop, or raw)"};
        }
    }
    return std::monostate{};
}

Result<FieldModes> resolve_field_modes(const FieldModesSpec& spec, const ResolvedFileSet& set,
                                       const SymbolTable& symbols) {
    FieldModes out;
    Selection sel;
    for (const ModeEntry& entry : spec.entries) {
        if (auto err = apply_entry(entry, symbols, sel)) {
            return *err;
        }
    }
    apply_selection(sel, set, out);
    if (!out.active()) {
        return out;  // incl. a no-op profile: default output, no identity, no inline namespace
    }
    out.normalized = std::move(sel.normalized);
    std::sort(out.normalized.begin(), out.normalized.end());
    // A named profile still carries a content-hash suffix: the name alone would let two DIFFERENT
    // selections that share a name link silently -- the exact ODR hole the id exists to close.
    // The name is for humans (banner, mangled-symbol readability); the hash is the identity.
    // 8 hex (32 bits) suffices: it guards against accidents, not adversaries.
    constexpr std::size_t kNameHashHexChars = 8;
    out.profile_id =
        spec.profile_name.empty()
            ? hash_id(out.normalized)
            : spec.profile_name + "_" + hash_id(out.normalized).substr(0, kNameHashHexChars);
    return out;
}

}  // namespace rapidproto::arenagen
