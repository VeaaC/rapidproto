#include "rapidproto/codegen/naming.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
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
        "const_cast",
        "dynamic_cast",
        "reinterpret_cast",
        "static_assert",
        "static_cast",
        // C++20 keywords. The library targets C++17, but a GENERATED header is included in the
        // consumer's TU, which is commonly C++20 -- where a field named `concept` or `requires` is
        // a hard error rather than a warning. Escaping them costs nothing on C++17.
        "char8_t",
        "concept",
        "consteval",
        "constinit",
        "co_await",
        "co_return",
        "co_yield",
        "requires",
        // Non-keyword reservations, each a real clash a field of that name would cause:
        //  - Value/Key/kNumber/kName: a streaming tag `struct value { using Value = ...; }` etc. would
        //    redeclare the injected-class-name.
        //  - decode: the generated decode() method (a same-named nested tag / arena accessor clashes).
        //  - std: the one namespace generated code LOOKS UP unrooted (`std::int32_t`,
        //    `std::string_view`, `std::optional`). Any proto name that becomes a C++ TYPE would
        //    shadow it from inside the class -- a message or enum at any depth, a package component
        //    after the first, a streaming field/map tag struct, an arena oneof-member tag struct.
        //    An arena accessor named `std` would NOT: a nested-name-specifier considers only
        //    namespaces, types and class templates, so a member function is skipped. It is escaped
        //    regardless, because one reserved set serves every role. A PACKAGE called `std` is the
        //    worst case -- it would emit `namespace std { ... }`, undefined behaviour per
        //    [namespace.std] that compiles without a diagnostic.
        // The two modes are worth keeping apart: SHADOWING needs a name the output looks up
        // unrooted (only `std`), while REDECLARATION needs a name the output already defines at the
        // same scope. `rp_dump_detail` is out of reach via the `rp_` rule, and the model roots are
        // out of reach by position -- they sit ABOVE every package, so no proto name can reach them.
        // Every other name the emitters introduce is `rp_`-prefixed -- including the template
        // parameter packs (`rp_Callbacks`, `rp_Fs`) and the friend/tag aliases (`rp_T`, `rp_Tag`),
        // which are named that way SO THAT they need no entry here. Reserve a name below only when
        // the identifier is public API a user writes and so cannot take the prefix.
        "Value",
        "Key",
        "kNumber",
        "kName",
        "decode",
        "std",
    };
    std::string out(name);
    // `rp_`-prefixed: any proto name beginning with the generator-internal prefix. A single trailing
    // `_` makes it distinct from every emitted `rp_` local (which never end in `_`).
    if (name.rfind("rp_", 0) == 0 || kReserved.count(name) != 0 || expands_as_macro(name)) {
        out += '_';
    }
    return out;
}

