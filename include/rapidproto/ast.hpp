#pragma once

// The normalized AST: a single model that abstracts away proto2/proto3/editions
// differences. Presence, enum openness, and repeated/message encoding are stored
// as resolved semantic enums (filled by the parser + the editions feature pass).
// Everything that affects decoding is a typed field; everything else (custom
// options, etc.) is retained raw under `options`.
//
// gRPC is out of scope: there is no service/method node. `extend`, extension
// ranges, and groups ARE retained (a group is represented after the parser
// synthesizes it as a nested MessageNode plus a FieldNode with is_group = true).
//
// Value semantics: nodes are plain copyable/movable structs. The recursive option
// value (OptionValue -> MessageLiteral/ListLiteral -> OptionValue) is broken with
// forward declarations + std::vector's incomplete-type support, so no heap
// indirection is needed.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "rapidproto/source_id.hpp"

namespace rapidproto {

// --- normalized semantic enums ---------------------------------------------

enum class SyntaxLevel : std::uint8_t { Proto2, Proto3, Edition };
enum class FieldPresence : std::uint8_t { Explicit, Implicit, Required };
enum class EnumOpenness : std::uint8_t { Open, Closed };
enum class RepeatedEncoding : std::uint8_t { Packed, Expanded };
enum class MessageEncoding : std::uint8_t {
    LengthPrefixed,
    Delimited
};  // Delimited = group wire format
enum class ImportKind : std::uint8_t { Standard, Public, Weak, Option };
enum class Visibility : std::uint8_t { Default, Export, Local };

// `to max` resolves to a different sentinel depending on context. kMaxMessageFieldNumber mirrors
// runtime.hpp's kMaxFieldNumber (same 2^29-1 value), kept separate so the AST has no wire-layer
// dep.
inline constexpr std::int32_t kMaxMessageFieldNumber = 536870911;  // 2^29 - 1
inline constexpr std::int32_t kMaxEnumNumber = INT32_MAX;

// --- option values: the protobuf text-format value tree --------------------

// An identifier-form option value — an enum value name, or `true`/`false` when an
// option is retained raw — kept distinct from a string literal. (Special floats are
// NOT identifiers: inf/nan/-inf/-nan are stored as `double`, i.e. +/-HUGE_VAL or NaN.)
struct Identifier {
    std::string name;
};

struct OptionValue;          // recursive
struct MessageLiteralField;  // recursive

// MessageLiteral: `{ field ... }` or `< field ... >`.
struct MessageLiteral {
    std::vector<MessageLiteralField> fields;
};

// ListLiteral: `[ value, ... ]` (text-format list).
struct ListLiteral {
    std::vector<OptionValue> elements;
};

// A text-format scalar/aggregate value. When constructing the integer alternatives
// directly, use explicitly-sized literals (std::int64_t{...} / std::uint64_t{...}) to
// avoid std::variant overload ambiguity.
struct OptionValue {
    std::variant<bool, std::int64_t, std::uint64_t, double, Identifier, std::string, MessageLiteral,
                 ListLiteral>
        value;
};

// A message-literal field name is a simple name, an extension `[a.b.c]`, or an
// Any type-URL `[domain/type]`.
enum class MessageFieldNameKind : std::uint8_t { Simple, Extension, AnyTypeUrl };
struct MessageLiteralField {
    MessageFieldNameKind name_kind = MessageFieldNameKind::Simple;
    std::string name;  // Simple: field name; Extension: the [qualified.name]; AnyTypeUrl: domain
    std::string any_type;  // AnyTypeUrl only: the type after '/'
    OptionValue value;
};

// --- options ----------------------------------------------------------------

// One dotted component of an option name; `(foo.bar)` is an extension component.
struct OptionNameComponent {
    std::string name;
    bool is_extension = false;
};

struct Option {
    std::vector<OptionNameComponent> name;
    OptionValue value;
};

// --- shared: number ranges (reserved / extension ranges) -------------------

// Inclusive [start, end]; `end` may be a max sentinel (see kMax* above).
struct NumberRange {
    std::int32_t start = 0;
    std::int32_t end = 0;
};

struct ReservedNode {
    std::vector<NumberRange> ranges;
    std::vector<std::string> names;
};

// --- enums ------------------------------------------------------------------

struct EnumValueNode {
    std::string name;
    std::int32_t number = 0;
    std::vector<Option> options;
    std::string fqn;  // set by the FQN-computation pass
};

struct EnumNode {
    std::string name;
    Visibility visibility = Visibility::Default;
    // normalized from syntax at parse time; refined for editions by the feature pass
    EnumOpenness openness = EnumOpenness::Open;
    std::vector<EnumValueNode> values;
    std::vector<ReservedNode> reserved;
    std::vector<Option> options;
    std::string fqn;  // set by the FQN-computation pass
};

// --- fields -----------------------------------------------------------------

struct FieldNode {
    std::string name;
    std::string type_name;  // as written: a scalar keyword or a (possibly qualified) type reference
    std::int32_t number = 0;
    // Source byte offset (the field name token) -> file:line:col via FileNode::source +
    // render_error. Only the nodes a semantic pass errors on carry an offset today
    // (FieldNode/MapFieldNode/ ExtendNode); add one to a message/enum/oneof node when a pass starts
    // reporting against it.
    std::size_t offset = 0;

