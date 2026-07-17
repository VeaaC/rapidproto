// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// PROOF-OF-CONCEPT debug dumper generator: emits `<stem>.rp.debug.hpp`, a companion to the arena
// header that, for each generated arena message, prints the decoded tree in a JSON-like text form for
// human debugging. It calls the arena header's OWN accessors (reusing the same CppNameTable so names
// match exactly), so it is purely additive: no arena-runtime change.

#include <string>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/naming.hpp"

namespace rapidproto {
struct SymbolTable;  // rapidproto/resolve.hpp
}  // namespace rapidproto

namespace rapidproto::debuggen {

// Emit the debug-dumper header for `file`. `names` is the SAME table the arena header was emitted with
// (so accessor / type names match). `symbols` supplies the enum value tables (for value -> name).
std::string generate_header(const FileNode& file, const codegen::CppNameTable& names,
                            const SymbolTable& symbols);

}  // namespace rapidproto::debuggen
