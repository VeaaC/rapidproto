// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Debug dumper generator: emits `<stem>.rp.debug.hpp`, a companion to the arena header that, for each
// generated arena message, prints the decoded tree in a JSON-like text form for human debugging. It
// calls the arena header's OWN accessors (reusing the same CppNameTable so names match exactly), so it
// is purely additive: no arena-runtime change. A debugging aid emitting JSON-*like* text -- explicitly
// not a spec-compliant JSON codec and not a wire serializer.

#include <string>

#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/naming.hpp"

namespace rapidproto {
struct SymbolTable;  // rapidproto/resolve.hpp
}  // namespace rapidproto

namespace rapidproto::debuggen {

// Emit the debug-dumper header for `file`. `names` is the SAME table the arena header was emitted with
// (so accessor / type names match), and `layouts` is the SAME LayoutSet the arena header was planned
// under -- the dumper derives arenagen's deduped synthesized names (the oneof visit-tag structs and
// the has_unknown_fields() accessor) from it so it references exactly the identifiers the arena header
// declared. `symbols` supplies the enum value tables (for value -> name).
std::string generate_header(const FileNode& file, const codegen::CppNameTable& names,
                            const arenagen::LayoutSet& layouts, const SymbolTable& symbols);

}  // namespace rapidproto::debuggen
