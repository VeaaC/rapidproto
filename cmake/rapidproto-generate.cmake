# rapidproto-generate.cmake -- defines rapidproto_generate(), the helper that turns .proto schemas into
# a linkable, header-only target by driving the rapidproto code generators.
#
# It works whether rapidproto is used in-tree (the main project include()s this file and the
# generator is the rapidproto::rapidprotoc ALIAS) or from an installed package
# (find_package(rapidproto) include()s the installed copy and the same target names are imported).
# What differs is how much the build system is told about the generated files: with an installed
# (imported) generator the helper ASKS it for the exact output list at configure time
# (`--list-outputs`); in-tree the tool does not exist yet, so a smaller list is declared -- see
# the two modes where the OUTPUT list is built, below. (Cross-compilation additionally REQUIRES
# the imported form -- see _rapidproto_require_host_tool.) The generated output is self-contained --
# the CLIs drop a std-only copy of the runtime beside the headers, so a consumer needs only the
# OUT_DIR on its include path.

include_guard(GLOBAL)

# FALLBACK-MODE ONLY (the query mode asks the CLI instead): the header path the CLI writes for
# `proto_abs`, computed so it can be a custom command's OUTPUT. Mirrors the generator's ENTRY rule
# (resolver.cpp canonical_entry_name + driver.hpp header_path) -- and only that rule: the entry's
# path relative to the first import dir that contains it, else its basename, with ".proto" -> `ext`,
# under `out_dir`. `import_dirs_abs` is the absolute import dirs in -I order.
function(_rapidproto_output_header out_var proto_abs ext out_dir import_dirs_abs)
  # Resolve symlinks (REALPATH) for the import-relative test, matching canonical_entry_name, which
  # weakly_canonical()s both the entry and the include dir before relativizing. file(RELATIVE_PATH)
  # is purely lexical, so without this a symlinked import dir would compute a different stem than
  # the CLI writes, and CMake would error with "output not produced by COMMAND".
  #
  # The FALLBACK below deliberately does NOT use the resolved path: when the entry resolves under no
  # import dir, canonical_entry_name returns the spelling it was GIVEN, and the CLI names the header
  # from that. Taking REALPATH's basename here instead made the two disagree for a symlinked entry
  # whose link name differs from its target -- `protolink/alias.proto -> ../real/aaa.proto` had
  # CMake declare aaa.rp.hpp while the CLI wrote alias.rp.hpp, so the declared output never appeared
  # and the target regenerated on every build.
  # Mirror weakly_canonical: resolve the longest EXISTING prefix, then re-attach what is missing.
  # REALPATH leaves the WHOLE path unresolved as soon as its last component is absent -- not just
  # that component -- and an entry another rule generates does not exist when this runs, possibly
  # several directories deep. Resolving only the parent would close one level and leave the rest
  # disagreeing with the CLI, which resolves at BUILD time when the file is there. A build tree
  # below macOS's /var -> /private/var makes this ordinary rather than exotic.
  set(_probe "${proto_abs}")
  set(_missing_tail "")
  while(NOT EXISTS "${_probe}")
    get_filename_component(_parent "${_probe}" DIRECTORY)
    if(_parent STREQUAL "${_probe}")
      break()  # walked up to the root without finding anything that exists
    endif()
    get_filename_component(_missing_name "${_probe}" NAME)
    if(_missing_tail STREQUAL "")
      set(_missing_tail "${_missing_name}")
    else()
      set(_missing_tail "${_missing_name}/${_missing_tail}")
    endif()
    set(_probe "${_parent}")
  endwhile()
  get_filename_component(_probe_real "${_probe}" REALPATH)
  if(_missing_tail STREQUAL "")
    set(_proto_real "${_probe_real}")
  else()
    set(_proto_real "${_probe_real}/${_missing_tail}")
  endif()
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
    get_filename_component(_rel "${proto_abs}" NAME)  # as GIVEN -- see above
  endif()
  string(REGEX REPLACE "\\.proto$" "" _rel "${_rel}")
  set(${out_var} "${out_dir}/${_rel}${ext}" PARENT_SCOPE)
endfunction()

