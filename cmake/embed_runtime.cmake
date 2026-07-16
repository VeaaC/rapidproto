# Embed a runtime header into a generated C++ translation unit, at build time.
#
# Run via:  cmake -DRUNTIME_HPP=<path> -DOUTPUT_CPP=<path> [-DEMBED_NS=<ns>] [-DEMBED_FUNC=<fn>]
#           [-DEMBED_DECL=<decl-header>] -P cmake/embed_runtime.cmake
# Invoked by an add_custom_command keyed on the runtime header, so the embedded copy is regenerated
# whenever it changes -- there is no checked-in copy to keep in sync. Pure CMake: no external tool.
#
# The output defines <EMBED_NS>::<EMBED_FUNC>() (declared in <EMBED_DECL>); the CLI writes that text
# next to its output so generated headers are self-contained. Defaults target the base runtime.hpp
# embed in the shared codegen layer; arenagen passes its own namespace/function/decl to embed
# include/rapidproto/arena_runtime.hpp.

if(NOT DEFINED EMBED_NS)
  set(EMBED_NS "rapidproto::codegen")
endif()
if(NOT DEFINED EMBED_FUNC)
  set(EMBED_FUNC "runtime_header")
endif()
if(NOT DEFINED EMBED_DECL)
  set(EMBED_DECL "rapidproto/codegen/runtime_embedded.hpp")
endif()

file(READ "${RUNTIME_HPP}" RUNTIME_TEXT)

# The runtime is embedded as a raw string with the )RPRT" delimiter; guard against it appearing in
# the header (which would terminate the literal early).
string(FIND "${RUNTIME_TEXT}" ")RPRT\"" _collision)
if(NOT _collision EQUAL -1)
  message(FATAL_ERROR "embed_runtime: raw-string delimiter collision in ${RUNTIME_HPP}")
endif()

# Embedded as ONE raw string literal. It exceeds the 65536-char literal the C++ standard requires
# compilers to support (both gcc and clang accept far larger; only clang's pedantic -Woverlength-strings
# flags it), so the TUs that compile this file suppress that warning -- see CMakeLists.txt. This file is
# internal to rapidprotoc; the runtime the CLI writes for consumers is the plain header, not this embed.
file(WRITE "${OUTPUT_CPP}"
"// GENERATED at build time from include/rapidproto/runtime.hpp (cmake/embed_runtime.cmake). DO NOT EDIT.
// Carries the runtime header text so the generator can drop a self-contained copy beside its output.

#include \"${EMBED_DECL}\"

#include <string_view>

namespace ${EMBED_NS} {
namespace {

constexpr std::string_view kRuntime =
    R\"RPRT(${RUNTIME_TEXT})RPRT\";

}  // namespace

std::string_view ${EMBED_FUNC}() { return kRuntime; }

}  // namespace ${EMBED_NS}
")
