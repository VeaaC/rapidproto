#pragma once

// Access to the embedded debug-dumper runtime header. include/rapidproto/debug_runtime.hpp is
// amalgamated into a generated translation unit at build time (cmake/embed_runtime.cmake), so the CLI
// can drop a self-contained copy next to its output (<out-dir>/rapidproto/debug_runtime.hpp) -- a
// generated <stem>.rp.debug.hpp includes it, so it must resolve from the out-dir with no rapidproto
// build-tree dependency, exactly as the arena/base runtime drops do.

#include <string_view>

namespace rapidproto::debuggen {

// The exact text of include/rapidproto/debug_runtime.hpp, embedded at build time.
std::string_view debug_runtime_header();

}  // namespace rapidproto::debuggen
