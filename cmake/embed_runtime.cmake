# Embed a runtime header into a generated C++ translation unit, at build time.
#
# Run via:  cmake -DRUNTIME_HPP=<path> -DRUNTIME_REL=<repo-relative path> -DOUTPUT_CPP=<path>
#           -DEMBED_NS=<ns> -DEMBED_FUNC=<fn> -DEMBED_DECL=<decl-header> -P cmake/embed_runtime.cmake
# Invoked by an add_custom_command keyed on the runtime header, so the embedded copy is regenerated
# whenever it changes -- there is no checked-in copy to keep in sync. Pure CMake: no external tool.
#
# The output defines <EMBED_NS>::<EMBED_FUNC>() (declared in <EMBED_DECL>); the CLI writes that text
# next to its output so generated headers are self-contained. Every input is passed explicitly by
# CMakeLists' _rapidproto_embed_runtime() -- one call per embedded runtime (codegen's runtime.hpp,
# arenagen's arena_runtime.hpp, dumpgen's dump_runtime.hpp).

# Every input is REQUIRED: the old defaults duplicated the codegen call site's values in a second
# place, and made a forgotten -D produce a silently mis-namespaced TU instead of an error.
foreach(_required RUNTIME_HPP RUNTIME_REL OUTPUT_CPP EMBED_NS EMBED_FUNC EMBED_DECL)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "embed_runtime.cmake: -D${_required}=... is required (and must be non-empty)")
  endif()
endforeach()

# Embedded as a byte ARRAY, not a string literal: a runtime header (runtime.hpp is ~67 KB) exceeds
# the 65535-byte literal MSVC hard-caps at (C2026) -- gcc and clang accept far larger, MSVC does not,
# and adjacent-literal concatenation cannot rescue it (the result is still one capped literal). A
# char[] initializer has no such limit. Sibling of embed_binary.cmake, which uses the same HEX read
# for arbitrary bytes; here the input is text, but the array form sidesteps the literal cap. sizeof
# the array is the exact byte count (no trailing NUL), so the string_view spans exactly the header.
# This file is internal to rapidprotoc; the runtime the CLI writes for consumers is the plain header.
file(READ "${RUNTIME_HPP}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _bytes "${_hexlen} / 2")
if(_bytes EQUAL 0)
  message(FATAL_ERROR "embed_runtime: ${RUNTIME_HPP} is empty -- refusing to embed a zero-length runtime")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _array "${_hex}")

file(WRITE "${OUTPUT_CPP}"
"// GENERATED at build time from ${RUNTIME_REL} (cmake/embed_runtime.cmake). DO NOT EDIT.
// Carries the runtime header text so the generator can drop a self-contained copy beside its output.

#include \"${EMBED_DECL}\"

#include <string_view>

namespace ${EMBED_NS} {
namespace {

// unsigned char, not char: char is signed on many targets and a byte >= 0x80 (UTF-8 lead bytes in
// the header text) would be a narrowing error in a char[] initializer. sizeof is the exact byte
// count -- no trailing NUL -- so the view below spans exactly the header. A C array is required (a
// string literal would exceed MSVC's cap), so the avoid-c-arrays guidance does not apply.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
constexpr unsigned char kRuntime[] = {${_array}};

}  // namespace

std::string_view ${EMBED_FUNC}() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte array -> char view, no aliasing
  return std::string_view{reinterpret_cast<const char*>(kRuntime), sizeof kRuntime};
}

}  // namespace ${EMBED_NS}
")