// Would this identifier MACRO-EXPAND rather than compile? Split out from the reserved set above
// because it is the only part that applies to names sanitize() never sees: arenagen synthesizes its
// oneof visit-tag struct from the proto name, and a struct called `EOF` expands to `(-1)`.
//
// A macro fires wherever the token appears, so unlike the rest of sanitize() this cannot be narrowed
// by role -- but it must not be WIDENED either. The rest of the reserved set holds names that are
// perfectly legal as a tag struct (`Value`, `Key`, `decode`), and escaping those renames the public
// tag struct of 77 corpus schemas for no compile benefit. Hence two sets, not one.
bool expands_as_macro(std::string_view name) {
    // The runtime's own `RP_`-prefixed macros (`RP_FLATTEN`, `RP_NOINLINE`) by prefix, so the rule
    // holds for whatever it adds later, plus a list of common C/C++ ones.
    //
    // The list is a portability guard, not a description of this toolchain: a generated header is
    // included in the CONSUMER's TU, so what matters is every macro THEY may have defined first.
    // `ERROR`/`TRUE`/`FALSE` come in via windows.h, `NAN`/`INFINITY` via <cmath> -- none is defined
    // by anything rapidproto includes, and all five stay. Deliberately not exhaustive in the other
    // direction either (<cerrno>, <climits> and the GNU predefined `linux`/`unix` are not here); a
    // list can only ever cover the names a SCREAMING_SNAKE enum value realistically hits.
    // Enum-prefix stripping additionally refuses to strip any enum whose bare remainder lands here
    // (see emit_enum), so one `*_ERROR` value keeps its whole enum unstripped.
    static const std::unordered_set<std::string_view> kMacros = {
        "EOF",      "NULL",     "NAN",          "INFINITY",     "ERROR",    "TRUE",
        "FALSE",    "BUFSIZ",   "EXIT_SUCCESS", "EXIT_FAILURE", "RAND_MAX", "SEEK_SET",
        "SEEK_CUR", "SEEK_END", "errno",        "stdin",        "stdout",   "stderr",
    };
    return name.rfind("RP_", 0) == 0 || kMacros.count(name) != 0;
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
// Ids claimed ahead of the main pass, by node. Deliberately its own map rather than a lookup into
// `names.local`: that one holds every scope's ids, so keying off it would let a future pre-claim of
// a NESTED or member node bypass that scope's own dedup without anything noticing.
using ClaimedIds = std::unordered_map<const void*, std::string>;

const std::string& assign_id(CppNameTable& names, std::unordered_set<std::string>& taken,
                             const ClaimedIds& claimed, const void* node, std::string_view raw) {
    // Claimed by the unescaped-names pass: keep that id rather than deduping it against the entry
    // it made itself.
    if (const auto found = claimed.find(node); found != claimed.end()) {
        return names.local.emplace(node, found->second).first->second;
    }
    std::string id = sanitize(raw);
    while (!taken.insert(id).second) {
        id += '_';
    }
    return names.local.emplace(node, std::move(id)).first->second;
}

// `msg_ns` is the namespace the whole top-level scope is emitted in; a nested message is a CLASS
// member, so it shares it verbatim (see CppNameTable::type_ns).
// `abs` is a full `::a::b::C` type name, `msg_ns` a bare `a::b` namespace -- distinct shapes, and both
// are internal to this file's single recursion.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void index_message(CppNameTable& names, const MessageNode& message, const std::string& abs,
                   const std::string& msg_ns, const ClaimedIds& claimed) {
    std::unordered_set<std::string> taken;
    // Seed with the class's OWN name: C++ forbids a member with the same name as its class
    // ([class.mem]), so `message Outer { message Outer {} }` -- which protoc accepts -- would emit a
    // header that does not compile. The parent keeps its name; the child is the one deduped.
    taken.insert(names.local.at(&message));
    std::vector<std::pair<const MessageNode*, std::string>> children;
    for (const auto& nested_enum : message.enums) {
        names.absolute.emplace(
            nested_enum.fqn,
            abs + "::" + assign_id(names, taken, claimed, &nested_enum, nested_enum.name));
    }
    for (const auto& nested : message.nested_messages) {
        std::string child_abs = abs + "::" + assign_id(names, taken, claimed, &nested, nested.name);
        names.absolute.emplace(nested.fqn, child_abs);
        names.type_ns.emplace(nested.fqn, msg_ns);
        children.emplace_back(&nested, std::move(child_abs));
    }
    for (const auto& field : message.fields) {
        assign_id(names, taken, claimed, &field, field.name);
    }
    for (const auto& oneof : message.oneofs) {
        // The oneof itself gets an id too: arenagen emits a reader method named after it, and that
        // shares the class scope with everything else here -- including the class's own name, which
        // `message config { oneof config { ... } }` (protoc-valid) would otherwise collide with.
        assign_id(names, taken, claimed, &oneof, oneof.name);
        for (const auto& field : oneof.fields) {
            assign_id(names, taken, claimed, &field, field.name);
        }
    }
    for (const auto& map : message.map_fields) {
        assign_id(names, taken, claimed, &map, map.name);
    }
    for (const auto& [child, child_abs] : children) {
        index_message(names, *child, child_abs, msg_ns, claimed);
    }
}

// Dedup scopes for namespace-scope names, keyed by the PACKAGE namespace and shared by every file
// in it. Not per file: the namespace these names land in is per package, so two files sharing one
// were deduped independently and an escape in one could take a name a sibling file legitimately
// uses -- `enum decode` -> `decode_` beside a real `message decode_` -- which collides in any TU
// that includes both headers.
using TakenByNamespace = std::unordered_map<std::string, std::unordered_set<std::string>>;

// First pass over the file set: every top-level name that sanitize() leaves ALONE claims its id
// before any escaped name is placed.
//
// Two reasons. A user's literal identifier keeps its spelling -- `message decode_` stays `decode_`
// and the escape from `enum decode` moves to `decode__`, rather than the other way round. And an
// escape stops depending on the order files are indexed in: without this, `x.proto y.proto` and
// `y.proto x.proto` gave the same schema different C++ names, because whoever was indexed first
// took the contested id. Escapes cannot collide with each OTHER -- sanitize() appends one `_`, so
// two distinct proto names reach one id only when one of them already IS that id, and this pass
// claims it -- and two identical names in a package are rejected before codegen.
//
// What this does NOT settle: two distinct PACKAGES that sanitize to one C++ namespace (`p.decode`
// and `p.decode_`, or `std` and `std_`) put two literal names in one scope, which is decided here
// by iteration order. Both spellings compile either way -- the loser takes a `_` -- so the output
// is valid but not order-stable. Fixing it means tie-breaking on the proto FQN, or rejecting the
// aliasing outright, which is a schema-level diagnostic rather than a naming one.
void claim_unescaped_toplevel(const CppNameTable& names, const FileNode& file,
                              TakenByNamespace& taken_by_ns, ClaimedIds& claimed) {
    std::unordered_set<std::string>& taken =
        taken_by_ns[join_ns(names.ns_prefix, namespace_of(file.package))];
    const auto claim = [&](const void* node, const std::string& raw) {
        std::string id = sanitize(raw);
        if (id != raw || !taken.insert(id).second) {
            return;  // needs an escape, or an identical name already claimed it: second pass
        }
        claimed.emplace(node, std::move(id));
    };
    for (const auto& node : file.enums) {
        claim(&node, node.name);
    }
    for (const auto& message : file.messages) {
        claim(&message, message.name);
    }
}

