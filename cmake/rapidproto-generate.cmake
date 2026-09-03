# rapidproto-generate.cmake -- defines rapidproto_generate(), the helper that turns .proto schemas into
# a linkable, header-only target by driving the rapidproto code generators.
#
# It works the same way whether rapidproto is used in-tree (the main project include()s this file and the
# generator is the rapidproto::rapidprotoc ALIAS) or from an installed package
# (find_package(rapidproto) include()s the installed copy and the same target names are imported). The
# generated output is self-contained -- the CLIs drop a std-only copy of the runtime beside the headers,
# so a consumer needs only the OUT_DIR on its include path.

include_guard(GLOBAL)

# Where the well-known-type SOURCES live: beside this file's parent in the source tree, beside this
# file in an installed prefix. The CLI embeds wellknown/*.proto as `google/protobuf/<name>.proto`
# (wellknown/embed_wellknown.py), and two of them import others (type.proto, api.proto) -- so the
# import-closure scan below must be able to READ them, or the transitively pulled-in headers go
# undeclared. A GLOBAL property, not a variable: include_guard(GLOBAL) means this file runs in ONE
# directory scope, and a directory variable set here is invisible to rapidproto_generate() calls
# made from sibling directories.
foreach(_rapidproto_wk_cand "${CMAKE_CURRENT_LIST_DIR}/../wellknown" "${CMAKE_CURRENT_LIST_DIR}/wellknown")
  if(EXISTS "${_rapidproto_wk_cand}/type.proto")
    set_property(GLOBAL PROPERTY _rapidproto_wellknown_dir "${_rapidproto_wk_cand}")
    break()
  endif()
endforeach()
unset(_rapidproto_wk_cand)

# The header path the CLI writes for `proto_abs`, computed so it can be a custom command's OUTPUT. This
# mirrors the generator exactly (resolver.cpp canonical_entry_name + driver.hpp header_path): the entry's
# path relative to the first import dir that contains it, else its basename, with ".proto" -> `ext`,
# under `out_dir`. `import_dirs_abs` is the absolute import dirs in -I order.
# An import string, normalized the way canonical_import_path does (std::filesystem
# lexically_normal): "./dep.proto" -> "dep.proto", "a//b.proto" -> "a/b.proto". Purely lexical --
# rebasing on a synthetic root collapses `.` and `..` without touching the filesystem or resolving
# symlinks, which is what the CLI does too.
function(_rapidproto_normalize_import out_var import_string)
  get_filename_component(_abs "${import_string}" ABSOLUTE BASE_DIR "/rapidproto-import-base")
  file(RELATIVE_PATH _rel "/rapidproto-import-base" "${_abs}")
  set(${out_var} "${_rel}" PARENT_SCOPE)
endfunction()

