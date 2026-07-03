// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The arena object-tree decoder code generator: emits a *.rp.hpp header whose classes materialize a
// protobuf message into a fully-allocated, read-only tree inside a bump Arena. Each message becomes a
// class with a padding-minimized storage layout (decided by the layout planner in layout.hpp), bit-packed
// presence/value flags, read-only value/optional accessors, and a static decode() entry point.
// Generated code depends only on the header-only arena runtime (rapidproto/arena_runtime.hpp).

#include <string>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/naming.hpp"

namespace rapidproto {
struct ResolvedFileSet;  // rapidproto/resolver.hpp
struct SymbolTable;      // rapidproto/resolve.hpp
}  // namespace rapidproto

namespace rapidproto::arenagen {

// Emit the header for `file`. `names` (build_cpp_names), `layouts` (plan_layouts), and `symbols` (the
// table analyze() returned, whose FQN -> node maps resolve referenced types) are built ONCE for the
// whole resolved set and shared across every file.
std::string generate_header(const FileNode& file, const codegen::CppNameTable& names,
                            const LayoutSet& layouts, const SymbolTable& symbols);

// Convenience: build the name table + layouts for `set` and emit `file`. `symbols` is analyze()'s
// table. `namespace_prefix` is the dot-separated prefix (proto convention) nesting every generated
// namespace, identical to streamgen's `--namespace-prefix`. A caller emitting a whole set should
// instead build names/layouts once and use the (file, names, layouts) overload per file.
std::string generate_header(const FileNode& file, const ResolvedFileSet& set,
                            const SymbolTable& symbols, const std::string& namespace_prefix = {});

}  // namespace rapidproto::arenagen
