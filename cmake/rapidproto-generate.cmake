# rapidproto-generate.cmake -- defines rapidproto_generate(), the helper that turns .proto schemas into
# a linkable, header-only target by driving the rapidproto code generators.
#
# It works the same way whether rapidproto is used in-tree (the main project include()s this file and the
# generator is the rapidproto::rapidprotoc ALIAS) or from an installed package
# (find_package(rapidproto) include()s the installed copy and the same target names are imported). The
# generated output is self-contained -- the CLIs drop a std-only copy of the runtime beside the headers,
# so a consumer needs only the OUT_DIR on its include path.

include_guard(GLOBAL)

# The header path the CLI writes for `proto_abs`, computed so it can be a custom command's OUTPUT. This
# mirrors the generator exactly (resolver.cpp canonical_entry_name + driver.hpp header_path): the entry's
# path relative to the first import dir that contains it, else its basename, with ".proto" -> `ext`,
# under `out_dir`. `import_dirs_abs` is the absolute import dirs in -I order.
function(_rapidproto_output_header out_var proto_abs ext out_dir import_dirs_abs)
  # Resolve symlinks (REALPATH) so this matches the generator's canonical_entry_name, which uses
  # weakly_canonical (symlink-resolving). file(RELATIVE_PATH) is purely lexical, so without this a
  # symlinked import dir or entry path would compute a different stem than the CLI actually writes,
  # and CMake would error with "output not produced by COMMAND".
  get_filename_component(_proto_real "${proto_abs}" REALPATH)
  set(_rel "")
  foreach(_dir IN LISTS import_dirs_abs)
    get_filename_component(_dir_real "${_dir}" REALPATH)
    file(RELATIVE_PATH _candidate "${_dir_real}" "${_proto_real}")
    if(NOT _candidate MATCHES "^\\.\\.")  # _proto_real lies under _dir_real (no "../" prefix)
      set(_rel "${_candidate}")
      break()
    endif()
  endforeach()
  if(_rel STREQUAL "")
    get_filename_component(_rel "${_proto_real}" NAME)
  endif()
  string(REGEX REPLACE "\\.proto$" "" _rel "${_rel}")
  set(${out_var} "${out_dir}/${_rel}${ext}" PARENT_SCOPE)
endfunction()

# Error unless `tool` is an imported target (came from find_package) -- under cross-compilation that is a
# host-built generator that can actually run on the build machine, whereas the in-tree ALIAS is built for
# the target and would not.
function(_rapidproto_require_host_tool tool target)
  set(_resolved "${tool}")
  get_target_property(_alias "${tool}" ALIASED_TARGET)
  if(_alias)
    set(_resolved "${_alias}")
  endif()
  get_target_property(_imported "${_resolved}" IMPORTED)
  if(NOT _imported)
    message(FATAL_ERROR
      "rapidproto_generate(${target}): cross-compiling, but ${tool} is the in-tree generator -- built "
      "for the target, so it cannot run on the build host. Provide a HOST build of the generators "
      "(build/install rapidproto for the host, brought in via find_package or an IMPORTED target). "
      "Note: an imported generator from a TARGET sysroot is equally unrunnable here, and this check "
      "cannot detect that -- make sure the imported rapidproto::* tools are host binaries.")
  endif()
endfunction()

