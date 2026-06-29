#include "rapidproto/codegen/naming.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"

namespace rapidproto::codegen {

// A C++ identifier for a proto name: append `_` if it would collide with a keyword or one of the few
// generated members it could clash with. Generator-internal locals all use the reserved `rp_` prefix
// (so the single rule "any proto name starting with `rp_` is escaped" covers them without enumerating),
// and the streaming decoder references each field tag by its message-qualified name, so a field named
// like a decode() local no longer collides. The remaining non-keyword reservations are listed (with the
// clash each prevents) below.
std::string sanitize(std::string_view name) {
    static const std::unordered_set<std::string_view> kReserved = {
        "alignas",
        "alignof",
        "and",
        "and_eq",
        "bitand",
        "bitor",
        "compl",
        "not",
        "not_eq",
        "or",
        "or_eq",
        "xor",
        "xor_eq",
        "asm",
        "auto",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char16_t",
        "char32_t",
        "class",
        "const",
        "constexpr",
        "continue",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "nullptr",
        "operator",
        "private",
        "protected",
        "public",
        "register",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "struct",
        "switch",
        "template",
        "this",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "thread_local",
        // Non-keyword reservations, each a real clash a field of that name would cause:
        //  - Value/Key/kNumber/kName: a streaming tag `struct value { using Value = ...; }` etc. would
        //    redeclare the injected-class-name.
        //  - decode: the generated decode() method (a same-named nested tag / arena accessor clashes).
        //  - Callbacks: the streaming decode() template parameter pack; a nested tag of that name
        //    shadows it in the out-of-line definition.
        //  - m_bytes: arena stores field `bytes` as member `m_bytes`, which clashes with the accessor
        //    of a sibling field literally named `m_bytes`.
        // Every other emitted local is `rp_`-prefixed and each streaming tag is referenced by its
        // message-qualified name, so common field names (value, tag, status, reader, ...) stay free.
        "Value",
        "Key",
        "kNumber",
        "kName",
        "decode",
        "Callbacks",
        "m_bytes",
        // Common C/C++ MACROS: a field accessor or a (prefix-stripped) bare enum value of one of these
        // would macro-expand rather than compile (e.g. `EOF` -> `(-1)`). Not exhaustive -- a best-effort
        // guard for the names a SCREAMING_SNAKE enum value realistically hits; enum-prefix stripping
        // additionally refuses to strip any enum whose bare remainder is reserved here (see emit_enum).
        "EOF",
        "NULL",
        "NAN",
        "INFINITY",
        "ERROR",
        "TRUE",
        "FALSE",
        "BUFSIZ",
        "EXIT_SUCCESS",
        "EXIT_FAILURE",
        "RAND_MAX",
        "SEEK_SET",
        "SEEK_CUR",
        "SEEK_END",
        "errno",
        "stdin",
        "stdout",
        "stderr",
    };
    std::string out(name);
    // `rp_`-prefixed: any proto name beginning with the reserved generator-internal prefix. A
    // single trailing `_` makes it distinct from every emitted `rp_` local (which never end in
    // `_`).
    if (name.rfind("rp_", 0) == 0 || kReserved.count(name) != 0) {
        out += '_';
    }
    return out;
}

namespace {

// A resolved type FQN (".pkg.Outer.Inner") -> a fully `::`-rooted absolute C++ name
// ("::ns_prefix::pkg::Outer::Inner"), each component sanitized, under `ns_prefix` (an already
// `::`-joined C++ namespace, possibly empty). Used as the fallback for types not defined in the
// file being generated (imported / well-known): we cannot see their scope's dedup decisions, so we
// assume the plain sanitized name per component.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): prefix vs fqn, distinct roles
std::string fqn_to_absolute(std::string_view ns_prefix, std::string_view fqn) {
    std::string out = "::";
    std::string sep;  // "" before the first component, "::" between components
    const auto append = [&](std::string_view component) {
        out += sep;
        out += sanitize(component);
        sep = "::";
    };
    if (!ns_prefix.empty()) {
        out += ns_prefix;  // already sanitized + "::"-joined by namespace_of
        sep = "::";
    }
    std::string component;
    for (const char ch : fqn) {
        if (ch == '.') {
            if (!component.empty()) {
                append(component);
                component.clear();
            }
        } else {
            component += ch;
        }
    }
    if (!component.empty()) {
        append(component);
    }
    return out;
}

// Records a scope member's collision-free C++ id into `names`: sanitize the proto name, then append
// `_` until it is unique among `taken` (this scope's already-used identifiers). Returns the id.
const std::string& assign_id(CppNameTable& names, std::unordered_set<std::string>& taken,
                             const void* node, std::string_view raw) {
    std::string id = sanitize(raw);
    while (!taken.insert(id).second) {
        id += '_';
    }
    return names.local.emplace(node, std::move(id)).first->second;
}

void index_message(CppNameTable& names, const MessageNode& message, const std::string& abs) {
    std::unordered_set<std::string> taken;
    std::vector<std::pair<const MessageNode*, std::string>> children;
    for (const auto& nested_enum : message.enums) {
        names.absolute.emplace(
            nested_enum.fqn, abs + "::" + assign_id(names, taken, &nested_enum, nested_enum.name));
    }
    for (const auto& nested : message.nested_messages) {
        std::string child_abs = abs + "::" + assign_id(names, taken, &nested, nested.name);
        names.absolute.emplace(nested.fqn, child_abs);
        children.emplace_back(&nested, std::move(child_abs));
    }
    for (const auto& field : message.fields) {
        assign_id(names, taken, &field, field.name);
    }
    for (const auto& oneof : message.oneofs) {
        for (const auto& field : oneof.fields) {
            assign_id(names, taken, &field, field.name);
        }
    }
    for (const auto& map : message.map_fields) {
        assign_id(names, taken, &map, map.name);
    }
    for (const auto& [child, child_abs] : children) {
        index_message(names, *child, child_abs);
    }
}

// Index one file's namespace-scope members (top-level enums + messages, recursing into nested
// scopes) into `names`. Each file's namespace is its own dedup scope.
void index_file(CppNameTable& names, const FileNode& file) {
    const std::string ns = join_ns(names.ns_prefix, namespace_of(file.package));
    const std::string root = ns.empty() ? std::string() : "::" + ns;  // "::" + Local rejoins below
    // Top-level messages (the decoders) may sit in a per-model sub-namespace so the two models' types
    // coexist in one TU; top-level enums stay at package scope (shared, in the common header). Derive
    // it via the shared message_namespace() so these absolute names match the `namespace` a generator
    // opens for the decoder.
    const std::string msg_ns = message_namespace(names, file);
    const std::string msg_root = msg_ns.empty() ? std::string() : "::" + msg_ns;
    std::unordered_set<std::string> taken;
    std::vector<std::pair<const MessageNode*, std::string>> tops;
    for (const auto& node : file.enums) {
        names.absolute.emplace(node.fqn, root + "::" + assign_id(names, taken, &node, node.name));
    }
    for (const auto& message : file.messages) {
        std::string abs = msg_root + "::" + assign_id(names, taken, &message, message.name);
        names.absolute.emplace(message.fqn, abs);
        tops.emplace_back(&message, std::move(abs));
    }
    for (const auto& [message, abs] : tops) {
        index_message(names, *message, abs);
    }
}

}  // namespace

std::string namespace_of(std::string_view package) {
    std::string out;
    std::string component;
    const auto flush = [&] {
        if (!component.empty()) {
            if (!out.empty()) {
                out += "::";
            }
            out += sanitize(component);
            component.clear();
        }
    };
    for (const char ch : package) {
        if (ch == '.') {
            flush();
        } else {
            component += ch;
        }
    }
    flush();
    return out;
}

std::string join_ns(std::string_view a, std::string_view b) {
    if (a.empty()) {
        return std::string(b);
    }
    if (b.empty()) {
        return std::string(a);
    }
    return std::string(a) + "::" + std::string(b);
}

std::string message_namespace(const CppNameTable& names, const FileNode& file) {
    return join_ns(join_ns(names.ns_prefix, namespace_of(file.package)), names.model_namespace);
}

CppNameTable build_cpp_names(const FileNode& file, const std::vector<FileNode>& all_files,
                             std::string ns_prefix, std::string model_namespace) {
    CppNameTable names;
    names.ns_prefix = std::move(ns_prefix);
    names.model_namespace = std::move(model_namespace);
    if (all_files.empty()) {
        index_file(names, file);
    } else {
        for (const auto& dep : all_files) {  // includes `file`
            index_file(names, dep);
        }
    }
    return names;
}

std::string cpp_type_name(const CppNameTable& names, std::string_view fqn) {
    const auto found = names.absolute.find(std::string(fqn));
    // build_cpp_names indexes every type in the file set and resolve() guarantees every reference
    // lands in it, so the lookup hits for any schema we actually generate from. The fqn_to_absolute
    // fallback is defensive -- it synthesizes a name for an un-indexed FQN -- and so stays untested.
    return found != names.absolute.end() ? found->second : fqn_to_absolute(names.ns_prefix, fqn);
}

}  // namespace rapidproto::codegen
