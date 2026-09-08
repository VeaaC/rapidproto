# Embed a binary file into a generated C translation unit, at build time.
#
# Run via:  cmake -DINPUT=<path> -DOUTPUT_C=<path> -DSYMBOL=<identifier> -P cmake/embed_binary.cmake
#
# The output defines `const unsigned char <SYMBOL>[]` and `const unsigned <SYMBOL>_len` with
# external linkage. Sibling of embed_runtime.cmake (which embeds TEXT as a raw string literal --
# unusable here: the input is arbitrary bytes, NULs included). Pure CMake: no xxd, no Python.
#
# Every input is REQUIRED, same rationale as embed_runtime.cmake: a forgotten -D must be an
# error, not a silently mis-named symbol.
foreach(_required INPUT OUTPUT_C SYMBOL)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "embed_binary.cmake: -D${_required}=... is required (and must be non-empty)")
  endif()
endforeach()

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _bytes "${_hexlen} / 2")
if(_bytes EQUAL 0)
  message(FATAL_ERROR "embed_binary.cmake: ${INPUT} is empty -- refusing to embed a zero-length array")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _array "${_hex}")

get_filename_component(_input_name "${INPUT}" NAME)
file(WRITE "${OUTPUT_C}"
"/* GENERATED at build time from ${_input_name} (cmake/embed_binary.cmake). DO NOT EDIT. */
const unsigned char ${SYMBOL}[] = {${_array}};
const unsigned ${SYMBOL}_len = ${_bytes}u;
")
