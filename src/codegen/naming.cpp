#include "rapidproto/codegen/naming.hpp"

#include <algorithm>
#include <cctype>
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
    // direction (<cerrno> and <climits> are not here); a list can only ever cover the names a
    // SCREAMING_SNAKE enum value realistically hits.
    //
    // Two groups are NOT a probability call like the rest:
    //  - <cstdint>'s macros: every generated header includes <cstdint> ITSELF, so these break
    //    unconditionally -- no consumer include order avoids them. The limit macros
    //    (`INT32_MAX`, `SIZE_MAX`, ...) expand anywhere; the function-like `INT8_C(x)` family
    //    expands wherever the name is followed by `(` -- which is exactly what an accessor
    //    declaration (`std::int32_t INT8_C() const`) and a generated constructor are. Both
    //    groups are listed in full.
    //  - `linux` and `unix`: gcc and clang predefine them under GNU extensions, which is the
    //    DEFAULT when a consumer passes no `-std`. They are lowercase, so an ordinary package or
    //    field of that name hits them -- `namespace linux` and `int linux;` both become
    //    `namespace 1` and `int 1;`.
    // Enum-prefix stripping additionally refuses to strip any enum whose bare remainder lands here
    // (see emit_enum), so one `*_ERROR` value keeps its whole enum unstripped.
    static const std::unordered_set<std::string_view> kMacros = {
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
        "linux",
        "unix",
        // <cstdint>, included by every generated header (see above).
        "INT8_MIN",
        "INT16_MIN",
        "INT32_MIN",
        "INT64_MIN",
        "INT8_MAX",
        "INT16_MAX",
        "INT32_MAX",
        "INT64_MAX",
        "UINT8_MAX",
        "UINT16_MAX",
        "UINT32_MAX",
        "UINT64_MAX",
        "INT_LEAST8_MIN",
        "INT_LEAST16_MIN",
        "INT_LEAST32_MIN",
        "INT_LEAST64_MIN",
        "INT_LEAST8_MAX",
        "INT_LEAST16_MAX",
        "INT_LEAST32_MAX",
        "INT_LEAST64_MAX",
        "UINT_LEAST8_MAX",
        "UINT_LEAST16_MAX",
        "UINT_LEAST32_MAX",
        "UINT_LEAST64_MAX",
        "INT_FAST8_MIN",
        "INT_FAST16_MIN",
        "INT_FAST32_MIN",
        "INT_FAST64_MIN",
        "INT_FAST8_MAX",
        "INT_FAST16_MAX",
        "INT_FAST32_MAX",
        "INT_FAST64_MAX",
        "UINT_FAST8_MAX",
        "UINT_FAST16_MAX",
        "UINT_FAST32_MAX",
        "UINT_FAST64_MAX",
        "INTPTR_MIN",
        "INTPTR_MAX",
        "UINTPTR_MAX",
        "INTMAX_MIN",
        "INTMAX_MAX",
        "UINTMAX_MAX",
        "PTRDIFF_MIN",
        "PTRDIFF_MAX",
        "SIZE_MAX",
        "SIG_ATOMIC_MIN",
        "SIG_ATOMIC_MAX",
        "WCHAR_MIN",
        "WCHAR_MAX",
        "WINT_MIN",
        "WINT_MAX",
        "INT8_C",
        "INT16_C",
        "INT32_C",
        "INT64_C",
        "INTMAX_C",
        "UINT8_C",
        "UINT16_C",
        "UINT32_C",
        "UINT64_C",
        "UINTMAX_C",
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
// Ids claimed ahead of the main pass, by node. Only TOP-LEVEL nodes are ever claimed
// (claim_toplevel_ids collects file->enums and file->messages and nothing else), so only
// index_file consults this map; the per-class member pass below never can hit it and does not
// take it. Deliberately its own map rather than a lookup into `names.local`: that one holds every
// scope's ids, so keying off it would let a claim bypass a scope's own dedup without notice.
using ClaimedIds = std::unordered_map<const void*, std::string>;

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

// index_file's variant: a top-level node keeps the id claim_toplevel_ids assigned it, rather than
// deduping it against the entry it made itself.
const std::string& assign_toplevel_id(CppNameTable& names, std::unordered_set<std::string>& taken,
                                      const ClaimedIds& claimed, const void* node,
                                      std::string_view raw) {
    if (const auto found = claimed.find(node); found != claimed.end()) {
        return names.local.emplace(node, found->second).first->second;
    }
    return assign_id(names, taken, node, raw);
}

// `msg_ns` is the namespace the whole top-level scope is emitted in; a nested message is a CLASS
// member, so it shares it verbatim (see CppNameTable::type_ns).
// `abs` is a full `::a::b::C` type name, `msg_ns` a bare `a::b` namespace -- distinct shapes, and both
// are internal to this file's single recursion.
// `enum_abs` is the same path as `abs` under the common root; a swap shows up in every golden.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void index_message(CppNameTable& names, const MessageNode& message, const std::string& abs,
                   const std::string& enum_abs, const std::string& msg_ns) {
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
            enum_abs + "::" + assign_id(names, taken, &nested_enum, nested_enum.name));
    }
    std::vector<std::string> child_enum_abs;
    for (const auto& nested : message.nested_messages) {
        const std::string id = assign_id(names, taken, &nested, nested.name);
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
        assign_id(names, taken, &field, field.name);
    }
    for (const auto& oneof : message.oneofs) {
        // The oneof itself gets an id too: arenagen emits a reader method named after it, and that
        // shares the class scope with everything else here -- including the class's own name, which
        // `message config { oneof config { ... } }` (protoc-valid) would otherwise collide with.
        assign_id(names, taken, &oneof, oneof.name);
        for (const auto& field : oneof.fields) {
            assign_id(names, taken, &field, field.name);
        }
    }
    for (const auto& map : message.map_fields) {
        assign_id(names, taken, &map, map.name);
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        index_message(names, *children[i].first, children[i].second, child_enum_abs[i], msg_ns);
    }
}

