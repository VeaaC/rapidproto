#include "rapidproto/features.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "rapidproto/result.hpp"

#include "rapidproto/ast.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/scalar.hpp"

namespace rapidproto {
namespace {

// The decode-relevant resolved FeatureSet at a scope. utf8_validation is carried for
// fidelity but not persisted (the AST has no field for it; it is non-critical for
// decoding). json_format is out of scope (no JSON).
struct ResolvedFeatures {
    FieldPresence field_presence = FieldPresence::Explicit;
    EnumOpenness enum_type = EnumOpenness::Open;
    MessageEncoding message_encoding = MessageEncoding::LengthPrefixed;
    RepeatedEncoding repeated_field_encoding = RepeatedEncoding::Packed;
    bool utf8_validation_verify = true;
};

// The editions this decoder knows the decode-relevant feature defaults for. THE list -- an edition
// absent from it is rejected rather than decoded by these defaults, because a later edition may
// change one and applying today's rules to it would decode the schema wrongly, and silently.
//
// A released protoc rejects an unknown edition too, so no schema one accepts TODAY lands here. That
// is a fact about protoc's current set, not an invariant: `descriptor.proto` already declares
// EDITION_2026, and the day it ships, schemas using it arrive here and are refused until this list
// grows. Growing it means checking that edition's defaults against the table in architecture.md, not
// appending a string -- though as of protobuf v35 none of the five decode-relevant features carries
// an `edition_defaults` entry above EDITION_2023, so 2024 and 2026 both still resolve to the 2023
// values.
constexpr std::array<std::string_view, 2> kKnownEditions = {"2023", "2024"};

bool is_known_edition(std::string_view edition) {
    return std::find(kKnownEditions.begin(), kKnownEditions.end(), edition) != kKnownEditions.end();
}

ResolvedFeatures edition_defaults(const std::string& /*edition*/) {
    // Every edition in kKnownEditions shares these decode-relevant defaults, so the argument is
    // unused today; branch on it here when an edition arrives that changes one.
    return ResolvedFeatures{};
}

// Apply one `features.<name> = <VALUE>` setting (VALUE is an enum identifier).
void apply_one_feature(const std::string& name, const std::string& value, ResolvedFeatures& out) {
    if (name == "field_presence") {
        if (value == "EXPLICIT") {
            out.field_presence = FieldPresence::Explicit;
        } else if (value == "IMPLICIT") {
            out.field_presence = FieldPresence::Implicit;
        } else if (value == "LEGACY_REQUIRED") {
            out.field_presence = FieldPresence::Required;
        }
    } else if (name == "enum_type") {
        if (value == "OPEN") {
            out.enum_type = EnumOpenness::Open;
        } else if (value == "CLOSED") {
            out.enum_type = EnumOpenness::Closed;
        }
    } else if (name == "repeated_field_encoding") {
        if (value == "PACKED") {
            out.repeated_field_encoding = RepeatedEncoding::Packed;
        } else if (value == "EXPANDED") {
            out.repeated_field_encoding = RepeatedEncoding::Expanded;
        }
    } else if (name == "message_encoding") {
        if (value == "LENGTH_PREFIXED") {
            out.message_encoding = MessageEncoding::LengthPrefixed;
        } else if (value == "DELIMITED") {
            out.message_encoding = MessageEncoding::Delimited;
        }
    } else if (name == "utf8_validation") {
        if (value == "VERIFY") {
            out.utf8_validation_verify = true;
        } else if (value == "NONE") {
            out.utf8_validation_verify = false;
        }
    }
}

// Aggregate form: `option features = { field_presence: IMPLICIT, ... };`.
void apply_aggregate_features(const MessageLiteral& literal, ResolvedFeatures& out) {
    for (const MessageLiteralField& field : literal.fields) {
        if (field.name_kind != MessageFieldNameKind::Simple) {
            continue;  // extension / Any sub-features (e.g. (pb.cpp).x) are not decode-relevant
        }
        if (const auto* value = std::get_if<Identifier>(&field.value.value)) {
            apply_one_feature(field.name, value->name, out);
        }
    }
}

// Merge every `features.*` option from `options` on top of `out` (later overrides earlier).
// Accepts both the dotted form (`option features.X = VALUE;`) and the aggregate form
// (`option features = { X: VALUE };`); the parser retains both.
void apply_features(const std::vector<Option>& options, ResolvedFeatures& out) {
    for (const Option& option : options) {
        if (option.name.empty() || option.name[0].is_extension ||
            option.name[0].name != "features") {
            continue;
        }
        if (option.name.size() == 1) {
            if (const auto* literal = std::get_if<MessageLiteral>(&option.value.value)) {
                apply_aggregate_features(*literal, out);
            }
        } else if (option.name.size() == 2) {
            if (const auto* value = std::get_if<Identifier>(&option.value.value)) {
                apply_one_feature(option.name[1].name, value->name, out);
            }
        }
    }
}

void resolve_field(FieldNode& field, ResolvedFeatures scope, bool is_oneof_member) {
    apply_features(field.options, scope);
    if (!is_oneof_member) {
        field.presence = scope.field_presence;  // oneof members are always explicit
    }
    if (!field.is_group) {
        field.message_encoding = scope.message_encoding;
    }
    // Only packable scalars are affected here. Message/string/bytes stay Expanded; enum-typed
    // repeated fields aren't known yet (type unresolved) and the type-resolution pass forces them
    // to Expanded as a non-critical simplification (decoders accept both packed and expanded wire
    // forms), so a PACKED/EXPANDED feature on a repeated enum is not reflected in
    // repeated_encoding.
    if (field.is_repeated && is_packable_scalar(field.type_name)) {
        field.repeated_encoding = scope.repeated_field_encoding;
    }
}

void resolve_enum(EnumNode& node, ResolvedFeatures scope) {
    apply_features(node.options, scope);
    node.openness = scope.enum_type;
    // The chain stops here: enum VALUES carry no decode-relevant feature (no FeatureSet field
    // targets ENUM_ENTRY), so node.values are intentionally not walked.
}

void resolve_extend(ExtendNode& node, ResolvedFeatures scope) {
    apply_features(node.options, scope);
    for (auto& field : node.fields) {
        resolve_field(field, scope, /*is_oneof_member=*/false);
    }
}

void resolve_message(MessageNode& message, ResolvedFeatures scope) {
    apply_features(message.options, scope);
    for (auto& field : message.fields) {
        resolve_field(field, scope, /*is_oneof_member=*/false);
    }
    for (auto& oneof : message.oneofs) {
        ResolvedFeatures oneof_scope = scope;
        apply_features(oneof.options, oneof_scope);
        for (auto& field : oneof.fields) {
            resolve_field(field, oneof_scope, /*is_oneof_member=*/true);
        }
    }
    for (auto& node : message.enums) {
        resolve_enum(node, scope);
    }
    for (auto& nested : message.nested_messages) {
        resolve_message(nested, scope);
    }
    for (auto& extend : message.extends) {
        resolve_extend(extend, scope);
    }
    // map_fields are intentionally skipped: MapFieldNode carries no presence/encoding fields to
    // write back (the entry message is synthesized downstream by codegen).
}

}  // namespace

Result<std::monostate> resolve_features(FileNode& file) {
    if (file.syntax_level != SyntaxLevel::Edition) {
        return std::monostate{};  // proto2/proto3 presence/openness/encoding fixed at parse time
    }
    if (!is_known_edition(file.edition)) {
        std::string known;
        for (const std::string_view name : kKnownEditions) {
            known += known.empty() ? "" : ", ";
            known += name;
        }
        // Cross-file form: an unknown edition can arrive in an IMPORT, so the diagnostic has to name
        // which file it came from, not only the offset.
        return Error{file.source, file.edition_offset,
                     "unknown edition \"" + file.edition +
                         "\"; this decoder knows the feature defaults for " + known +
                         " only, and a later edition may change one -- decoding it by those rules "
                         "could be silently wrong"};
    }
    ResolvedFeatures scope = edition_defaults(file.edition);
    apply_features(file.options, scope);
    for (auto& message : file.messages) {
        resolve_message(message, scope);
    }
    for (auto& node : file.enums) {
        resolve_enum(node, scope);
    }
    for (auto& extend : file.extends) {
        resolve_extend(extend, scope);
    }
    return std::monostate{};
}

Result<std::monostate> resolve_features(ResolvedFileSet& file_set) {
    for (auto& file : file_set.files) {
        // The set is in dependency order, so an unknown edition in an IMPORT is reported before the
        // importer is resolved against defaults that may not apply to it.
        if (auto resolved = resolve_features(file); !resolved) {
            return std::move(resolved).error();
        }
    }
    return std::monostate{};
}

}  // namespace rapidproto
