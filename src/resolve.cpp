#include "rapidproto/resolve.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/features.hpp"
#include "rapidproto/interpret.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/scalar.hpp"
#include "rapidproto/source_id.hpp"

namespace rapidproto {
namespace {

// Enum values are SIBLING-scoped: `scope` is the enum's enclosing scope, so a value's FQN is
// `scope.value` (e.g. ".pkg.RED"), NOT `enum.value`.
void assign_enum_fqn(EnumNode& node, const std::string& scope) {
    node.fqn = scope + "." + node.name;
    for (auto& value : node.values) {
        value.fqn = scope + "." + value.name;
    }
}

// Extension fields are qualified by their DECLARATION scope (where the `extend` block is), not by
// the extendee.
void assign_extend_fqns(ExtendNode& node, const std::string& scope) {
    for (auto& field : node.fields) {
        field.fqn = scope + "." + field.name;
    }
}

void assign_message_fqn(MessageNode& message, const std::string& scope) {
    message.fqn = scope + "." + message.name;
    for (auto& nested : message.nested_messages) {
        assign_message_fqn(nested, message.fqn);
    }
    for (auto& node : message.enums) {
        assign_enum_fqn(node, message.fqn);
    }
    for (auto& extend : message.extends) {
        assign_extend_fqns(extend, message.fqn);
    }
}

// --- type reference resolution ----------------------------------------------

// Strip the last dotted component: ".pkg.M" -> ".pkg" -> "".
std::string parent_scope(const std::string& scope) {
    const auto dot = scope.rfind('.');
    return dot == std::string::npos ? std::string{} : scope.substr(0, dot);
}

class TypeResolver {
public:
    explicit TypeResolver(ResolvedFileSet& file_set) : m_files(&file_set) {}

    Result<SymbolTable> run() {
        collect_symbols();
        for (std::size_t i = 0; i < files().size(); ++i) {
            const std::unordered_set<std::size_t> visible = visible_files(i);
            FileNode& file = files()[i];
            m_source = file.source;  // errors below point into this file (node.offset)
            for (auto& message : file.messages) {
                if (auto err = resolve_message(message, visible)) {
                    return *err;
                }
            }
            const std::string file_scope =
                file.package.empty() ? std::string{} : "." + file.package;
            for (auto& extend : file.extends) {
                if (auto err = resolve_extend(extend, file_scope, visible)) {
                    return *err;
                }
            }
        }
        return std::move(m_table);
    }

private:
    struct Sym {
        SymbolKind kind;
        std::size_t file_index;
    };

    std::vector<FileNode>& files() { return m_files->files; }

    void record(const std::string& fqn, SymbolKind kind, std::size_t file_index) {
        m_symbols[fqn] = Sym{kind, file_index};
        m_table.symbols[fqn] = kind;
    }

    void collect_message(MessageNode& message, std::size_t file_index) {
        record(message.fqn, SymbolKind::Message, file_index);
        m_table.messages.emplace(message.fqn, &message);
        for (auto& nested : message.nested_messages) {
            collect_message(nested, file_index);
        }
        for (auto& node : message.enums) {
            record(node.fqn, SymbolKind::Enum, file_index);
            m_table.enums.emplace(node.fqn, &node);
        }
    }

    void collect_symbols() {
        for (std::size_t i = 0; i < files().size(); ++i) {
            for (auto& message : files()[i].messages) {
                collect_message(message, i);
            }
            for (auto& node : files()[i].enums) {
                record(node.fqn, SymbolKind::Enum, i);
                m_table.enums.emplace(node.fqn, &node);
            }
        }
    }