// Dedup scopes for namespace-scope names, keyed by the PACKAGE namespace and shared by every file
// in it. Not per file: the namespace these names land in is per package, so two files sharing one
// were deduped independently and an escape in one could take a name a sibling file legitimately
// uses -- `enum decode` -> `decode_` beside a real `message decode_` -- which collides in any TU
// that includes both headers.
using TakenByNamespace = std::unordered_map<std::string, std::unordered_set<std::string>>;

// Group 1 of claim_toplevel_ids: one file's package components, each claimed in its PARENT
// package's scope.
void claim_package_components(const CppNameTable& names, const FileNode& file,
                              TakenByNamespace& taken_by_ns) {
    std::string parent;  // the dotted package prefix seen so far ("" = global scope)
    std::string component;
    const auto claim_component = [&] {
        if (component.empty()) {
            return;
        }
        taken_by_ns[join_ns(names.ns_prefix, namespace_of(parent))].insert(sanitize(component));
        if (!parent.empty()) {
            parent += '.';
        }
        parent += component;
        component.clear();
    };
    for (const char ch : file.package) {
        if (ch == '.') {
            claim_component();
        } else {
            component += ch;
        }
    }
    claim_component();
}

// Claim every namespace-scope id in the file set, up front and in ONE deterministic order:
//
//   1. PACKAGE COMPONENTS, into their parent package's scope. A child package is a namespace in
//      the parent's C++ scope whose id comes from sanitize() alone -- every file naming it must
//      agree on it -- so it can never yield. A top-level type whose sanitized id lands on it must
//      be the one to move: `package a.linux` opens `namespace linux_` inside `a`, where a sibling
//      file's `message linux_` (protoc-valid -- the proto names differ) would otherwise redeclare
//      that namespace as a class under every model root, and its enum mirror would merge into the
//      package's namespace in the common header, colliding enum by enum. A RAW collision (type
//      and package literally sharing a proto name) is rejected by protoc, so for protoc-valid
//      input a package blocks a type only when sanitize() itself created the alias.
//   2. LITERALS -- names sanitize() leaves alone -- so a user's spelling wins wherever possible:
//      `message decode_` keeps `decode_` and the escape from `enum decode` moves to `decode__`,
//      never the other way round.
//   3. Literals a package component BLOCKED, escalated next -- still ahead of every escape.
//   4. ESCAPES -- names sanitize() changed -- escalated until free.
//
// Within each group the order is the proto FQN, a property of the schema -- so the ids cannot
// depend on the order files arrive in. Only groups 3 and 4 ever contest an id: two literals reach
// one id only as identical names in one package (rejected before codegen), and sanitize() appends
// a single `_`, so an escape meets an escape the same way. index_file below then finds every
// top-level node pre-claimed; nested scopes keep their own per-class dedup.
//
// What this deliberately does NOT settle: two distinct PACKAGES that sanitize to one C++
// namespace (`p.decode` and `p.decode_`) put two literal names in one scope and simply merge.
// Both spellings compile -- the colliding TYPES inside are deduped here like any others -- so
// rejecting the aliasing would be a schema-level diagnostic, not a naming concern.
void claim_toplevel_ids(const CppNameTable& names, const std::vector<const FileNode*>& files,
                        TakenByNamespace& taken_by_ns, ClaimedIds& claimed) {
    // 1. Package components.
    for (const FileNode* file : files) {
        claim_package_components(names, *file, taken_by_ns);
    }

    // One entry per top-level type, FQN-sorted once for groups 2-4.
    struct Claimant {
        const void* node;
        const std::string* raw;
        const std::string* fqn;
        std::unordered_set<std::string>* taken;
    };
    std::vector<Claimant> literals;
    std::vector<Claimant> escapes;
    for (const FileNode* file : files) {
        std::unordered_set<std::string>& taken =
            taken_by_ns[join_ns(names.ns_prefix, namespace_of(file->package))];
        const auto collect = [&](const void* node, const std::string& raw, const std::string& fqn) {
            (sanitize(raw) == raw ? literals : escapes).push_back({node, &raw, &fqn, &taken});
        };
        for (const auto& node : file->enums) {
            collect(&node, node.name, node.fqn);
        }
        for (const auto& message : file->messages) {
            collect(&message, message.name, message.fqn);
        }
    }
    const auto by_fqn = [](const Claimant& a, const Claimant& b) { return *a.fqn < *b.fqn; };
    std::sort(literals.begin(), literals.end(), by_fqn);
    std::sort(escapes.begin(), escapes.end(), by_fqn);

    // 2. Literals; a package component may hold one's id (see group 1), deferring it to group 3.
    std::vector<Claimant> blocked;
    for (const Claimant& entry : literals) {
        if (entry.taken->insert(*entry.raw).second) {
            claimed.emplace(entry.node, *entry.raw);
        } else {
            blocked.push_back(entry);
        }
    }
    // 3. Blocked literals, then 4. escapes: from the sanitized id, escalate with `_` until free.
    // The starting id must be sanitize(raw) itself, NOT one step past it: an UNCONTESTED escape
    // takes `sanitize(raw)` (`enum decode` alone is `decode_`, exactly as inside a class), and a
    // blocked literal's sanitize(raw) == raw is already in `taken`, so the loop escalates it.
    for (const std::vector<Claimant>* group : {&blocked, &escapes}) {
        for (const Claimant& entry : *group) {
            std::string id = sanitize(*entry.raw);
            while (!entry.taken->insert(id).second) {
                id += '_';
            }
            claimed.emplace(entry.node, std::move(id));
        }
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
            node.fqn,
            enum_root + "::" + assign_toplevel_id(names, taken, claimed, &node, node.name));
    }
    for (const auto& message : file.messages) {
        std::string abs =
            msg_root + "::" + assign_toplevel_id(names, taken, claimed, &message, message.name);
        names.absolute.emplace(message.fqn, abs);
        names.type_ns.emplace(message.fqn, msg_ns);
        tops.emplace_back(&message, std::move(abs));
    }
    for (const auto& [message, abs] : tops) {
        // The mirror path for a top-level message: same id, common root instead of the model root.
        index_message(names, *message, abs, enum_root + "::" + names.local.at(message), msg_ns);
    }
}

}  // namespace

