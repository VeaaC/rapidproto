#pragma once

// Access to the embedded debug-dumper runtime header. include/rapidproto/dump_runtime.hpp is
// amalgamated into a generated translation unit at build time (cmake/embed_runtime.cmake), so the CLI
// can drop a self-contained copy next to its output (<out-dir>/rapidproto/dump_runtime.hpp) -- a
// generated <stem>.rp.dump.hpp includes it, so it must resolve from the out-dir with no rapidproto
// build-tree dependency, exactly as the arena/base runtime drops do.

#include <string_view>

namespace rapidproto::dumpgen {

// The exact text of include/rapidproto/dump_runtime.hpp, embedded at build time.
std::string_view dump_runtime_header();

}  // namespace rapidproto::dumpgen
