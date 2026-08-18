#include "rapidproto/codegen/naming.hpp"

#include <cstddef>
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
namespace {

// Illegal or undefined as ANY C++ name, a namespace included -- so this is the half that also
// governs a --namespace-prefix component. The keywords, plus `std`.
const std::unordered_set<std::string_view>& cpp_reserved() {
    static const std::unordered_set<std::string_view> kSet = {
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
        // `std`: the one namespace generated code LOOKS UP unrooted (`std::int32_t`,
        // `std::string_view`, `std::optional`). Any proto name that becomes a C++ TYPE would shadow
        // it from inside the class -- a message or enum at any depth, a package component after the
        // first, a streaming field/map tag struct, an arena oneof-member tag struct. An arena
        // accessor named `std` would NOT: a nested-name-specifier considers only namespaces, types
        // and class templates, so a member function is skipped. It is escaped regardless, because
        // one reserved set serves every role. A PACKAGE called `std` is the worst case -- it would
        // emit `namespace std { ... }`, undefined behaviour per [namespace.std] that compiles
        // without a diagnostic.
        "std",
    };
    return kSet;
}

// Clash only with a generated MEMBER, so they are reserved for a proto name and for nothing else.
// They are ordinary words in every other role: a namespace called `decode` or `Value` compiles, and
// refusing one as a --namespace-prefix component rejected a name that works.
//
//  - Value/Key/kNumber/kName: a streaming tag `struct value { using Value = ...; }` etc. would
//    redeclare the injected-class-name.
//  - decode: the generated decode() method (a same-named nested tag / arena accessor clashes).
//
// REDECLARATION is the only mode here; SHADOWING (a name the output looks up unrooted) needs `std`,
// which is in cpp_reserved() above because it is illegal as a namespace too. `rp_dump_detail` is out
// of reach via the `rp_` rule, and the model roots are out of reach by position -- they sit ABOVE
// every package, so no proto name can reach them. Every other name the emitters introduce is
// `rp_`-prefixed -- including the template parameter packs (`rp_Callbacks`, `rp_Fs`) and the
// friend/tag aliases (`rp_T`, `rp_Tag`), which are named that way SO THAT they need no entry here.
// Reserve a name below only when the identifier is public API a user writes and so cannot take the
// prefix.
const std::unordered_set<std::string_view>& member_reserved() {
    static const std::unordered_set<std::string_view> kSet = {
        "Value", "Key", "kNumber", "kName", "decode",
    };
    return kSet;
}

}  // namespace

