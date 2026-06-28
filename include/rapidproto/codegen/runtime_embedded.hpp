#pragma once

// Access to the embedded runtime header. include/rapidproto/runtime.hpp is amalgamated into a
// generated translation unit at build time (cmake/embed_runtime.cmake), so the CLI can drop a
// self-contained copy next to its output (<out-dir>/rapidproto/runtime.hpp).

#include <string_view>

namespace rapidproto::codegen {

// The exact text of include/rapidproto/runtime.hpp, embedded at build time.
std::string_view runtime_header();

}  // namespace rapidproto::codegen