    // Normalized, decode-relevant attributes (filled by parsing + the feature pass,
    // corrected for user-defined types by type resolution).
    FieldPresence presence = FieldPresence::Explicit;
    bool is_repeated = false;
    RepeatedEncoding repeated_encoding = RepeatedEncoding::Expanded;
    bool is_group = false;
    MessageEncoding message_encoding = MessageEncoding::LengthPrefixed;
    // proto2 [default = ...], stored RAW/uninterpreted: a numeric default is an int/uint/double
    // OptionValue arm, but an ENUM default is an Identifier (the value NAME, not its number) and a
    // bool default is Identifier{"true"}/{"false"} -- so this is NOT a resolved scalar. A consumer
    // that materializes defaults must interpret it against the field's resolved type. (A
    // MessageLiteral/ListLiteral never legitimately appears here.)
    std::optional<OptionValue> default_value;

    std::vector<Option> options;  // all options retained raw

    // set by the FQN-computation pass for EXTENSION fields only (declaration scope + name);
    // empty for regular fields:
    std::string fqn;

    // set by the type-resolution pass:
    std::string resolved_type_fqn;  // empty for scalar types
    bool is_message_type = false;
    bool is_enum_type = false;
};

struct MapFieldNode {
    std::string name;
    std::string key_type;
    std::string value_type;
    std::int32_t number = 0;
    std::size_t offset = 0;  // source byte offset (the field name token), for diagnostics
    std::vector<Option> options;

    // set by the type-resolution pass (resolves the value type; map keys are always scalar):
    std::string resolved_value_type_fqn;
    bool value_is_message = false;
    bool value_is_enum = false;
};

struct OneofNode {
    std::string name;
    std::vector<FieldNode> fields;
    std::vector<Option> options;
};

struct ExtensionRangeNode {
    std::vector<NumberRange> ranges;
    std::vector<Option> options;
};

// --- messages / extend / file ----------------------------------------------

struct MessageNode;  // recursive (nested_messages)

// `extend Extendee { ... }`. Valid at file scope (FileNode::extends) and nested
// in a message (MessageNode::extends). A group inside it becomes a FieldNode with
// is_group = true here, plus a synthesized MessageNode in the enclosing scope.
struct ExtendNode {
    std::string extendee_type_name;
    std::size_t offset = 0;  // source byte offset (the `extend` keyword), for diagnostics
    std::vector<FieldNode> fields;
    std::vector<Option> options;
};

struct MessageNode {
    std::string name;
    Visibility visibility = Visibility::Default;
    std::vector<FieldNode> fields;
    std::vector<MapFieldNode> map_fields;
    std::vector<OneofNode> oneofs;
    std::vector<EnumNode> enums;
    std::vector<MessageNode> nested_messages;
    std::vector<ReservedNode> reserved;
    std::vector<ExtensionRangeNode> extension_ranges;
    std::vector<ExtendNode> extends;
    std::vector<Option> options;
    std::string fqn;  // set by the FQN-computation pass
};

struct ImportNode {
    std::string path;
    ImportKind kind = ImportKind::Standard;
};

struct FileNode {
    SyntaxLevel syntax_level = SyntaxLevel::Proto2;  // absent syntax/edition decl => proto2
    std::string edition;                             // for SyntaxLevel::Edition (e.g. "2023")
    std::string package;
    std::string filename;
    SourceId
        source;  // set by the resolver; combined with a node's `offset` to render file:line:col
    std::vector<ImportNode> imports;
    std::vector<MessageNode> messages;
    std::vector<EnumNode> enums;
    std::vector<ExtendNode> extends;
    std::vector<Option> options;
};

}  // namespace rapidproto
