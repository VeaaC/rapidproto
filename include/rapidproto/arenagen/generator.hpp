// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The arena object-tree decoder code generator: emits a *.rp.hpp header whose classes materialize a
// protobuf message into a fully-allocated, read-only tree inside a bump Arena. Each message becomes a
// class with a padding-minimized storage layout (decided by the layout planner in layout.hpp), bit-packed
// presence/value flags, read-only value/optional accessors, and a static decode() entry point.
// Generated code depends only on the header-only arena runtime (rapidproto/arena_runtime.hpp).

#include <string>
#include <unordered_map>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/naming.hpp"

namespace rapidproto {
struct ResolvedFileSet;  // rapidproto/resolver.hpp
struct SymbolTable;      // rapidproto/resolve.hpp
}  // namespace rapidproto

namespace rapidproto::arenagen {

// Identifiers arenagen synthesizes from field/oneof/map names -- the <Oneof> visit-tag struct, the
// <Map>Entry type, and the has_unknown_fields() accessor. The shared CppNameTable dedups the base
// names (fields/maps/nested types) against each other, but not these derived forms, so a user nested
// type literally named `FooEntry` could collide. Computed once per message (keyed by node pointer)
// with a `_` suffix on collision, so the output always compiles. Exposed so a companion emitter (the
// debug dumper) can name the SAME deduped identifiers the arena header used and match it exactly.
struct SynthNames {
    std::unordered_map<const OneofNode*, std::string> case_tag;       // <Oneof> visit-tag struct
    std::unordered_map<const MapFieldNode*, std::string> entry_type;  // <Map>Entry
    std::unordered_map<const MessageNode*, std::string> unknown;      // has_unknown_fields()
    // Private storage members, deduped in the same class scope as everything above. They are
    // derived (`m_` + the member's id), so without dedup they can take the CLASS's own name
    // (`message m_x { int32 x = 1; }`) or a sibling's accessor (`int32 foo = 1; int32 m_foo = 2;`)
    // -- both protoc-valid, both previously emitting a header that does not compile.
    // Keyed by FieldNode*/MapFieldNode* for a field's storage, and by OneofNode* for its union.
    std::unordered_map<const void*, std::string> storage;
    std::unordered_map<const OneofNode*, std::string> case_member;  // m_rp_<oneof>_case
    std::unordered_map<std::string, std::string> mask;              // message fqn -> m_rp_mask
};

// Build the synthesized-name table for every message in `file`, deduped against the already-deduped
// field/map/nested-type names in `names` and the layout-derived members in `layouts`. The arena
// header emits with this table; the debug dumper takes it (via generate_header) to match.
SynthNames build_synth_names(const codegen::CppNameTable& names, const LayoutSet& layouts,
                             const FileNode& file);

// Emit the header for `file`. `names` (build_cpp_names), `layouts` (plan_layouts), and `symbols` (the
// table analyze() returned, whose FQN -> node maps resolve referenced types) are built ONCE for the
// whole resolved set and shared across every file. `modes` is the resolved field-modes selection
// the SAME layouts were planned under (or null): when active, the header carries the profile in
// its banner and wraps the classes in an `inline namespace rp_modes_<id>` so mixed-profile TUs
// hold distinct types (a link error at any exchange point, never a silent ODR violation).
std::string generate_header(const FileNode& file, const codegen::CppNameTable& names,
                            const LayoutSet& layouts, const SymbolTable& symbols,
                            const FieldModes* modes = nullptr);

// Convenience: build the name table + layouts for `set` and emit `file`. `symbols` is analyze()'s
// table. `namespace_prefix` is the ROOT the generated code lives under (`prefix::arena::a::b`),
// identical to streamgen's `--namespace-prefix`. Empty means the DEFAULT (`rp`), not "no prefix":
// codegen::effective_ns_prefix substitutes it, so a caller that omits the argument cannot emit
// rootless names. A caller emitting a whole set should
// instead build names/layouts once and use the (file, names, layouts) overload per file.
std::string generate_header(const FileNode& file, const ResolvedFileSet& set,
                            const SymbolTable& symbols, const std::string& namespace_prefix = {});

}  // namespace rapidproto::arenagen