std::string sanitize(std::string_view name) {
    std::string out(name);
    // `rp_`-prefixed: any proto name beginning with the generator-internal prefix. A single trailing
    // `_` makes it distinct from every emitted `rp_` local (which never end in `_`).
    if (name.rfind("rp_", 0) == 0 || cpp_reserved().count(name) != 0 ||
        member_reserved().count(name) != 0 || expands_as_macro(name)) {
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

// A name for a resolved type FQN that is NOT in the name table. Deliberately one that cannot
// compile.
//
// There used to be a fallback here that synthesized `::<prefix>::<pkg>::<Type>` by sanitizing each
// component. It cannot be right any more, and could not be trusted before: the roots put messages
// and enums under DIFFERENT ones (`<prefix>::arena::<pkg>` vs `<prefix>::common::<pkg>`) and nothing
// in an FQN says which kind it names, so every synthesized name is wrong. Even pre-roots it could
// return an id belonging to a different real type -- names are deduped per package, so `.p.decode`
// synthesized `::p::decode_`, which is what a sibling `message decode_` actually holds.
//
// Reaching this is a generator bug (`resolve()` guarantees every reference is indexed), so the goal
// is to make that bug loud and traceable rather than to produce output. An undeclared identifier
// carrying the FQN gives a compile error at the exact use site, naming the type that went missing.
std::string unresolved_type_name(std::string_view fqn) {
    std::string out = "rp_type_not_in_name_table__";
    for (const char ch : fqn) {
        out += (ch == '.') ? '_' : ch;
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
// `enum_abs` is the same path as `abs` under the enums root; a swap shows up in every golden.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void index_message(CppNameTable& names, const MessageNode& message, const std::string& abs,
                   const std::string& enum_abs, const std::string& msg_ns,
                   const ClaimedIds& claimed) {
    std::unordered_set<std::string> taken;
    // Seed with the class's OWN name: C++ forbids a member with the same name as its class
    // ([class.mem]), so `message Outer { message Outer {} }` -- which protoc accepts -- would emit a
    // header that does not compile. The parent keeps its name; the child is the one deduped.
    taken.insert(names.local.at(&message));
    std::vector<std::pair<const MessageNode*, std::string>> children;
    // A nested enum's canonical home is the MIRROR under the common root, not this class: one C++
    // type both models alias, instead of a copy per model. The mirror reuses the ids assigned here,
    // so its path is the model path with the root swapped -- which is why `taken` still governs both
    // and why no second dedup pass exists.
    for (const auto& nested_enum : message.enums) {
        names.absolute.emplace(
            nested_enum.fqn,
            enum_abs + "::" + assign_id(names, taken, claimed, &nested_enum, nested_enum.name));
    }
    std::vector<std::string> child_enum_abs;
    for (const auto& nested : message.nested_messages) {
        const std::string id = assign_id(names, taken, claimed, &nested, nested.name);
        std::string child_abs = abs;
        child_abs.append("::").append(id);
        names.absolute.emplace(nested.fqn, child_abs);
        names.type_ns.emplace(nested.fqn, msg_ns);
        std::string child_enums = enum_abs;
        child_enums.append("::").append(id);
        child_enum_abs.push_back(std::move(child_enums));
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
    for (std::size_t i = 0; i < children.size(); ++i) {
        index_message(names, *children[i].first, children[i].second, child_enum_abs[i], msg_ns,
                      claimed);
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
    // `<prefix>::common::<pkg>` -- so a top-level type of any name can no longer collide with a
    // generator-invented segment: there is none inside the package scope any more.
    const std::string msg_ns = message_namespace(names, file);
    const std::string msg_root = "::" + msg_ns;
    const std::string enum_ns = enum_namespace(names, file);
    const std::string enum_root = "::" + enum_ns;
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
        // The mirror path for a top-level message: same id, common root instead of the model root.
        index_message(names, *message, abs, enum_root + "::" + names.local.at(message), msg_ns,
                      claimed);
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
            // No `rapidproto` special case: under the roots a package of that name lands at
            // `<prefix>::arena::rapidproto`, three levels below the runtime's `::rapidproto`, so it
            // cannot merge into it. The only component that could is the PREFIX, and that is
            // refused up front (ns_prefix_component_problem) rather than silently renamed.
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
    return join_ns(join_ns(names.ns_prefix, names.model_namespace), namespace_of(file.package));
}

std::string ns_prefix_component_problem(std::string_view component) {
    // Deliberately NOT `sanitize(component) != component`. sanitize()'s set is calibrated for proto
    // names, which become MEMBERS as well as namespaces, so deriving the rule from it refused
    // `decode`, `Value`, `Key`, `kNumber` and `kName` -- every one of which is a working namespace
    // name. A prefix is what the user asked their code to be called; refusing a name that compiles
    // is as wrong an answer as silently renaming one that does not.
    if (cpp_reserved().count(component) != 0) {
        return "it is a C++ keyword or `std`";
    }
    // The generator's own prefix, in both spellings: `rp_` names its internals (rp_dump_detail and
    // friends) and `RP_` its macros, so a prefix starting with either can collide with a name the
    // output introduces for itself. Checked BEFORE the macro rule, which also matches every `RP_`
    // name and would otherwise answer a prefix question with a claim about macro expansion.
    if (component.rfind("rp_", 0) == 0 || component.rfind("RP_", 0) == 0) {
        return "`rp_` and `RP_` are reserved for the generator's own names";
    }
    if (expands_as_macro(component)) {
        return "it expands as a macro rather than compiling as a name";
    }
    if (component == "rapidproto") {
        // A PACKAGE of this name is harmless under the roots (it lands at
        // `<prefix>::arena::rapidproto`), but a prefix component would put generated code inside a
        // namespace named like the runtime's own, where an unqualified `rapidproto::` in any
        // hand-written code near it resolves to the wrong one.
        return "it is the runtime's own namespace";
    }
    return {};
}

std::string effective_ns_prefix(std::string_view prefix) {
    return namespace_of(prefix.empty() ? kDefaultNsPrefix : prefix);
}

std::string enum_namespace(const CppNameTable& names, const FileNode& file) {
    return join_ns(join_ns(names.ns_prefix, kCommonRoot), namespace_of(file.package));
}

CppNameTable build_cpp_names(const FileNode& file, const std::vector<FileNode>& all_files,
                             std::string ns_prefix, std::string model_namespace) {
    CppNameTable names;
    // Substituted HERE as well as in the convenience overloads: this is a public entry point, and an
    // empty prefix would put the model roots -- `arena`, `stream`, `common` -- at global scope, which
    // is exactly what the CLI and the CMake helper refuse. A library caller should not be able to
    // reach a layout the tools forbid.
    names.ns_prefix = ns_prefix.empty() ? namespace_of(kDefaultNsPrefix) : std::move(ns_prefix);
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
    // lands in it, so the lookup hits for any schema we actually generate from. A miss is a bug in
    // this library, not bad input, and there is no correct name to fall back to -- see
    // unresolved_type_name.
    return found != names.absolute.end() ? found->second : unresolved_type_name(fqn);
}

std::string relative_type_name(const CppNameTable& names, std::string_view fqn) {
    std::string absolute = cpp_type_name(names, fqn);
    const auto it = names.type_ns.find(std::string(fqn));
    if (it == names.type_ns.end()) {
        return absolute;  // not a message (or an unresolved reference): nothing to strip.
    }
    const std::string head = "::" + it->second + "::";
    return absolute.rfind(head, 0) == 0 ? absolute.substr(head.size()) : absolute;
}

}  // namespace rapidproto::codegen