# The transitive import closure of `protos_abs`, as IMPORT STRINGS (`out_imports`) plus every file
# scanned (`out_scanned`).
#
# The CLI writes a full header set for EVERY file in the resolved set, not only the listed ones, so
# an imported schema's headers are outputs as much as an entry's are. An undeclared header that is
# deleted never comes back: the build fails on the missing include and keeps failing, because
# nothing tells the build system that command should re-run.
#
# Import strings, not resolved paths, because that is what the header is NAMED from. An ENTRY is
# named by canonical_entry_name (resolve symlinks, relativize against the first import dir that
# holds it); an IMPORT is named by the import string itself -- canonical_import_path normalizes it
# and never rebases it on an import dir (src/rapidprotoc/main.cpp). Naming imports by the entry rule
# declares a path the CLI never writes whenever the two differ -- `IMPORT_DIRS inc inc/sub` with
# `import "dep.proto"` is enough -- and an output that is never written leaves the command
# permanently out of date: it regenerates on every build, forever, while the real header stays
# undeclared. Ninja does not even warn.
#
# Strip `//` and `/* */` comments from proto source WITHOUT reaching into string literals: `//`
# inside an import string (`import "sub//x.proto";` -- a spelling the CLI accepts) is path, not
# comment, and a stripper that cannot tell ate from mid-string to end of line, splicing the next
# import into a phantom output path with a newline in it -- a declared OUTPUT nothing ever writes,
# i.e. a target that regenerates on every build forever. Line-oriented, because a proto string
# literal cannot span lines; only the block-comment state crosses them. Backslash-escaped quotes
# inside a string are not modeled -- an import path containing a quote has no header path worth
# predicting.
function(_rapidproto_strip_comments out_var text)
  # Protect real semicolons before splitting into a line list, or every proto statement terminator
  # becomes a list separator.
  string(REPLACE ";" "\\;" _protected "${text}")
  string(REPLACE "\n" ";" _lines "${_protected}")
  set(_clean "")
  set(_in_block FALSE)
  foreach(_line IN LISTS _lines)
    set(_out "")
    while(NOT _line STREQUAL "")
      if(_in_block)
        string(FIND "${_line}" "*/" _pos)
        if(_pos EQUAL -1)
          set(_line "")
        else()
          math(EXPR _pos "${_pos} + 2")
          string(SUBSTRING "${_line}" ${_pos} -1 _line)
          set(_in_block FALSE)
        endif()
        continue()
      endif()
      # The EARLIEST of the four significant tokens decides: a quote before a `//` means the
      # slashes are inside the string, and vice versa.
      set(_next -1)
      set(_kind "")
      foreach(_tok "//" "/*" "\"" "'")
        string(FIND "${_line}" "${_tok}" _pos)
        if(NOT _pos EQUAL -1 AND (_next EQUAL -1 OR _pos LESS _next))
          set(_next ${_pos})
          set(_kind "${_tok}")
        endif()
      endforeach()
      if(_next EQUAL -1)
        string(APPEND _out "${_line}")
        set(_line "")
      elseif(_kind STREQUAL "//")
        string(SUBSTRING "${_line}" 0 ${_next} _head)
        string(APPEND _out "${_head}")
        set(_line "")
      elseif(_kind STREQUAL "/*")
        string(SUBSTRING "${_line}" 0 ${_next} _head)
        string(APPEND _out "${_head}")
        math(EXPR _skip "${_next} + 2")
        string(SUBSTRING "${_line}" ${_skip} -1 _line)
        set(_in_block TRUE)
      else()
        # A string literal: copy it through verbatim, up to the matching close quote.
        math(EXPR _start "${_next} + 1")
        string(SUBSTRING "${_line}" 0 ${_start} _head)
        string(APPEND _out "${_head}")
        string(SUBSTRING "${_line}" ${_start} -1 _rest)
        string(FIND "${_rest}" "${_kind}" _close)
        if(_close EQUAL -1)
          string(APPEND _out "${_rest}")  # unterminated: the CLI will reject the file anyway
          set(_line "")
        else()
          math(EXPR _len "${_close} + 1")
          string(SUBSTRING "${_rest}" 0 ${_len} _body)
          string(APPEND _out "${_body}")
          string(SUBSTRING "${_rest}" ${_len} -1 _line)
        endif()
      endif()
    endwhile()
    string(APPEND _clean "${_out}\n")
  endforeach()
  set(${out_var} "${_clean}" PARENT_SCOPE)
endfunction()