# rapidproto_generate(<target>
#   PROTOS <file.proto>...          # schema entry files (generated as ONE batch with their imports)
#   [GENERATOR arena|stream|both]   # which decoder(s) to emit (default: arena -- the default model)
#   [IMPORT_DIRS <dir>...]          # -I import search roots (the root your .proto tree imports against)
#   [NAMESPACE_PREFIX <ns>]         # nest generated namespaces under <ns> (e.g. to coexist with protoc)
#   [OUT_DIR <dir>]                 # where headers are written (default: a private dir under the build)
#   [UNKNOWN_PRESENT]               # arena: reserve a per-message "unknown fields present" bit
#   [FIELD_MODES <file>...]         # arena: decode profile files (`name|drop|raw <name>` lines)
#   [DROP <name>...]                # arena: drop these fields/types (no storage, no accessor)
#   [RAW <name>...]                 # arena: keep message fields'/types' payloads for deferred decodes
#   [NO_WELLKNOWN])                 # do not supply the embedded google.protobuf well-known types
#
# Creates an INTERFACE library `<target>`. Linking it (target_link_libraries(app PRIVATE <target>)) both
# generates the headers before `app` compiles and adds OUT_DIR to `app`'s include path, so
# `#include "<schema-stem>.rp.stream.hpp"` (or ".rp.hpp") resolves.
#
# GENERATOR both writes both headers into one OUT_DIR and they COEXIST in a single translation unit:
# the arena types live at `pkg::Msg`, the streaming types at `pkg::stream::Msg`, and the schema's enums
# are ONE shared type in `<stem>.rp.common.hpp` that both #include. So a TU can use both models at once
# (examples/consumer decodes the same bytes both ways to prove it).
function(rapidproto_generate target)
  set(_options UNKNOWN_PRESENT NO_WELLKNOWN)
  set(_one OUT_DIR GENERATOR NAMESPACE_PREFIX)
  set(_multi PROTOS IMPORT_DIRS FIELD_MODES DROP RAW)
  cmake_parse_arguments(RPG "${_options}" "${_one}" "${_multi}" ${ARGN})

  if(RPG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "rapidproto_generate(${target}): unexpected arguments: ${RPG_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT RPG_PROTOS)
    message(FATAL_ERROR "rapidproto_generate(${target}): PROTOS is required")
  endif()
  if(NOT RPG_GENERATOR)
    set(RPG_GENERATOR "arena")  # arena is the default decoder model (see rapidprotoc)
  endif()

  set(_jobs "")
  if(RPG_GENERATOR STREQUAL "stream" OR RPG_GENERATOR STREQUAL "both")
    list(APPEND _jobs "stream")
  endif()
  if(RPG_GENERATOR STREQUAL "arena" OR RPG_GENERATOR STREQUAL "both")
    list(APPEND _jobs "arena")
  endif()
  if(NOT _jobs)
    message(FATAL_ERROR
      "rapidproto_generate(${target}): GENERATOR must be stream, arena, or both (got '${RPG_GENERATOR}')")
  endif()

  if(NOT RPG_OUT_DIR)
    set(RPG_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/rapidproto/${target}")
  endif()
  get_filename_component(RPG_OUT_DIR "${RPG_OUT_DIR}" ABSOLUTE)

  set(_common "")
  set(_import_dirs_abs "")
  foreach(_dir IN LISTS RPG_IMPORT_DIRS)
    get_filename_component(_dir_abs "${_dir}" ABSOLUTE)
    list(APPEND _import_dirs_abs "${_dir_abs}")
    list(APPEND _common "-I${_dir_abs}")
  endforeach()
  if(RPG_NAMESPACE_PREFIX)
    list(APPEND _common "--namespace-prefix" "${RPG_NAMESPACE_PREFIX}")
  endif()
  if(RPG_NO_WELLKNOWN)
    list(APPEND _common "--no-wellknown")
  endif()

  # add_custom_command(DEPFILE) drives regeneration when an imported .proto changes. Ninja supports it
  # on every CMake we need; the Makefile generators require CMake >= 3.20, Xcode/Visual Studio >= 3.21.
  # Where unsupported, generation is still correct but won't auto-retrigger on an import edit.
  set(_rpg_depfile TRUE)
  if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    if(CMAKE_VERSION VERSION_LESS 3.20)
      set(_rpg_depfile FALSE)
    elseif(CMAKE_GENERATOR MATCHES "Xcode|Visual Studio" AND CMAKE_VERSION VERSION_LESS 3.21)
      set(_rpg_depfile FALSE)
    endif()
  endif()
  if(NOT _rpg_depfile AND NOT DEFINED _RAPIDPROTO_DEPFILE_WARNED)
    message(WARNING
      "rapidproto_generate(): incremental import tracking needs CMake >= 3.20 (Makefiles) or >= 3.21 "
      "(Xcode/Visual Studio); on ${CMAKE_GENERATOR} with CMake ${CMAKE_VERSION} it is off. Generation "
      "stays correct -- but after editing an imported .proto, re-run CMake or do a clean build.")
    set(_RAPIDPROTO_DEPFILE_WARNED TRUE CACHE INTERNAL "rapidproto depfile-unsupported warning shown")
  endif()

  # CMP0116 (CMake >= 3.20): Ninja transforms add_custom_command DEPFILEs -- their paths are interpreted
  # relative to CMAKE_CURRENT_BINARY_DIR, the same as the Makefile generators. Set it explicitly (both to
  # silence the dev warning and to lock the behavior in) and run the generator from that matching base.
  # On older CMake the policy doesn't exist and Ninja reads the depfile raw, relative to the top build
  # dir -- so point there instead. Either way the CLI emits depfile paths relative to its working dir.
  if(POLICY CMP0116)
    cmake_policy(SET CMP0116 NEW)
    set(_rpg_workdir "${CMAKE_CURRENT_BINARY_DIR}")
  else()
    set(_rpg_workdir "${CMAKE_BINARY_DIR}")
  endif()

  # GENERATOR both rides the same single invocation: the CLI writes both decoders (+ the shared
  # common header) per file, so the custom command lists every selected header as OUTPUT under one
  # multi-target depfile. The model flags and produced headers derive from the selected jobs
  # (arena before stream, matching the CLI's emit + depfile-target order).
  set(_model_flags "")
  set(_modes_files_abs "")
  if("arena" IN_LIST _jobs)
    list(APPEND _model_flags "--arena")
    if(RPG_UNKNOWN_PRESENT)
      list(APPEND _model_flags "--unknown-present")
    endif()
    foreach(_modes IN LISTS RPG_FIELD_MODES)
      get_filename_component(_modes_abs "${_modes}" ABSOLUTE)
      list(APPEND _modes_files_abs "${_modes_abs}")
      list(APPEND _model_flags "--field-modes=${_modes_abs}")
    endforeach()
    foreach(_name IN LISTS RPG_DROP)
      list(APPEND _model_flags "--drop=${_name}")
    endforeach()
    foreach(_name IN LISTS RPG_RAW)
      list(APPEND _model_flags "--raw=${_name}")
    endforeach()
  elseif(RPG_FIELD_MODES OR RPG_DROP OR RPG_RAW)
    message(FATAL_ERROR
      "rapidproto_generate(${target}): FIELD_MODES/DROP/RAW shape the arena decoder; use "
      "GENERATOR arena or both (got '${RPG_GENERATOR}')")
  endif()
  if("stream" IN_LIST _jobs)
    list(APPEND _model_flags "--stream")
  endif()

  set(_cli rapidproto::rapidprotoc)
  if(CMAKE_CROSSCOMPILING)
    _rapidproto_require_host_tool(${_cli} ${target})
  endif()

  # ONE rapidprotoc invocation for the whole target: the entries resolve as a batch (shared
  # imports parse once, and a FIELD_MODES profile resolves against every proto's symbols at once,
  # so one profile can span the target's schemas). The cost: touching any listed proto re-runs
  # generation for the whole target.
  set(_outputs "")
  set(_protos_abs "")
  foreach(_proto IN LISTS RPG_PROTOS)
    get_filename_component(_proto_abs "${_proto}" ABSOLUTE)
    list(APPEND _protos_abs "${_proto_abs}")
    if("arena" IN_LIST _jobs)
      _rapidproto_output_header(_h "${_proto_abs}" ".rp.hpp" "${RPG_OUT_DIR}" "${_import_dirs_abs}")
      list(APPEND _outputs "${_h}")
    endif()
    if("stream" IN_LIST _jobs)
      _rapidproto_output_header(_h "${_proto_abs}" ".rp.stream.hpp" "${RPG_OUT_DIR}" "${_import_dirs_abs}")
      list(APPEND _outputs "${_h}")
    endif()
  endforeach()
  # Name the depfile off the first header; the CLI lists every entry's decoder header as a target
  # in it (so each output node gets the import edges), and re-running regenerates the whole batch.
  list(GET _outputs 0 _anchor)
  set(_depfile_cli "")
  set(_depfile_cmd "")
  if(_rpg_depfile)
    set(_depfile_cli --depfile "${_anchor}.d")
    set(_depfile_cmd DEPFILE "${_anchor}.d")
  endif()
  # Run from the depfile's interpretation base (see _rpg_workdir) so the targets the CLI emits --
  # relative to its working directory -- match how the build tool names the output nodes. All CLI
  # arguments are absolute, so the working directory does not otherwise matter.
  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND ${_cli} ${_common} ${_model_flags} --out-dir "${RPG_OUT_DIR}" ${_depfile_cli} ${_protos_abs}
    ${_depfile_cmd}
    DEPENDS ${_protos_abs} ${_modes_files_abs} ${_cli}
    WORKING_DIRECTORY "${_rpg_workdir}"
    COMMENT "rapidproto: ${target}"
    VERBATIM)

  # A driver target builds all the headers; the INTERFACE library consumers link depends on it (so
  # linking the library triggers generation) and carries OUT_DIR as a usage-requirement include dir
  # plus the C++17 floor -- without it a consumer inherits the toolchain's default standard, and the
  # generated headers happen to compile only where that default is >= 17.
  add_custom_target(${target}_generate DEPENDS ${_outputs})
  add_library(${target} INTERFACE)
  add_dependencies(${target} ${target}_generate)
  target_include_directories(${target} INTERFACE "${RPG_OUT_DIR}")
  target_compile_features(${target} INTERFACE cxx_std_17)
endfunction()
