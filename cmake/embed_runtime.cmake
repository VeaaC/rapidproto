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

# A runtime header (runtime.hpp is ~67 KB) exceeds the limit MSVC hard-caps a single string literal
# at (C2026) -- gcc and clang accept far larger, MSVC does not. Embed it as raw-string CHUNKS held
# in a std::array and joined once at run time:
# each chunk stays well under the cap and no single literal is ever large, while the text stays
# readable (not a hex blob). Chunks break on NEWLINE boundaries so a multi-byte UTF-8 sequence is
# never split. This file is internal to rapidprotoc; the runtime the CLI writes is the plain header.
file(READ "${RUNTIME_HPP}" _text)
string(LENGTH "${_text}" _total)
if(_total EQUAL 0)
  message(FATAL_ERROR "embed_runtime: ${RUNTIME_HPP} is empty -- refusing to embed a zero-length runtime")
endif()
# The raw-string delimiter must not appear as )RPRT" in the header, or a chunk would end early.
string(FIND "${_text}" ")RPRT\"" _collision)
if(NOT _collision EQUAL -1)
  message(FATAL_ERROR "embed_runtime: raw-string delimiter collision in ${RUNTIME_HPP}")
endif()

# ~8000 bytes per chunk (well under MSVC's per-literal cap), each ending at a newline. LENGTH/FIND/
# SUBSTRING are all byte-indexed, and a newline is one byte, so breaking there never splits a char.
# The break is newline-only, so a chunk is ~8000 + (length of the line it ends on): this assumes
# runtime-header lines stay well under MSVC's per-literal cap (they are ~280 chars today). A single
# line longer than that cap would need character-boundary splitting instead (as embed_wellknown.py
# does); the runtime headers are our own, so the assumption is safe and checked by the Windows CI.
set(_chunk_bytes 8000)
set(_pos 0)
set(_count 0)
set(_elems "")
while(_pos LESS _total)
  math(EXPR _target "${_pos} + ${_chunk_bytes}")
  if(_target GREATER_EQUAL _total)
    set(_end ${_total})
  else()
    math(EXPR _restlen "${_total} - ${_target}")
    string(SUBSTRING "${_text}" ${_target} ${_restlen} _rest)
    string(FIND "${_rest}" "\n" _nl)
    if(_nl EQUAL -1)
      set(_end ${_total})
    else()
      math(EXPR _end "${_target} + ${_nl} + 1")  # include the newline in this chunk
    endif()
  endif()
  math(EXPR _len "${_end} - ${_pos}")
  string(SUBSTRING "${_text}" ${_pos} ${_len} _chunk)
  string(APPEND _elems "    R\"RPRT(${_chunk})RPRT\",\n")
  math(EXPR _count "${_count} + 1")
  set(_pos ${_end})
endwhile()

file(WRITE "${OUTPUT_CPP}"
"// GENERATED at build time from ${RUNTIME_REL} (cmake/embed_runtime.cmake). DO NOT EDIT.
// Carries the runtime header text so the generator can drop a self-contained copy beside its output.

#include \"${EMBED_DECL}\"

#include <array>
#include <string>
#include <string_view>

namespace ${EMBED_NS} {
namespace {

// Raw-string chunks (see embed_runtime.cmake): each is under MSVC's single-literal cap; joined once
// on first use into a static string the returned view spans for the program's lifetime.
constexpr std::array<std::string_view, ${_count}> kChunks = {
${_elems}};

}  // namespace

std::string_view ${EMBED_FUNC}() {
  static const std::string text = [] {
    std::string s;
    for (const std::string_view chunk : kChunks) {
      s += chunk;
    }
    return s;
  }();
  return text;
}

}  // namespace ${EMBED_NS}
")