    std::optional<std::size_t> import_index(const std::string& path) const {
        const auto it = m_files->file_index.find(path);
        if (it == m_files->file_index.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Transitively add files reachable from `start` via public imports.
    void add_public_closure(std::size_t start, std::unordered_set<std::size_t>& visible) {
        std::vector<std::size_t> stack{start};
        while (!stack.empty()) {
            const std::size_t current = stack.back();
            stack.pop_back();
            for (const auto& import : files()[current].imports) {
                if (import.kind != ImportKind::Public) {
                    continue;
                }
                if (const auto target = import_index(canonical_import_path(import.path))) {
                    if (visible.insert(*target).second) {
                        stack.push_back(*target);
                    }
                }
            }
        }
    }

    // Symbols visible to file `file_index`: itself, its direct imports, and the transitive
    // public-import closure of those imports.
    std::unordered_set<std::size_t> visible_files(std::size_t file_index) {
        std::unordered_set<std::size_t> visible{file_index};
        for (const auto& import : files()[file_index].imports) {
            // `import option` files (editions) define options/features only; their symbols are not
            // visible as field/extendee types, so they are excluded from type-resolution
            // visibility.
            if (import.kind == ImportKind::Option) {
                continue;
            }
            if (const auto target = import_index(canonical_import_path(import.path))) {
                visible.insert(*target);
                add_public_closure(*target, visible);
            }
        }
        return visible;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): name vs scope, distinct roles
    std::optional<std::string> resolve_name(const std::string& name, const std::string& scope,
                                            const std::unordered_set<std::size_t>& visible) const {
        const auto is_visible = [&](const std::string& fqn) {
            const auto it = m_symbols.find(fqn);
            return it != m_symbols.end() && visible.count(it->second.file_index) != 0;
        };
        if (!name.empty() && name.front() == '.') {  // already absolute
            return is_visible(name) ? std::optional<std::string>(name) : std::nullopt;
        }
        // Progressive scope-prefix search, innermost-out. For valid schemas this matches protobuf
        // resolution; it does NOT implement the full "first-component-binds" rule, so it can accept
        // some schemas protoc would reject -- a wrong-output-for-invalid-input case only (it resolves
        // to a real symbol or fails cleanly; never unsafe).
        std::string current = scope;
        for (;;) {
            std::string candidate = current;
            candidate += '.';
            candidate += name;
            if (is_visible(candidate)) {
                return candidate;
            }
            if (current.empty()) {
                break;
            }
            current = parent_scope(current);
        }
        return std::nullopt;
    }

    std::optional<Error> resolve_field(FieldNode& field, const std::string& scope,
                                       const std::unordered_set<std::size_t>& visible) {
        if (is_scalar_type(field.type_name)) {
            return std::nullopt;  // scalar: nothing to resolve
        }
        const auto fqn = resolve_name(field.type_name, scope, visible);
        if (!fqn) {
            std::string message = "unresolved type '";
            message += field.type_name;
            message += "' in ";
            message += scope;
            return Error{m_source, field.offset, std::move(message)};
        }
        field.resolved_type_fqn = *fqn;
        const SymbolKind kind = m_symbols.at(*fqn).kind;
        field.is_message_type = kind == SymbolKind::Message;
        field.is_enum_type = kind == SymbolKind::Enum;
        // Post-resolution fixup for user-defined types (the parser's parse-time normalization
        // could not know the type kind):
        if (field.is_message_type && field.presence == FieldPresence::Implicit) {
            field.presence = FieldPresence::Explicit;  // message-typed fields always have presence
        }
        if (field.is_repeated && (field.is_message_type || field.is_enum_type)) {
            field.repeated_encoding = RepeatedEncoding::Expanded;
        }
        return std::nullopt;
    }

    std::optional<Error> resolve_map(MapFieldNode& field, const std::string& scope,
                                     const std::unordered_set<std::size_t>& visible) {
        if (!is_valid_map_key_type(field.key_type)) {
            std::string message = "map key type '";
            message += field.key_type;
            message += "' is not allowed (a map key must be an integral or string scalar) in ";
            message += scope;
            return Error{m_source, field.offset, std::move(message)};
        }
        if (is_scalar_type(field.value_type)) {
            return std::
                nullopt;  // the key is a validated scalar; a scalar value needs no resolution
        }
        const auto fqn = resolve_name(field.value_type, scope, visible);
        if (!fqn) {
            std::string message = "unresolved map value type '";
            message += field.value_type;
            message += "' in ";
            message += scope;
            return Error{m_source, field.offset, std::move(message)};
        }
        field.resolved_value_type_fqn = *fqn;
        const SymbolKind kind = m_symbols.at(*fqn).kind;
        field.value_is_message = kind == SymbolKind::Message;
        field.value_is_enum = kind == SymbolKind::Enum;
        return std::nullopt;
    }

    std::optional<Error> resolve_extend(ExtendNode& extend, const std::string& scope,
                                        const std::unordered_set<std::size_t>& visible) {
        const auto extendee = resolve_name(extend.extendee_type_name, scope, visible);
        if (!extendee) {
            std::string message = "unresolved extendee '";
            message += extend.extendee_type_name;
            message += "'";
            return Error{m_source, extend.offset, std::move(message)};
        }
        if (m_symbols.at(*extendee).kind != SymbolKind::Message) {
            std::string message = "extendee '";
            message += extend.extendee_type_name;
            message += "' is not a message";
            return Error{m_source, extend.offset, std::move(message)};
        }
        for (auto& field : extend.fields) {
            if (auto err = resolve_field(field, scope, visible)) {
                return err;
            }
            // An extension number must be unique per extendee; protoc rejects a collision and it is
            // cheap to detect, so reject it here too rather than silently last-winning.
            if (m_table.extensions.count({*extendee, field.number}) != 0) {
                std::string message = "duplicate extension number ";
                message += std::to_string(field.number);
                message += " extending '";
                message += *extendee;
                message += "'";
                return Error{m_source, field.offset, std::move(message)};
            }
            m_table.extensions[{*extendee, field.number}] = &field;
        }
        return std::nullopt;
    }

    // Recurses into nested messages; the parser caps nesting depth (kMaxParseDepth), so input deep
    // enough to overflow this walk is rejected before analysis runs.
    std::optional<Error> resolve_message(MessageNode& message,
                                         const std::unordered_set<std::size_t>& visible) {
        const std::string& scope = message.fqn;  // contents resolve relative to this message
        for (auto& field : message.fields) {
            if (auto err = resolve_field(field, scope, visible)) {
                return err;
            }
        }
        for (auto& oneof : message.oneofs) {
            for (auto& field : oneof.fields) {
                if (auto err = resolve_field(field, scope, visible)) {
                    return err;
                }
            }
        }
        for (auto& field : message.map_fields) {
            if (auto err = resolve_map(field, scope, visible)) {
                return err;
            }
        }
        for (auto& nested : message.nested_messages) {
            if (auto err = resolve_message(nested, visible)) {
                return err;
            }
        }
        for (auto& extend : message.extends) {
            if (auto err = resolve_extend(extend, scope, visible)) {
                return err;
            }
        }
        return std::nullopt;
    }

    ResolvedFileSet* m_files;
    std::unordered_map<std::string, Sym> m_symbols;
    SymbolTable m_table;
    SourceId m_source;  // the file currently being resolved (for error attribution)
};

}  // namespace

void compute_fqns(FileNode& file) {
    // File scope = ".package" (or "" for no package), so a top-level type is ".package.Name"
    // (or ".Name").
    const std::string scope = file.package.empty() ? std::string{} : "." + file.package;
    for (auto& message : file.messages) {
        assign_message_fqn(message, scope);
    }
    for (auto& node : file.enums) {
        assign_enum_fqn(node, scope);
    }
    for (auto& extend : file.extends) {
        assign_extend_fqns(extend, scope);
    }
}

void compute_fqns(ResolvedFileSet& file_set) {
    for (auto& file : file_set.files) {
        compute_fqns(file);
    }
}

Result<SymbolTable> resolve_types(ResolvedFileSet& file_set) {
    return TypeResolver(file_set).run();
}

Result<SymbolTable> analyze(ResolvedFileSet& file_set) {
    resolve_features(file_set);              // editions features (no-op for proto2/proto3)
    compute_fqns(file_set);                  // FQN computation
    auto symbols = resolve_types(file_set);  // type resolution
    if (!symbols) {
        return std::move(symbols).error();
    }
    // Option interpretation: interpret_options only mutates fields in place (no reallocation), so
    // the extension FieldNode* pointers in `symbols` remain valid.
    if (auto interpreted = interpret_options(file_set); !interpreted) {
        return std::move(interpreted).error();
    }
    return std::move(symbols).value();
}

}  // namespace rapidproto
