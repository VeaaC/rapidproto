#pragma once

// Test-support: serialize a fully-analyzed AST (a ResolvedFileSet after analyze(), plus
// its SymbolTable) to a deterministic, human-readable text format. This is NOT protobuf
// serialization (the library is decode-only and never serializes wire/JSON) -- it is a
// debug dump used only by the golden tests to pin the exact shape of the syntax tree,
// including every decode-relevant resolved attribute (FQNs, resolved types, presence,
// encoding, openness, defaults, the full option-value tree, and the extension registry).
//
// The format is intentionally stable and diff-friendly: two-space indentation, one node
// per line, attributes in a fixed order. unordered containers (the symbol table) are
// sorted before emission so the output is reproducible.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

namespace rapidproto::astdump {

class Dumper {
public:
    std::string dump(const ResolvedFileSet& set, const SymbolTable& symbols) {
        for (const auto& file : set.files) {
            dump_file(file);
        }
        dump_symbols(symbols);
        return std::move(m_out);
    }

private:
    std::string m_out;
    int m_indent = 0;

    void line(const std::string& text) {
        for (int i = 0; i < m_indent; ++i) {
            m_out += "  ";
        }
        m_out += text;
        m_out += '\n';
    }

    struct Indent {
        explicit Indent(Dumper& dumper) : m_dumper(dumper) { ++m_dumper.m_indent; }
        ~Indent() { --m_dumper.m_indent; }
        Indent(const Indent&) = delete;
        Indent& operator=(const Indent&) = delete;
        Indent(Indent&&) = delete;
        Indent& operator=(Indent&&) = delete;
        Dumper& m_dumper;
    };

    // --- scalars / leaf formatting ---------------------------------------

    static std::string escape(const std::string& str) {
        std::string out;
        for (const char raw : str) {
            const auto ch = static_cast<unsigned char>(raw);
            switch (ch) {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                default:
                    if (ch < 0x20 || ch > 0x7e) {
                        std::array<char, 8> buf{};
                        std::snprintf(buf.data(), buf.size(), "\\x%02x", ch);
                        out += buf.data();
                    } else {
                        out += static_cast<char>(ch);
                    }
            }
        }
        return out;
    }