# The generator's on-disk binary, when one can exist at CONFIGURE time. An IMPORTED target
# (find_package consumers) must yield one -- IMPORTED_LOCATION, possibly per-configuration -- and
# a location that does not exist on disk is a broken install, refused here rather than silently
# degrading to the smaller fallback declaration set. Empty ONLY for a non-imported target (the
# in-tree ALIAS: this project's own build, or a FetchContent consumer), which this very
# buildsystem builds and so cannot exist yet.
function(_rapidproto_tool_location out_var tool)
  if(NOT TARGET "${tool}")
    # Without this, the literal target name reached the generated makefile, where its colons are a
    # syntax error a consumer cannot trace back to the missing find_package/add_subdirectory.
    message(FATAL_ERROR
      "rapidproto_generate(): no `${tool}` target -- find_package(rapidproto) first, or add the "
      "rapidproto source tree before calling this")
  endif()
  set(_resolved "${tool}")
  get_target_property(_alias "${tool}" ALIASED_TARGET)
  if(_alias)
    set(_resolved "${_alias}")
  endif()
  get_target_property(_imported "${_resolved}" IMPORTED)
  if(NOT _imported)
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  set(_locations "")
  get_target_property(_loc "${_resolved}" IMPORTED_LOCATION)
  if(_loc)
    list(APPEND _locations "${_loc}")
  endif()
  get_target_property(_configs "${_resolved}" IMPORTED_CONFIGURATIONS)
  if(_configs)
    foreach(_cfg IN LISTS _configs)
      get_target_property(_loc "${_resolved}" IMPORTED_LOCATION_${_cfg})
      if(_loc)
        list(APPEND _locations "${_loc}")
      endif()
    endforeach()
  endif()
  foreach(_loc IN LISTS _locations)
    if(EXISTS "${_loc}")
      set(${out_var} "${_loc}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "rapidproto_generate(): `${tool}` is an imported target but none of its IMPORTED_LOCATION"
    " properties names an existing file (checked: '${_locations}') -- the rapidproto install is "
    "broken or was moved")
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
#   [DUMP]                          # also emit the JSON-like debug dumper (<stem>.rp.dump.hpp); needs arena
#   [IMPORT_DIRS <dir>...]          # -I import search roots (the root your .proto tree imports against)
#   [NAMESPACE_PREFIX <ns>]         # rename the root the generated code lives under (default: rp)
#   [OUT_DIR <dir>]                 # where headers are written (default: a private dir under the build)
#   [UNKNOWN_PRESENT]               # arena: reserve the "unknown fields present" bit on every message
#   [UNKNOWN <message>...]          # arena: reserve that bit on these messages only
#   [FIELD_MODES <file>...]         # arena: decode profile files (`name|drop|raw|unknown-fields <name>`)
#   [DROP <name>...]                # arena: drop these fields/types (no storage, no accessor)
#   [RAW <name>...]                 # arena: keep message fields'/types' payloads for deferred decodes
#   [NO_WELLKNOWN])                 # do not supply the embedded google.protobuf well-known types
#
# Creates an INTERFACE library `<target>`. Linking it (target_link_libraries(app PRIVATE <target>)) both
# generates the headers before `app` compiles and adds OUT_DIR to `app`'s include path, so
# `#include "<schema-stem>.rp.stream.hpp"` (or ".rp.hpp") resolves.
#
# GENERATOR both writes both headers into one OUT_DIR and they COEXIST in a single translation unit:
# the arena types live at `rp::arena::pkg::Msg`, the streaming types at `rp::stream::pkg::Msg`, and
# the schema's enums are ONE shared type in `<stem>.rp.common.hpp` that both #include. So a TU can use both models at once
# (examples/consumer decodes the same bytes both ways to prove it).
function(rapidproto_generate target)
  set(_options UNKNOWN_PRESENT NO_WELLKNOWN DUMP)
  set(_one OUT_DIR GENERATOR NAMESPACE_PREFIX)
  set(_multi PROTOS IMPORT_DIRS FIELD_MODES DROP RAW UNKNOWN)
  cmake_parse_arguments(RPG "${_options}" "${_one}" "${_multi}" ${ARGN})

  if("NAMESPACE_PREFIX" IN_LIST RPG_KEYWORDS_MISSING_VALUES)
    # Empty is not "no prefix": the generated roots would land at global scope, so the CLI rejects it.
    # Caught here because cmake_parse_arguments leaves the variable UNSET for an explicit empty value,
    # which would otherwise look exactly like omitting the keyword and silently use the default.
    message(FATAL_ERROR
      "rapidproto_generate(${target}): NAMESPACE_PREFIX cannot be empty -- the arena/stream/common "
      "roots would land at global scope. Pass a name instead (default: rp).")
  endif()

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
  # DEFINED, not truthiness: CMake reads `NAMESPACE_PREFIX N` (or `no`/`off`/`false`/`0`) as false,
  # so a truthiness test silently dropped the flag and generated under the default instead. `N` is a
  # plausible short namespace.
  if(DEFINED RPG_NAMESPACE_PREFIX)
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
    foreach(_name IN LISTS RPG_UNKNOWN)
      list(APPEND _model_flags "--unknown=${_name}")
    endforeach()
  elseif(RPG_FIELD_MODES OR RPG_DROP OR RPG_RAW OR RPG_UNKNOWN OR RPG_UNKNOWN_PRESENT)
    message(FATAL_ERROR
      "rapidproto_generate(${target}): FIELD_MODES/DROP/RAW/UNKNOWN/UNKNOWN_PRESENT shape the arena "
      "decoder; use GENERATOR arena or both (got '${RPG_GENERATOR}')")
  endif()
  if("stream" IN_LIST _jobs)
    list(APPEND _model_flags "--stream")
  endif()
  # The debug dumper (<stem>.rp.dump.hpp) reads the arena accessors, so it needs the arena header.
  if(RPG_DUMP)
    if(NOT "arena" IN_LIST _jobs)
      message(FATAL_ERROR
        "rapidproto_generate(${target}): DUMP emits a dumper over the arena decoder; use GENERATOR "
        "arena or both (got '${RPG_GENERATOR}')")
    endif()
    list(APPEND _model_flags "--dump")
  endif()

  set(_cli rapidproto::rapidprotoc)
  if(CMAKE_CROSSCOMPILING)
    _rapidproto_require_host_tool(${_cli} ${target})
  endif()

  # ONE rapidprotoc invocation for the whole target: the entries resolve as a batch (shared
  # imports parse once, and a FIELD_MODES profile resolves against every proto's symbols at once,
  # so one profile can span the target's schemas). The cost: touching any listed proto re-runs
  # generation for the whole target.
  set(_protos_abs "")
  foreach(_proto IN LISTS RPG_PROTOS)
    get_filename_component(_proto_abs "${_proto}" ABSOLUTE)
    list(APPEND _protos_abs "${_proto_abs}")
  endforeach()

  # The OUTPUT list, two modes:
  #
  # ASK THE GENERATOR (find_package consumers -- the tool exists at configure time). The CLI's
  # `--list-outputs` dry-runs the real resolver and prints every path a generation would write, so
  # the declared list is exact by construction: imported schemas' headers, the embedded well-known
  # types' whole closure, and the runtime copies included. Predicting that list here instead means
  # mirroring the resolver's lexing and naming rules in CMake, which is how this helper once
  # accumulated ~380 lines that drifted from the CLI in six different ways. A schema error
  # surfaces right here at configure time, with the CLI's own diagnostic. `--list-inputs` is the
  # matching on-disk .proto closure: CONFIGURE_DEPENDS on it, so an added or removed import
  # re-runs this query instead of leaving the declared list stale. Sharing an OUT_DIR between two
  # targets is not a supported layout; the explicit overlap check below the two modes is what
  # refuses it (CMake's own conflict detection does not -- see there).
  #
  # ENTRIES-ONLY FALLBACK (the in-tree ALIAS and FetchContent -- this buildsystem builds the tool,
  # so there is nothing to ask yet). The listed schemas' headers, their common headers, and the
  # runtime copies (whose paths are constant) are declared; an IMPORTED schema's headers are
  # generated but undeclared, so deleting one of those needs a regeneration (touch an entry, or
  # rebuild from clean) to recover. This is the released behavior, kept deliberately small rather
  # than grown back into a resolver mirror.
  set(_outputs "")
  _rapidproto_tool_location(_cli_bin ${_cli})
  # An entry another build rule produces does not exist at configure time, so the generator cannot
  # be asked about it yet -- that shape takes the fallback, like a not-yet-built in-tree tool.
  # The cost, disclosed in the docs: a TYPO'D entry is indistinguishable from a generated one, so
  # its "entry file not found" surfaces at build rather than configure.
  if(_cli_bin)
    foreach(_proto_abs IN LISTS _protos_abs)
      if(NOT EXISTS "${_proto_abs}")
        set(_cli_bin "")
        break()
      endif()
    endforeach()
  endif()
  if(_cli_bin)
    # The queries carry exactly the generation command's content flags plus --out-dir, so a
    # refusal's diagnostic names the consumer's real directory. NOTE the newline->list split
    # cannot represent a path containing `;`; the CLI accepts such a .proto name, this helper
    # does not.
    execute_process(
      COMMAND "${_cli_bin}" ${_common} ${_model_flags} --out-dir "${RPG_OUT_DIR}"
              --list-outputs ${_protos_abs}
      OUTPUT_VARIABLE _listed ERROR_VARIABLE _list_err RESULT_VARIABLE _list_rc)
    if(NOT _list_rc EQUAL 0)
      # RESULT_VARIABLE included: a child that cannot even launch (non-executable file, a
      # target-arch binary) reports the reason there, with stderr empty.
      message(FATAL_ERROR
        "rapidproto_generate(${target}): ${_cli_bin} --list-outputs failed (${_list_rc}):\n"
        "${_list_err}")
    endif()
    string(REPLACE "\n" ";" _listed "${_listed}")
    foreach(_rel IN LISTS _listed)
      if(NOT _rel STREQUAL "")
        list(APPEND _outputs "${RPG_OUT_DIR}/${_rel}")
      endif()
    endforeach()
    execute_process(
      COMMAND "${_cli_bin}" ${_common} ${_model_flags} --out-dir "${RPG_OUT_DIR}"
              --list-inputs ${_protos_abs}
      OUTPUT_VARIABLE _inputs ERROR_VARIABLE _in_err RESULT_VARIABLE _in_rc)
    if(NOT _in_rc EQUAL 0)
      message(FATAL_ERROR
        "rapidproto_generate(${target}): ${_cli_bin} --list-inputs failed (${_in_rc}):\n"
        "${_in_err}")
    endif()
    string(REPLACE "\n" ";" _inputs "${_inputs}")
    # The BINARY is a configure input too: upgrading an installed rapidproto in place must re-run
    # the query, or a version emitting a different file set leaves the declared list stale.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs} "${_cli_bin}")
  else()
    foreach(_proto_abs IN LISTS _protos_abs)
      if("arena" IN_LIST _jobs)
        _rapidproto_output_header(_h "${_proto_abs}" ".rp.hpp" "${RPG_OUT_DIR}" "${_import_dirs_abs}")
        list(APPEND _outputs "${_h}")
      endif()
      if("stream" IN_LIST _jobs)
        _rapidproto_output_header(_h "${_proto_abs}" ".rp.stream.hpp" "${RPG_OUT_DIR}" "${_import_dirs_abs}")
        list(APPEND _outputs "${_h}")
      endif()
      if(RPG_DUMP)
        _rapidproto_output_header(_h "${_proto_abs}" ".rp.dump.hpp" "${RPG_OUT_DIR}" "${_import_dirs_abs}")
        list(APPEND _outputs "${_h}")
      endif()
      # The shared common header is an output too. Undeclared, deleting it did not re-run the
      # command -- the build stayed broken on `fatal error: <stem>.rp.common.hpp: No such file or
      # directory` until something else invalidated the batch.
      _rapidproto_output_header(_h "${_proto_abs}" ".rp.common.hpp" "${RPG_OUT_DIR}" "${_import_dirs_abs}")
      list(APPEND _outputs "${_h}")
    endforeach()
    # The runtime copies need no prediction -- their paths are constant -- so the fallback
    # declares them too.
    list(APPEND _outputs "${RPG_OUT_DIR}/rapidproto/runtime.hpp")
    if("arena" IN_LIST _jobs)
      list(APPEND _outputs "${RPG_OUT_DIR}/rapidproto/arena_runtime.hpp")
    endif()
    if(RPG_DUMP)
      list(APPEND _outputs "${RPG_OUT_DIR}/rapidproto/dump_runtime.hpp")
    endif()
  endif()
  # No two rapidproto_generate() calls may declare one path: CMake conflict-checks only a custom
  # command's FIRST output, so overlapping SECONDARY outputs (with a shared OUT_DIR, always at
  # least rapidproto/runtime.hpp) configure cleanly everywhere -- then Ninja hard-errors at build
  # ("multiple rules generate") while Make SILENTLY resolves them by whichever target builds last.
  # Refused in both modes, at configure -- give each target its own OUT_DIR.
  get_property(_taken GLOBAL PROPERTY _rapidproto_declared_outputs)
  foreach(_out IN LISTS _outputs)
    if("${_out}" IN_LIST _taken)
      message(FATAL_ERROR
        "rapidproto_generate(${target}): ${_out} is already generated by another "
        "rapidproto_generate() target -- two targets must not share an OUT_DIR")
    endif()
  endforeach()
  set_property(GLOBAL APPEND PROPERTY _rapidproto_declared_outputs ${_outputs})

  # Name the depfile off the first header -- in BOTH modes the first entry's first decoder header:
  # the CLI plans entries before imports and decoders before the common header precisely so that
  # this anchor is the depfile's own first target (Ninja accepts a depfile only when its first
  # target is the rule's first output) and a write_file output whose mtime advances every run
  # (Make re-runs a rule forever when its target stays older than an edited prerequisite, which a
  # skip-identical common header would). The CLI lists every entry's decoder header as a depfile
  # target (so each output node gets the import edges), and re-running regenerates the whole
  # batch.
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