namespace {

// Split `dotted` on '.', run each component through `escape`, join with "::".
template <typename Escape>
std::string dotted_to_ns(std::string_view dotted, Escape escape) {
    std::string out;
    std::string component;
    const auto flush = [&] {
        if (!component.empty()) {
            if (!out.empty()) {
                out += "::";
            }
            out += escape(component);
            component.clear();
        }
    };
    for (const char ch : dotted) {
        if (ch == '.') {
            flush();
        } else {
            component += ch;
        }
    }
    flush();
    return out;
}

// The escape for a --namespace-prefix component. A prefix is an INSTRUCTION, vetted by
// ns_prefix_component_problem at the CLI, so a component that validation accepts must be emitted
// VERBATIM -- the member-reserved words (`decode`, `Value`, ...) clash only with generated class
// members and are working namespace names, and running them through sanitize() here silently
// renamed exactly what the validation had just accepted (`--namespace-prefix=Value` emitted
// `namespace Value_`). The rules that remain are a PARTIAL safety net for LIBRARY callers, who
// reach this without the CLI's validation: keywords, `std`, macro names and `rp_` take the `_`,
// but reserved identifiers (`__x`, `_X...`) and `rapidproto` pass through verbatim -- only the
// CLI refuses those, so a library caller owns the validity of what it passes.
std::string sanitize_prefix_component(const std::string& name) {
    std::string out(name);
    if (name.rfind("rp_", 0) == 0 || cpp_reserved().count(name) != 0 || expands_as_macro(name)) {
        out += '_';
    }
    return out;
}

}  // namespace

