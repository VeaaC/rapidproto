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
// Within a message's struct scope the members are: nested enums, nested messages, field tags, and
// map tags. Nested types are assigned first so their names stay plain-sanitized where possible,
// keeping absolute references stable.
struct CppNameTable {
    std::unordered_map<std::string, std::string> absolute;  // type fqn -> "::a::b::Local"
    std::unordered_map<const void*, std::string> local;     // member node -> unqualified C++ id
    std::string ns_prefix;  // C++ namespace prefix prepended to every file's package namespace
    // Per-model sub-namespace for TOP-LEVEL messages (the decoders), e.g. "stream", so the streaming
    // and arena types coexist in one TU; empty (the default / arena model) keeps messages at package
    // scope. Top-level enums are never nested under it -- they are shared, in the common header. Must
    // be a single valid C++ identifier: it is concatenated verbatim (not run through namespace_of).
    std::string model_namespace;
};

// Build the name table for the resolved set. `all_files` is the whole set (a file's imports precede
// it); indexing every file means a cross-file type reference resolves to the imported scope's
// dedup-stable absolute name. When `all_files` is empty, only `file` is indexed (the single-file
// convenience path, valid when `file` has no cross-file type references). `ns_prefix` is an already
// `::`-joined C++ namespace (see `namespace_of`), possibly empty. `model_namespace` (e.g. "stream")
// nests TOP-LEVEL messages under that extra segment so the two decoder models coexist; empty leaves
// them at package scope (top-level enums are never nested). Built ONCE per set and reused for every
// file's `generate_header`.
CppNameTable build_cpp_names(const FileNode& file, const std::vector<FileNode>& all_files,
                             std::string ns_prefix, std::string model_namespace = {});

// A resolved type FQN -> its absolute C++ name. Types in the set use their dedup-stable name;
// anything not in the set (should not occur for a resolved type) falls back to a plain mapping.
std::string cpp_type_name(const CppNameTable& names, std::string_view fqn);

// proto package "a.b.c" -> C++ namespace "a::b::c" (empty package -> ""), each component sanitized.
std::string namespace_of(std::string_view package);

// Join two C++ namespace fragments with "::", dropping empties ("" + "a::b" -> "a::b").
std::string join_ns(std::string_view a, std::string_view b);

// The C++ namespace a generated DECODER opens for its message types: the file's package namespace
// (under `ns_prefix`) plus `model_namespace`, e.g. "rp::pkg::stream" (empty for a no-package,
// default-model file). The single source of truth shared by the name table and the generators, so a
// decoder's `namespace ... {` always matches the absolute message names in `names`. Top-level enums
// use the package namespace WITHOUT the model segment (the common header) -- one shared enum type.
std::string message_namespace(const CppNameTable& names, const FileNode& file);

// A proto name -> a collision-free C++ identifier: append `_` if it collides with a keyword, any
// `rp_`-prefixed identifier, or one of a few generated members (the streaming tag members
// Value/Key/kNumber/kName, decode(), the decode() template parameter Callbacks, and the arena `m_bytes`
// storage name -- see naming.cpp). For names not pre-assigned in the table -- e.g. enum values, which
// are sanitized at emit time. (Members in `CppNameTable::local` are already sanitized + de-duped.)
std::string sanitize(std::string_view name);

}  // namespace rapidproto::codegen