# The CLI's EMBEDDED well-known types need no special case for NAMING -- the header path follows
# from the import string alone -- but their own imports do: `api.proto` pulls in `type.proto`,
# `source_context.proto` and (through type) `any.proto`, and the CLI writes a header set for each.
# `wellknown_dir` is where their shipped sources live (see the probe at the top of this file; ""
# under NO_WELLKNOWN), consulted after the user's import dirs -- the same order as read_import.
function(_rapidproto_import_closure out_imports out_scanned protos_abs import_dirs_abs wellknown_dir)
  set(_imports "")
  set(_scanned "")
  set(_queue ${protos_abs})
  while(_queue)
    list(GET _queue 0 _cur)
    list(REMOVE_AT _queue 0)
    if("${_cur}" IN_LIST _scanned)
      continue()
    endif()
    list(APPEND _scanned "${_cur}")
    if(NOT EXISTS "${_cur}")
      continue()  # an entry another rule generates: not readable yet, and not ours to scan
    endif()
    # Comments are stripped before matching (string-aware -- see _rapidproto_strip_comments). A
    # commented-out import is not an import, and declaring its headers is the permanently-
    # out-of-date failure described above -- a `/* ... */` around an import statement is the
    # realistic way that happens.
    file(READ "${_cur}" _text)
    _rapidproto_strip_comments(_text "${_text}")
    # MATCHALL, not a per-line match: two imports on one line, and `import"x.proto";` with no space
    # after the keyword, are both valid proto that a line-anchored pattern misses. Both quote
    # styles: `import 'x.proto';` is valid proto too, and the CLI generates from it.
    string(REGEX MATCHALL "import[ \t\r\n]*(public|weak)?[ \t\r\n]*(\"[^\"]*\"|'[^']*')" _stmts
           "${_text}")
    foreach(_stmt IN LISTS _stmts)
      string(REGEX MATCH "[\"']([^\"']*)[\"']" _quoted "${_stmt}")
      _rapidproto_normalize_import(_imp "${CMAKE_MATCH_1}")
      if(_imp STREQUAL "")
        continue()
      endif()
      if(NOT "${_imp}" IN_LIST _imports)
        list(APPEND _imports "${_imp}")
      endif()
      set(_found FALSE)
      foreach(_dir IN LISTS import_dirs_abs)
        if(EXISTS "${_dir}/${_imp}")
          list(APPEND _queue "${_dir}/${_imp}")
          set(_found TRUE)
          break()  # first import dir that has it owns it, as read_import does
        endif()
      endforeach()
      # The embedded copy is flat: `google/protobuf/<name>.proto` ships as `<name>.proto`.
      if(NOT _found AND wellknown_dir AND _imp MATCHES "^google/protobuf/[^/]+\\.proto$")
        get_filename_component(_wk_name "${_imp}" NAME)
        if(EXISTS "${wellknown_dir}/${_wk_name}")
          list(APPEND _queue "${wellknown_dir}/${_wk_name}")
        endif()
      endif()
    endforeach()
  endwhile()
  set(${out_imports} "${_imports}" PARENT_SCOPE)
  set(${out_scanned} "${_scanned}" PARENT_SCOPE)
endfunction()

# Declare `path` as an output of THIS target, unless another rapidproto_generate() call already
# claimed it. Two targets writing to one OUT_DIR share the runtime headers, and share the headers of
# any schema they both import; declaring one file from two commands is a hard error under Ninja
# ("multiple rules generate ...") that configure does not catch, and a silently overridden recipe
# under Make.
#
# A path collision is only SHARING when both claimants would write the same bytes, which is what
# `fingerprint` pins: the resolved source the file is generated from, plus every content-shaping
# flag. Without it, two targets generating different schemas that happen to share a stem -- or the
# same schema under different NAMESPACE_PREFIXes -- "shared" the file, and whichever built last
# won, silently. That is worse than the hard error this function replaces, so a fingerprint
# mismatch is a configure error naming both targets.
#
# On a genuine share, the first claimer's command writes the file and the owner's name is appended
# to `out_owners`, so the caller can build-order itself after the owner -- a shared header deleted
# by hand must come back when EITHER claimant is built, not only the first.
#
# The registry is three parallel GLOBAL list properties indexed by the PATHS themselves, not
# per-path properties keyed on a mangling of the path: MAKE_C_IDENTIFIER collapses every
# non-identifier character to `_`, so `a-b.rp.hpp` and `a_b.rp.hpp` shared one key and the second
# file was silently never declared -- the very under-declaration this function exists to prevent.
function(_rapidproto_claim_output out_list out_owners path fingerprint target)
  # One list ELEMENT per fingerprint: a raw semicolon would split it and shear the three lists
  # out of alignment.
  string(REPLACE ";" "," _fp "${fingerprint}")
  get_property(_paths GLOBAL PROPERTY _rapidproto_claimed_outputs)
  list(FIND _paths "${path}" _idx)
  if(NOT _idx EQUAL -1)
    get_property(_fps GLOBAL PROPERTY _rapidproto_claimed_fingerprints)
    get_property(_owners GLOBAL PROPERTY _rapidproto_claimed_owners)
    list(GET _fps ${_idx} _prev_fp)
    list(GET _owners ${_idx} _owner)
    if(NOT _prev_fp STREQUAL "${_fp}")
      message(FATAL_ERROR
        "rapidproto_generate(${target}): ${path} is already generated by target '${_owner}', from "
        "a different source or under different flags. The two invocations would overwrite each "
        "other's header, decided by build order. Give the targets distinct OUT_DIRs, or make "
        "their sources and generation flags agree.")
    endif()
    set(_local "${${out_owners}}")
    list(APPEND _local "${_owner}")
    set(${out_owners} "${_local}" PARENT_SCOPE)
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY _rapidproto_claimed_outputs "${path}")
  set_property(GLOBAL APPEND PROPERTY _rapidproto_claimed_fingerprints "${_fp}")
  set_property(GLOBAL APPEND PROPERTY _rapidproto_claimed_owners "${target}")
  set(_local "${${out_list}}")
  list(APPEND _local "${path}")
  set(${out_list} "${_local}" PARENT_SCOPE)