    static std::string format_double(double value) {
        if (std::isnan(value)) {
            return "nan";
        }
        if (std::isinf(value)) {
            return value < 0 ? "-inf" : "inf";
        }
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), "%g", value);
        return buf.data();
    }

    static std::string presence_str(FieldPresence presence) {
        switch (presence) {
            case FieldPresence::Explicit:
                return "explicit";
            case FieldPresence::Implicit:
                return "implicit";
            case FieldPresence::Required:
                return "required";
        }
        return "?";
    }

    static std::string visibility_str(Visibility visibility) {
        switch (visibility) {
            case Visibility::Default:
                return "";
            case Visibility::Export:
                return " export";
            case Visibility::Local:
                return " local";
        }
        return "";
    }

    // --- option value tree -----------------------------------------------

    static std::string value_str(const OptionValue& value) {
        return std::visit(
            [](const auto& held) -> std::string {
                using T = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<T, bool>) {
                    return held ? "true" : "false";
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return "i" + std::to_string(held);
                } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                    return "u" + std::to_string(held);
                } else if constexpr (std::is_same_v<T, double>) {
                    return "d" + format_double(held);
                } else if constexpr (std::is_same_v<T, Identifier>) {
                    return "id:" + held.name;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return "\"" + escape(held) + "\"";
                } else if constexpr (std::is_same_v<T, MessageLiteral>) {
                    return message_literal_str(held);
                } else {  // ListLiteral
                    return list_literal_str(held);
                }
            },
            value.value);
    }

    static std::string message_literal_str(const MessageLiteral& literal) {
        std::string out = "{";
        bool first = true;
        for (const auto& field : literal.fields) {
            if (!first) {
                out += " ";
            }
            first = false;
            switch (field.name_kind) {
                case MessageFieldNameKind::Simple:
                    out += field.name;
                    break;
                case MessageFieldNameKind::Extension:
                    out += "[" + field.name + "]";
                    break;
                case MessageFieldNameKind::AnyTypeUrl:
                    out += "[" + field.name + "/" + field.any_type + "]";
                    break;
            }
            out += ":" + value_str(field.value);
        }
        out += "}";
        return out;
    }

    static std::string list_literal_str(const ListLiteral& literal) {
        std::string out = "[";
        bool first = true;
        for (const auto& element : literal.elements) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += value_str(element);
        }
        out += "]";
        return out;
    }

    static std::string option_name_str(const Option& option) {
        std::string out;
        bool first = true;
        for (const auto& component : option.name) {
            if (!first) {
                out += ".";
            }
            first = false;
            out += component.is_extension ? "(" + component.name + ")" : component.name;
        }
        return out;
    }

    void dump_options(const std::vector<Option>& options) {
        for (const auto& option : options) {
            line("option " + option_name_str(option) + " = " + value_str(option.value));
        }
    }

    // --- ranges / reserved -----------------------------------------------

    static std::string range_str(const NumberRange& range) {
        if (range.start == range.end) {
            return "[" + std::to_string(range.start) + "]";
        }
        std::string end = range.end == kMaxMessageFieldNumber ? "maxfield"
                          : range.end == kMaxEnumNumber       ? "maxenum"
                                                              : std::to_string(range.end);
        return "[" + std::to_string(range.start) + ".." + end + "]";
    }

    void dump_reserved(const std::vector<ReservedNode>& reserved) {
        for (const auto& node : reserved) {
            std::string nums = "reserved nums";
            for (const auto& range : node.ranges) {
                nums += " " + range_str(range);
            }
            if (!node.ranges.empty()) {
                line(nums);
            }
            std::string names = "reserved names";
            for (const auto& name : node.names) {
                names += " \"" + name + "\"";
            }
            if (!node.names.empty()) {
                line(names);
            }
        }
    }

    // --- fields ----------------------------------------------------------

    void dump_field(const FieldNode& field, const char* tag) {
        std::string head = tag;
        head += " " + field.name + " num=" + std::to_string(field.number);
        head += " presence=" + presence_str(field.presence);
        if (field.is_repeated) {
            head += field.repeated_encoding == RepeatedEncoding::Packed ? " repeated=packed"
                                                                        : " repeated=expanded";
        }
        if (field.is_group) {
            head += " group";
        }
        if (field.message_encoding == MessageEncoding::Delimited) {
            head += " delimited";
        }
        head += " type=" + field.type_name;
        if (!field.resolved_type_fqn.empty()) {
            head += " -> " + field.resolved_type_fqn;
            head += field.is_message_type ? " msg" : (field.is_enum_type ? " enum" : "");
        }
        if (!field.fqn.empty()) {
            head += " fqn=" + field.fqn;
        }
        line(head);
        const Indent indent(*this);
        if (field.default_value.has_value()) {
            line("default = " + value_str(*field.default_value));
        }
        dump_options(field.options);
    }

    void dump_map(const MapFieldNode& field) {
        std::string head = "map " + field.name + " num=" + std::to_string(field.number);
        head += " key=" + field.key_type + " value=" + field.value_type;
        if (!field.resolved_value_type_fqn.empty()) {
            head += " -> " + field.resolved_value_type_fqn;
            head += field.value_is_message ? " msg" : (field.value_is_enum ? " enum" : "");
        }
        line(head);
        const Indent indent(*this);
        dump_options(field.options);
    }

    void dump_oneof(const OneofNode& oneof) {
        line("oneof " + oneof.name);
        const Indent indent(*this);
        dump_options(oneof.options);
        for (const auto& field : oneof.fields) {
            dump_field(field, "field");
        }
    }

    // --- enums -----------------------------------------------------------

    void dump_enum(const EnumNode& node) {
        line("enum " + node.name + " [fqn=" + node.fqn +
             (node.openness == EnumOpenness::Open ? " open]" : " closed]") +
             visibility_str(node.visibility));
        const Indent indent(*this);
        dump_options(node.options);
        dump_reserved(node.reserved);
        for (const auto& value : node.values) {
            line("value " + value.name + " = " + std::to_string(value.number) +
                 " [fqn=" + value.fqn + "]");
            const Indent value_indent(*this);
            dump_options(value.options);
        }
    }

    // --- extend ----------------------------------------------------------

    void dump_extend(const ExtendNode& node) {
        line("extend " + node.extendee_type_name);
        const Indent indent(*this);
        dump_options(node.options);
        for (const auto& field : node.fields) {
            dump_field(field, "field");
        }
    }

    // --- messages --------------------------------------------------------

    void dump_message(const MessageNode& node) {
        line("message " + node.name + " [fqn=" + node.fqn + "]" + visibility_str(node.visibility));
        const Indent indent(*this);
        dump_options(node.options);
        dump_reserved(node.reserved);
        for (const auto& range : node.extension_ranges) {
            std::string head = "extension-range";
            for (const auto& number_range : range.ranges) {
                head += " " + range_str(number_range);
            }
            line(head);
            const Indent range_indent(*this);
            dump_options(range.options);
        }
        for (const auto& field : node.fields) {
            dump_field(field, "field");
        }
        for (const auto& field : node.map_fields) {
            dump_map(field);
        }
        for (const auto& oneof : node.oneofs) {
            dump_oneof(oneof);
        }
        for (const auto& nested : node.enums) {
            dump_enum(nested);
        }
        for (const auto& nested : node.nested_messages) {
            dump_message(nested);
        }
        for (const auto& extend : node.extends) {
            dump_extend(extend);
        }
    }

    // --- file ------------------------------------------------------------

    static std::string syntax_str(const FileNode& file) {
        switch (file.syntax_level) {
            case SyntaxLevel::Proto2:
                return "proto2";
            case SyntaxLevel::Proto3:
                return "proto3";
            case SyntaxLevel::Edition:
                return "edition " + file.edition;
        }
        return "?";
    }

    static std::string import_kind_str(ImportKind kind) {
        switch (kind) {
            case ImportKind::Standard:
                return "";
            case ImportKind::Public:
                return "public ";
            case ImportKind::Weak:
                return "weak ";
            case ImportKind::Option:
                return "option ";
        }
        return "";
    }

    void dump_file(const FileNode& file) {
        line("file " + file.filename);
        const Indent indent(*this);
        line("syntax " + syntax_str(file));
        if (!file.package.empty()) {
            line("package " + file.package);
        }
        for (const auto& import : file.imports) {
            line("import " + import_kind_str(import.kind) + "\"" + import.path + "\"");
        }
        dump_options(file.options);
        for (const auto& node : file.enums) {
            dump_enum(node);
        }
        for (const auto& node : file.messages) {
            dump_message(node);
        }
        for (const auto& extend : file.extends) {
            dump_extend(extend);
        }
    }

    // --- symbol table + extension registry -------------------------------

    void dump_symbols(const SymbolTable& symbols) {
        line("symbols");
        const Indent indent(*this);
        std::vector<std::pair<std::string, SymbolKind>> sorted(symbols.symbols.begin(),
                                                               symbols.symbols.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        for (const auto& [fqn, kind] : sorted) {
            line(fqn + " " + (kind == SymbolKind::Message ? "message" : "enum"));
        }
        line("extensions");
        const Indent ext_indent(*this);
        for (const auto& [key, field] : symbols.extensions) {  // std::map: already ordered
            line("(" + key.first + ", " + std::to_string(key.second) + ") -> " + field->fqn);
        }
    }
};

inline std::string dump_ast(const ResolvedFileSet& set, const SymbolTable& symbols) {
    return Dumper().dump(set, symbols);
}

}  // namespace rapidproto::astdump