std::string namespace_of(std::string_view package) {
    // No `rapidproto` special case: under the roots a package of that name lands at
    // `<prefix>::arena::rapidproto`, three levels below the runtime's `::rapidproto`, so it
    // cannot merge into it. The only component that could is the PREFIX, and that is
    // refused up front (ns_prefix_component_problem) rather than silently renamed.
    return dotted_to_ns(package, [](const std::string& component) { return sanitize(component); });
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

std::string ns_prefix_component_problem(std::string_view component, bool first) {
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
    // Reserved to the implementation in every scope: `__x`, and `_X` with a capital. Some are
    // ordinary identifiers, but the predefined macros live here too (`--namespace-prefix=__LINE__`
    // emitted `namespace __LINE__::common::pkg`, which cannot compile), and no list would keep up
    // with a toolchain's spellings. A proto NAME of this shape is data and stays sanitize()'s
    // business; a prefix is an instruction, so it is refused.
    if (component.rfind("__", 0) == 0 ||
        (component.size() >= 2 && component[0] == '_' &&
         std::isupper(static_cast<unsigned char>(component[1])) != 0)) {
        return "identifiers starting with `__` or `_` + a capital are reserved to the compiler";
    }
    // The FIRST component becomes a global-namespace name, and [lex.name] reserves EVERY
    // `_`-initial identifier there -- so `_x` is refused in that position and accepted after a dot
    // (`my._x` is fine). The rule above covers the spellings reserved in all scopes; this one is
    // the position-dependent remainder.
    if (first && !component.empty() && component[0] == '_') {
        return "a leading `_` is reserved in the global namespace, where the first prefix "
               "component lands";
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
    return dotted_to_ns(prefix.empty() ? kDefaultNsPrefix : prefix, sanitize_prefix_component);
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
    std::vector<const FileNode*> file_set;
    if (all_files.empty()) {
        file_set.push_back(&file);
    } else {
        for (const auto& dep : all_files) {  // includes `file`
            file_set.push_back(&dep);
        }
    }
    claim_toplevel_ids(names, file_set, taken_by_ns, claimed);
    for (const FileNode* dep : file_set) {
        index_file(names, *dep, taken_by_ns, claimed);
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