// Index one file's namespace-scope members (top-level enums + messages, recursing into nested
// scopes) into `names`.
void index_file(CppNameTable& names, const FileNode& file, TakenByNamespace& taken_by_ns,
                const ClaimedIds& claimed) {
    // Messages and enums are emitted under DIFFERENT roots -- `<prefix>::<model>::<pkg>` and
    // `<prefix>::enums::<pkg>` -- so a top-level type of any name can no longer collide with a
    // generator-invented segment: there is none inside the package scope any more.
    const std::string msg_ns = message_namespace(names, file);
    const std::string msg_root = msg_ns.empty() ? std::string() : "::" + msg_ns;
    const std::string enum_ns = enum_namespace(names, file);
    const std::string enum_root = enum_ns.empty() ? std::string() : "::" + enum_ns;
    // ONE dedup scope for both, keyed on the PACKAGE namespace alone -- never on the root. The two
    // roots are separate C++ scopes, so a shared set is stricter than C++ requires; that is
    // deliberate. It keeps ids model-independent (the common header is shared, and ids differing
    // between the arena and streaming runs would make the two commons disagree), and it keeps an
    // enum's id equal to the one the model's alias will use, so the two spellings differ only in
    // their root segment.
    const std::string pkg_ns = join_ns(names.ns_prefix, namespace_of(file.package));
    std::unordered_set<std::string>& taken = taken_by_ns[pkg_ns];
    std::vector<std::pair<const MessageNode*, std::string>> tops;
    for (const auto& node : file.enums) {
        names.absolute.emplace(
            node.fqn, enum_root + "::" + assign_id(names, taken, claimed, &node, node.name));
    }
    for (const auto& message : file.messages) {
        std::string abs =
            msg_root + "::" + assign_id(names, taken, claimed, &message, message.name);
        names.absolute.emplace(message.fqn, abs);
        names.type_ns.emplace(message.fqn, msg_ns);
        tops.emplace_back(&message, std::move(abs));
    }
    for (const auto& [message, abs] : tops) {
        index_message(names, *message, abs, msg_ns, claimed);
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
            // `rapidproto` is escaped HERE rather than in sanitize(), because it only clashes as
            // a NAMESPACE: a package of that name merges the schema's own types into the runtime's,
            // where any sharing a name with something the runtime declares (`wire`, `Arena`,
            // `ByteView`, `WireType`, `dump`, `ArrayView`, ... -- not a closed list) redeclares it.
            // A message or field called `rapidproto` sits in the schema's own namespace, so
            // reserving it outright would rename working API in every schema that has a package.
            // It is NOT unreachable: with no package at all the type lands at global scope beside
            // the runtime's own namespace, and `message rapidproto` there is a redeclaration
            // today. That case wants its own fix (the reservation would have to be conditional on
            // the package being empty), not a blanket rename.
            const std::string id = sanitize(component);
            out += id == "rapidproto" ? "rapidproto_" : id;
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
    return join_ns(join_ns(names.ns_prefix, names.model_namespace), namespace_of(file.package));
}

std::string effective_ns_prefix(std::string_view prefix) {
    return namespace_of(prefix.empty() ? kDefaultNsPrefix : prefix);
}

std::string enum_namespace(const CppNameTable& names, const FileNode& file) {
    return join_ns(join_ns(names.ns_prefix, kEnumsRoot), namespace_of(file.package));
}

CppNameTable build_cpp_names(const FileNode& file, const std::vector<FileNode>& all_files,
                             std::string ns_prefix, std::string model_namespace) {
    CppNameTable names;
    names.ns_prefix = std::move(ns_prefix);
    names.model_namespace = std::move(model_namespace);
    // One set of dedup scopes for the whole file set, so files sharing a package see each other's
    // ids. Iteration order therefore decides who keeps a contested name and who gets the `_`; the
    // resolved file set is in a fixed order, and every generator run indexes the same set, so the
    // ids are stable across files and across models.
    TakenByNamespace taken_by_ns;
    ClaimedIds claimed;
    if (all_files.empty()) {
        claim_unescaped_toplevel(names, file, taken_by_ns, claimed);
        index_file(names, file, taken_by_ns, claimed);
    } else {
        for (const auto& dep : all_files) {  // includes `file`
            claim_unescaped_toplevel(names, dep, taken_by_ns, claimed);
        }
        for (const auto& dep : all_files) {
            index_file(names, dep, taken_by_ns, claimed);
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