endfunction()

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
  set(_outputs "")
  set(_protos_abs "")
  foreach(_proto IN LISTS RPG_PROTOS)
    get_filename_component(_proto_abs "${_proto}" ABSOLUTE)
    list(APPEND _protos_abs "${_proto_abs}")
  endforeach()
  set(_wellknown_dir "")
  if(NOT RPG_NO_WELLKNOWN)
    get_property(_wellknown_dir GLOBAL PROPERTY _rapidproto_wellknown_dir)
  endif()
  _rapidproto_import_closure(_import_strings _scanned_protos
    "${_protos_abs}" "${_import_dirs_abs}" "${_wellknown_dir}")
  # The import scan runs at CONFIGURE time, so CMake must re-run when an import statement changes.
  # Without this, adding an import leaves the new file's headers undeclared, and removing one leaves
  # a declared output nothing writes -- a target that regenerates on every build, forever.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_scanned_protos})

  set(_exts "")
  if("arena" IN_LIST _jobs)
    list(APPEND _exts ".rp.hpp")
  endif()
  if("stream" IN_LIST _jobs)
    list(APPEND _exts ".rp.stream.hpp")
  endif()
  if(RPG_DUMP)
    list(APPEND _exts ".rp.dump.hpp")
  endif()
  # The shared common header is an output too. Undeclared, deleting it did not re-run the command
  # -- the build stayed broken on `fatal error: <stem>.rp.common.hpp: No such file or directory`
  # until something else invalidated the batch. It carries every enum in the schema (nested ones
  # included), so it is load-bearing for nearly every schema rather than the few with a top-level
  # enum.
  list(APPEND _exts ".rp.common.hpp")

  # Everything that shapes a generated file's CONTENT besides its own source: the namespace prefix
  # and the arena-shaping flags. Joined with commas into one token -- it becomes part of each
  # claim's fingerprint (see _rapidproto_claim_output).
  list(JOIN _model_flags "," _flags_token)
  set(_content_flags "${RPG_NAMESPACE_PREFIX}|${_flags_token}")
  set(_shared_owners "")

  # Entries are named by canonical_entry_name, which _rapidproto_output_header mirrors; their
  # fingerprint carries the resolved entry path, so two targets listing the SAME schema with the
  # same flags share, and two different schemas colliding on a stem are a configure error.
  foreach(_proto_abs IN LISTS _protos_abs)
    get_filename_component(_entry_real "${_proto_abs}" REALPATH)
    foreach(_ext IN LISTS _exts)
      _rapidproto_output_header(_h "${_proto_abs}" "${_ext}" "${RPG_OUT_DIR}" "${_import_dirs_abs}")
      _rapidproto_claim_output(_outputs _shared_owners "${_h}"
        "${_entry_real}|${_content_flags}" "${target}")
    endforeach()
  endforeach()
  # Imports are named by the import STRING (see _rapidproto_import_closure), and fingerprinted by
  # what the string RESOLVES to -- two targets can share one import string while their IMPORT_DIRS
  # hand it different files, and that is a stem collision, not a share. A file that is both listed
  # and imported keeps its entry name (the resolver registers entries first), so its import
  # spelling is skipped rather than declared a second time under a different path; when the
  # entry's canonical name and the import string do agree, the entry claim above already owns the
  # path and this loop's claim is a same-fingerprint no-op.
  foreach(_imp IN LISTS _import_strings)
    set(_is_entry FALSE)
    set(_imp_source "embedded:${_imp}")  # unresolved = the CLI's embedded copy (or a build error)
    foreach(_dir IN LISTS _import_dirs_abs)
      if(EXISTS "${_dir}/${_imp}")
        get_filename_component(_imp_real "${_dir}/${_imp}" REALPATH)
        set(_imp_source "${_imp_real}")
        foreach(_proto_abs IN LISTS _protos_abs)
          get_filename_component(_entry_real "${_proto_abs}" REALPATH)
          if(_imp_real STREQUAL _entry_real)
            set(_is_entry TRUE)
          endif()
        endforeach()
        break()
      endif()
    endforeach()
    if(_is_entry)
      continue()
    endif()
    string(REGEX REPLACE "\\.proto$" "" _stem "${_imp}")
    foreach(_ext IN LISTS _exts)
      _rapidproto_claim_output(_outputs _shared_owners "${RPG_OUT_DIR}/${_stem}${_ext}"
        "${_imp_source}|${_content_flags}" "${target}")
    endforeach()
  endforeach()
  # The runtime the CLI drops beside the headers so the generated tree is self-contained (see the
  # file header). Written on every run, included by every generated header, and no flag turns it
  # off -- so it is an output on the same footing as the headers, for the same reason: deleted and
  # undeclared, it never comes back. Its content depends on nothing a target chooses, so the
  # fingerprint is a constant: any two targets sharing an OUT_DIR share these.
  _rapidproto_claim_output(_outputs _shared_owners "${RPG_OUT_DIR}/rapidproto/runtime.hpp"
    "runtime" "${target}")
  if("arena" IN_LIST _jobs)
    _rapidproto_claim_output(_outputs _shared_owners "${RPG_OUT_DIR}/rapidproto/arena_runtime.hpp"
      "runtime" "${target}")
  endif()
  if(RPG_DUMP)
    _rapidproto_claim_output(_outputs _shared_owners "${RPG_OUT_DIR}/rapidproto/dump_runtime.hpp"
      "runtime" "${target}")
  endif()
  if(NOT _outputs)
    message(FATAL_ERROR
      "rapidproto_generate(${target}): every output this target would declare is already claimed by "
      "another rapidproto_generate() call writing to ${RPG_OUT_DIR}")
  endif()

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
  # Shared files are declared by (and rebuilt through) their first claimer's command, so this
  # target must build-order itself after each owner: without the edge, `cmake --build --target
  # <this consumer>` leaves a hand-deleted shared header missing until something happens to build
  # the owner. Target-level dependencies, which work across directories on every generator --
  # file-level dependencies on another directory's custom-command output do not under Make.
  if(_shared_owners)
    list(REMOVE_DUPLICATES _shared_owners)
    foreach(_owner IN LISTS _shared_owners)
      add_dependencies(${target}_generate ${_owner}_generate)
    endforeach()
  endif()
  add_library(${target} INTERFACE)
  add_dependencies(${target} ${target}_generate)
  target_include_directories(${target} INTERFACE "${RPG_OUT_DIR}")
  target_compile_features(${target} INTERFACE cxx_std_17)
endfunction()
