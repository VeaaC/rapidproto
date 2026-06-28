#pragma once

// Access to the embedded arena runtime header. include/rapidproto/arena_runtime.hpp is amalgamated
// into a generated translation unit at build time (cmake/embed_runtime.cmake), so the CLI can drop a
// self-contained copy next to its output (<out-dir>/rapidproto/arena_runtime.hpp). The arena runtime
// itself depends on rapidproto/runtime.hpp, which the CLI also drops (reusing the codegen layer's
// runtime_header() embed).

#include <string_view>

namespace rapidproto::arenagen {

// The exact text of include/rapidproto/arena_runtime.hpp, embedded at build time.
std::string_view arena_runtime_header();

}  // namespace rapidproto::arenagen
