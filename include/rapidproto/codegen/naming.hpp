#pragma once

// C++ name model shared by both C++ code generators (streamgen and arenagen): maps every resolved
// proto type and nameable struct member to a collision-free C++ identifier, in one pre-pass over the
// resolved file set. This is the language-specific naming layer the generators build on; it is
// intentionally separate from the resolver's own `rapidproto::SymbolTable` (FQN -> kind),
// which is language-agnostic and which this layer does not use.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rapidproto/ast.hpp"

namespace rapidproto::codegen {

// Resolves every message/enum and every nameable struct member to a collision-free C++ identifier,
// so declarations and references always agree. Two hazards motivate it:
//   1. A namespace-relative type reference can be shadowed by a sibling and silently bind to the
//      wrong type -> we emit fully `::`-rooted absolute names (`absolute`).
//   2. Distinct proto names can sanitize to one C++ identifier (e.g. sibling fields `read` and
//      `read_`, or a nested type `int` beside a field `int_`) and redefine a struct -> each scope
//      dedups its members, appending `_` until unique (`local`).
// Within a message's struct scope the members are: nested enums, nested messages, field tags,
// oneofs (their reader shares the scope), and map tags. Nested types are assigned first so their
// names stay plain-sanitized where possible, keeping absolute references stable.
struct CppNameTable {
    std::unordered_map<std::string, std::string> absolute;  // type fqn -> "::a::b::Local"
    std::unordered_map<const void*, std::string> local;     // member node -> unqualified C++ id
    // MESSAGE fqn -> the C++ namespace that message is emitted in, i.e. exactly the
    // `message_namespace()` of its defining file (a nested message is a class, so it shares its
    // top-level parent's namespace). Recorded here because it cannot be recovered from the FQN
    // afterwards: nothing in `a.b.C` marks where the package stops and message nesting starts. A
    // generator emitting a cross-file QUALIFIED call needs the callee's namespace exactly -- guessing
    // it breaks when a package segment collides with a message name.
    std::unordered_map<std::string, std::string> type_ns;
    std::string ns_prefix;  // C++ namespace prefix prepended to every root (never empty)
    // Per-model ROOT segment for this table's messages -- "arena" or "stream" -- sitting BETWEEN the
    // prefix and the package: `<prefix>::<model>::<pkg>::Msg`. A root rather than a suffix so no
    // generator-invented name lands inside the user's package scope, where a top-level type of that
    // name would collide with it. Enums are never under it: they are shared, and live at
    // `<prefix>::common::<pkg>` (see kCommonRoot). Must be a single valid C++ identifier -- it is
    // concatenated verbatim, not run through namespace_of.
    std::string model_namespace;
};

// Build the name table for the resolved set. `all_files` is the whole set (a file's imports precede
// it); indexing every file means a cross-file type reference resolves to the imported scope's
// dedup-stable absolute name.
//
// Top-level names are deduplicated per PACKAGE across the whole set, because that is the scope they
// are emitted into -- so a reserved-name escape in one file cannot take an identifier another file
// of the same package really uses. Two consequences worth knowing at the call site: an id can be
// ESCAPED because of a name in a different file, and adding a file to the set can therefore change
// a sibling's id -- including one an earlier run already emitted, so a split generation over one
// package can leave headers that disagree. Package components claim their ids first, then literal
// identifiers, so a literal keeps its spelling unless a package component took it -- in which case
// the blocked literal escalates like an escape. Member scopes (nested types,
// fields, oneofs, map entries) are unchanged: they dedup per message, first-come. When `all_files` is empty, only `file` is indexed (the single-file
// convenience path, valid when `file` has no cross-file type references). `ns_prefix` is an already
// `::`-joined C++ namespace (see `namespace_of`), never empty. `model_namespace` is the model ROOT
// (`kArenaRoot` / `kStreamRoot`) that messages sit under, between the prefix and the package.
//
// `model_namespace` has NO default on purpose. It used to default to "arena's" empty string, and a
// caller that forgot it silently produced rootless names that collide with protoc and with a
// consumer's own scope -- a wrong answer rather than a diagnostic. Making it explicit turns that
// mistake into a compile error. Built ONCE per set and reused for every file's `generate_header`.
CppNameTable build_cpp_names(const FileNode& file, const std::vector<FileNode>& all_files,
                             std::string ns_prefix, std::string model_namespace);

// A resolved type FQN -> its absolute C++ name, from the table built for the whole resolved set.
// `resolve()` guarantees every reference is indexed, so a miss is a bug in this library rather than
// bad input -- and there is no correct name to guess, since messages and enums sit under different
// roots and an FQN does not say which it is. A miss therefore yields a deliberately undeclared
// identifier naming the FQN, so the bug surfaces as a compile error at the use site instead of a
// silently wrong reference. Callers pass FQNs straight from the AST (leading-dot form, `.pkg.Name`).
std::string cpp_type_name(const CppNameTable& names, std::string_view fqn);

// A message's C++ name RELATIVE to the namespace it is emitted in ("Outer::Inner"): the absolute
// name with its namespace head removed. For diagnostics emitted once per FIELD, where the absolute
// head is identical for every message in the file and the nested path already says which message --
// spelling it in full there costs a copy of `<prefix>::<model>::<package>::` per field guard, which
// on a large schema is megabytes of header. Message-level diagnostics keep the absolute name, which
// a reader may need to tell two same-named messages apart.
std::string relative_type_name(const CppNameTable& names, std::string_view fqn);

// proto package "a.b.c" -> C++ namespace "a::b::c" (empty package -> ""), each component sanitized.
std::string namespace_of(std::string_view package);

// Why `component` cannot serve as one dot-separated piece of a --namespace-prefix, or "" if it
// can. `first` marks the leading component, which lands in the GLOBAL namespace -- where every
// `_`-initial identifier is reserved, so `_x` is refused there and accepted after a dot.
//
// A prefix is an INSTRUCTION; a package is DATA. A package is escaped through sanitize() -- the
// schema's author cannot be asked to avoid C++ keywords -- but a prefix the user typed is either
// emitted VERBATIM (effective_ns_prefix) or refused here: silently handing back a namespace the
// user did not ask for (`--namespace-prefix=std` once quietly became `std_`) is as wrong as
// refusing one that compiles. The refusal list is therefore NARROWER than sanitize()'s: the
// member-reserved words (`decode`, `Value`, ...) clash only with generated class members, are
// working namespace names, and pass.
std::string ns_prefix_component_problem(std::string_view component, bool first);

// Join two C++ namespace fragments with "::", dropping empties ("" + "a::b" -> "a::b").
std::string join_ns(std::string_view a, std::string_view b);

// The root segment for entities SHARED by both decoder models, between the prefix and the package.
// Today that is the schema's enums. Named for the role rather than the contents: a second shared
// entity (a case tag, a field-number constant) then needs no rename, and a rename here changes every
// generated name. `rp_`-free on purpose: it is unreachable from a proto name because it sits above
// every package, not inside one.
inline constexpr std::string_view kCommonRoot = "common";

// The model root segments, the two legal values of `CppNameTable::model_namespace`.
inline constexpr std::string_view kArenaRoot = "arena";
inline constexpr std::string_view kStreamRoot = "stream";

// The default `--namespace-prefix`. One source of truth: the CLI, the library entry points that
// take a prefix, and the CMake helper all mean this same string, and a build where they disagreed
// would emit headers that cannot see each other's types.
inline constexpr std::string_view kDefaultNsPrefix = "rp";

// The prefix a generator actually uses: the dot-separated components ::-joined, each emitted
// VERBATIM unless it is a keyword, `std`, a macro name, or `rp_`-prefixed (the narrow library
// safety net -- naming.cpp's sanitize_prefix_component), or the default when the caller gave
// nothing. The two entry points answer an empty prefix differently on purpose: the CLI REFUSES it
// (cli::namespace_prefix_problem), because someone typing it means something by it and the tool can
// say so; the library SUBSTITUTES the default, because an embedder who simply left the argument off
// would otherwise put the three root segments at global scope -- silently, and only in their build.
std::string effective_ns_prefix(std::string_view prefix);

// The C++ namespace a generated DECODER opens for its message types:
// `<ns_prefix>::<model_namespace>::<package>`, e.g. "rp::stream::pkg". The single source of truth
// shared by the name table and the generators, so a decoder's `namespace ... {` always matches the
// absolute message names in `names`.
std::string message_namespace(const CppNameTable& names, const FileNode& file);

// The C++ namespace the SHARED common header opens for this file's enums:
// `<ns_prefix>::common::<package>`. Model-independent by construction, which is what lets both
// decoders alias one enum type.
std::string enum_namespace(const CppNameTable& names, const FileNode& file);

// A proto name -> a collision-free C++ identifier: append `_` if it collides with a keyword, any
// `rp_`- or `RP_`-prefixed identifier (generator-internal names and the runtime's macros), or one of
// a few generated members (the streaming tag members Value/Key/kNumber/kName and decode()), or the
// namespace `std` -- see naming.cpp. Everything the emitters introduce for themselves is named into
// the `rp_` prefix instead, so it needs no reservation.
// For names not pre-assigned in the table -- e.g. enum values, which are sanitized at emit time.
// (Members in `CppNameTable::local` are already sanitized + de-duped.)
std::string sanitize(std::string_view name);

// Would this identifier macro-expand rather than compile (`EOF` -> `(-1)`, `RP_FLATTEN` -> an
// attribute)? Call it on any identifier an emitter SYNTHESIZES from a proto name -- both when the
// raw name never reached sanitize() (arenagen's oneof visit-tag struct) and when a sanitized id is
// then transformed (`capitalize()` turns the un-reserved `rP_x` into `RP_x` for the map entry
// type). Having passed through sanitize() is not on its own enough. Deliberately narrower than
// sanitize(): a tag struct may legitimately be called `Value` or `decode`, so escaping the full
// reserved set there would rename working API.
bool expands_as_macro(std::string_view name);

}  // namespace rapidproto::codegen
