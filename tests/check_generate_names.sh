#!/usr/bin/env bash
#
# The `names` stage's whole body: everything that keeps the CLI and the CMake helper agreeing
# about generated files, end to end.
#
#   1. Prediction parity -- in FALLBACK mode (no generator binary at configure) the helper must
#      PREDICT the header path the CLI actually writes; 13 shapes compare the two rules.
#   2. The CLI's refusals of inputs it cannot name unambiguously.
#   3. NAMESPACE_PREFIX plumbing through the helper, including the refusal of an empty one.
#   4. The declared-outputs contract in BOTH modes -- query (--list-outputs) delete/restore and
#      no-op rebuilds, and the fallback's reduced declaration set -- plus the configure-time
#      refusals (broken schema, broken install, shared OUT_DIR) and the generated-entry fallback.
#   5. A compile of generated output under a dotted member-reserved prefix.
#
# `rapidproto_generate()` declares those paths as a custom command's OUTPUT, so a disagreement is not
# a cosmetic difference: the declared output is never created, and the target regenerates on every
# build forever (Ninja does not even error). The two rules live in different languages --
# `_rapidproto_output_header` in cmake/rapidproto-generate.cmake, `canonical_entry_name` +
# `header_path` in the CLI -- so nothing but this check keeps them in step. They have drifted once:
# the helper resolved symlinks for its fallback while the CLI kept the spelling it was given, so a
# symlinked entry whose link name differed from its target's silently rebuilt forever.
#
# Each case below is a shape where the two rules could diverge. The check runs the real CLI, sees
# which header appeared, asks the real helper what it predicted, and compares.
#
#   tests/check_generate_names.sh <rapidprotoc>
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/gcc/rapidprotoc}"
HELPER="$ROOT/cmake/rapidproto-generate.cmake"

if [[ ! -x "$BIN" ]]; then
  echo ">> $BIN is not executable (build rapidprotoc first)"; exit 1
fi
# Absolute: the `relative` cases below run the CLI from $WORK, where a relative $BIN would not
# resolve.
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
if ! command -v cmake >/dev/null 2>&1; then
  echo ">> cmake not found"; exit 1
fi

WORK="$(mktemp -d "$ROOT/build/generate-names.XXXXXX")" || { echo ">> cannot create a work dir"; exit 1; }
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/real" "$WORK/link" "$WORK/nested/sub"
printf 'syntax = "proto3";\nmessage Aaa { int32 x = 1; }\n' >"$WORK/real/aaa.proto"
printf 'syntax = "proto3";\nmessage Sub { int32 x = 1; }\n' >"$WORK/nested/sub/deep.proto"
ln -s ../real/aaa.proto "$WORK/link/alias.proto"   # link name differs from its target's
ln -s aaa.proto "$WORK/real/sibling.proto"         # link beside its target, under the import dir
ln -sfn nested "$WORK/nesteddir_link"              # a symlinked import DIRECTORY
printf 'syntax = "proto3";\nmessage Dotted { int32 x = 1; }\n' >"$WORK/nested/a.proto.proto"

# case = <proto path>|<import dirs, space-separated>[|generated]. Paths are relative to $WORK and
# passed absolute, as rapidproto_generate does. `generated` means the entry does not exist when the
# helper predicts and is created before the CLI runs -- the shape of a .proto emitted by another
# build rule, where the helper sees a path CMake cannot resolve yet, and the only way to observe
# that REALPATH leaves an absent path unresolved while the CLI resolves it at build time.
#
# Every case must be able to FAIL. A symlinked import dir with a top-level entry cannot: the right
# answer and the fallback answer are both the basename, so that fixture is one directory deep.
cases=(
  "real/aaa.proto|real"                        # plain: entry under its import dir
  "nested/sub/deep.proto|nested"               # under the import dir, in a subdirectory
  "link/alias.proto|link"                      # symlinked entry, target OUTSIDE the dir -> fallback
  "real/sibling.proto|real"                    # symlinked entry resolving INSIDE the import dir
  "nesteddir_link/sub/deep.proto|nesteddir_link"  # symlinked import dir, entry one level down
  "real/aaa.proto|nested"                      # entry under an import dir that does not hold it
  "nesteddir_link/sub/gen.proto|nesteddir_link|generated"  # not on disk when the helper predicts
  "nesteddir_link/deep/x/gen2.proto|nesteddir_link|generated"  # ...with whole directories missing
  # Two overlapping import dirs: the rule is FIRST match in -I order, so these two cases differ only
  # in that order and pin both it and the loop's early exit. Without them a helper that scanned to
  # the last match, or dropped the break, stayed green.
  "nested/sub/deep.proto|nested nested/sub"
  "nested/sub/deep.proto|nested/sub nested"
  "nested/a.proto.proto|nested"                # ".proto" mid-name: only the trailing one is stripped
  # RELATIVE spellings. Every case above passes an absolute path, because that is what CMake does --
  # so none of them can see what the CLI does with a relative one, which is the half of the rule
  # that has no other guard. The CLI absolutises its entries so that an entry under no -I falls back
  # to a basename (what the helper predicts for the same file) rather than to the given path, which
  # became the header's location and could point outside the out-dir entirely.
  "real/aaa.proto|nested|relative"              # under no -I: basename, NOT `real/aaa.rp.hpp`
  "nested/sub/deep.proto|nested|relative"       # under an -I: keeps its import-relative subpath
)

fail=0
for case in "${cases[@]}"; do
  IFS='|' read -r rel_proto rel_import mode <<<"$case"
  proto="$WORK/$rel_proto"
  mode="${mode:-}"
  # One or more import dirs, in -I order; the helper takes them as a CMake list.
  imports=()
  for dir in $rel_import; do imports+=("$WORK/$dir"); done
  cli_includes=()
  for dir in "${imports[@]}"; do cli_includes+=(-I "$dir"); done
  helper_dirs=$(printf '%s;' "${imports[@]}"); helper_dirs="${helper_dirs%;}"

  # For a generated entry the helper must predict BEFORE the file exists, which is the whole point:
  # CMake resolves paths at configure time and the CLI at build time. Its DIRECTORIES must be absent
  # too -- REALPATH gives up on the whole path at the first missing component, so a case whose
  # parents already exist cannot see a helper that only resolves one level up.
  if [[ "$mode" == generated ]]; then
    rm -f "$proto"
    rmdir -p --ignore-fail-on-non-empty "$(dirname "$proto")" 2>/dev/null || true
  fi

  cat >"$WORK/predict.cmake" <<CMAKE
include("$HELPER")
_rapidproto_output_header(h "$proto" ".rp.hpp" "" "$helper_dirs")
message("\${h}")
CMAKE
  if ! predicted=$(cmake -P "$WORK/predict.cmake" 2>&1); then
    echo ">> the helper errored on $rel_proto:"; tail -3 <<<"$predicted"; fail=1; continue
  fi
  predicted="${predicted#/}"  # out_dir is empty above, so the result starts with the separator

  if [[ "$mode" == generated ]]; then
    mkdir -p "$(dirname "$proto")"
    printf 'syntax = "proto3";\nmessage Gen { int32 x = 1; }\n' >"$proto"
  fi

  out="$WORK/out"
  rm -rf "$out"
  if [[ "$mode" == relative ]]; then
    # Spelled relative, from $WORK -- the helper still gets the absolute path, as CMake gives it.
    rel_includes=()
    for dir in $rel_import; do rel_includes+=(-I "$dir"); done
    cli_log=$(cd "$WORK" && "$BIN" --arena "${rel_includes[@]}" --out-dir "$out" "$rel_proto" 2>&1)
  else
    cli_log=$("$BIN" --arena "${cli_includes[@]}" --out-dir "$out" "$proto" 2>&1)
  fi
  if [[ -n "$cli_log" ]] && grep -q "^error:" <<<"$cli_log"; then
    echo ">> the CLI failed on $rel_proto:"; tail -3 <<<"$cli_log"; fail=1; continue
  fi
  # The decoder header, relative to the out-dir. The CLI writes one per file in the closure, so
  # every case here uses an entry with no imports; the headers under rapidproto/ are the runtime it
  # also drops, never decoders.
  written=$(cd "$out" 2>/dev/null && find . -name '*.rp.hpp' -not -path './rapidproto/*' | sed 's|^\./||')
  if [[ -z "$written" ]]; then
    echo ">> the CLI wrote no decoder header for $rel_proto"; fail=1; continue
  fi
  if [[ $(wc -l <<<"$written") -ne 1 ]]; then
    echo ">> $rel_proto produced more than one decoder header; this check compares exactly one:"
    sed 's/^/   /' <<<"$written"; fail=1; continue
  fi

  if [[ "$written" != "$predicted" ]]; then
    echo ">> $rel_proto (-I $rel_import): the CLI wrote '$written', the helper predicted '$predicted'"
    echo "   A custom command declaring that output would never see it produced."
    fail=1
  fi
done

# ...and the CLI refuses what it cannot name unambiguously. None of the comparisons above can see
# these: they live in the driver, so removing the checks from main.cpp leaves every name match green
# while headers overwrite each other or land wherever a `..` points.
mkdir -p "$WORK/cwd" "$WORK/c1" "$WORK/c2" "$WORK/isub" "$WORK/iup"
printf 'syntax = "proto3";\nmessage C1 { int32 a = 1; }\n' >"$WORK/c1/dup.proto"
printf 'syntax = "proto3";\nmessage C2 { int32 b = 1; }\n' >"$WORK/c2/dup.proto"
printf 'syntax = "proto3";\nmessage U { int32 x = 1; }\n' >"$WORK/iup/u.proto"
printf 'syntax = "proto3";\nimport "../iup/u.proto";\nmessage R { U u = 1; }\n' >"$WORK/isub/root.proto"

# <label>|<expected message fragment>|<args...>
refusals=(
  "two schemas, one header|both generate|--out-dir $WORK/cwd/o1 $WORK/c1/dup.proto $WORK/c2/dup.proto"
  "two schemas, one name|relative to the include paths|-I $WORK/c1 -I $WORK/c2 --out-dir $WORK/cwd/o2 $WORK/c1/dup.proto $WORK/c2/dup.proto"
  "import escaping the out-dir|would write outside --out-dir|-I $WORK/isub --out-dir $WORK/cwd/o3 $WORK/isub/root.proto"
)
for spec in "${refusals[@]}"; do
  IFS='|' read -r label want args <<<"$spec"
  # shellcheck disable=SC2086  # args is a deliberately word-split argument list
  if "$BIN" --arena $args >"$WORK/refuse.log" 2>&1; then
    echo ">> $label: accepted, but it cannot be generated unambiguously"; fail=1
  elif ! grep -qF "$want" "$WORK/refuse.log"; then
    echo ">> $label: refused, but not with the expected diagnostic (wanted '$want'):"
    tail -3 "$WORK/refuse.log"; fail=1
  fi
done
# A refused run is inert: every check runs before the first write.
stray=$(find "$WORK/cwd" -name '*.rp*.hpp' 2>/dev/null)
if [[ -n "$stray" ]]; then
  echo ">> a refused run still wrote:"; sed 's/^/   /' <<<"$stray"; fail=1
fi

# Write a fixture project into <dir>: the generator target in one of its two real shapes --
# `imported` (find_package consumers: the query path) or `alias` (in-tree/FetchContent: the
# fallback path) -- or an explicit IMPORTED_LOCATION for broken-install shapes; then the include
# and the caller's lines. ONE authority for this boilerplate: it drifted once (an unnamespaced
# target name in a single fixture), invisible until the helper started introspecting the target.
write_fixture() {
  local dir="$1" shape="$2"
  shift 2
  mkdir -p "$dir"
  {
    echo 'cmake_minimum_required(VERSION 3.16)'
    echo 'project(fixture CXX)'  # a fixed name: a dir-derived one smuggles the dir's spelling
                                # (spaces, leaked variables) into project()'s language list
    case "$shape" in
      imported)
        echo "add_executable(rapidproto::rapidprotoc IMPORTED GLOBAL)"
        echo "set_target_properties(rapidproto::rapidprotoc PROPERTIES IMPORTED_LOCATION \"$BIN\")"
        ;;
      alias)
        printf 'int main() { return 0; }\n' >"$dir/dummy.cpp"
        echo "add_executable(rapidprotoc_local dummy.cpp)"
        echo "add_executable(rapidproto::rapidprotoc ALIAS rapidprotoc_local)"
        ;;
      *)  # an explicit location, e.g. a path that does not exist
        echo "add_executable(rapidproto::rapidprotoc IMPORTED GLOBAL)"
        echo "set_target_properties(rapidproto::rapidprotoc PROPERTIES IMPORTED_LOCATION \"$shape\")"
        ;;
    esac
    echo "include(\"$HELPER\")"
    printf '%s\n' "$@"
  } >"$dir/CMakeLists.txt"
}

# Configure <dir> expecting FAILURE whose output contains <fragment>; a clean configure or a
# different diagnostic is a finding. The WHY of each expected failure lives at the call site.
expect_configure_failure() {
  local dir="$1" fragment="$2" label="$3" out
  if out=$(cmake -S "$dir" -B "$dir/b" 2>&1); then
    echo ">> $label: configured cleanly, but must be refused"
    fail=1
  elif ! grep -q "$fragment" <<<"$out"; then
    echo ">> $label: refused, but not with the expected diagnostic ('$fragment'):"
    tail -3 <<<"$out"
    fail=1
  fi
}

# ...and the helper passes NAMESPACE_PREFIX through as the user wrote it. Nothing else in the tree
# uses that keyword, so both halves of it were unexercised: the value reaching the CLI at all (it was
# tested for TRUTH, and CMake reads `N`, `no`, `off`, `0` as false, so those were silently dropped
# and generation fell back to the default), and the refusal of an explicit empty one.
#
# Configure-only, then read the generated build system: that is what the flag ends up in, and it
# needs no compiler. A full build would cost minutes to check an argument.
cmake_case() {
  local label="$1" arg="$2" want="$3"
  # A separate statement on purpose: in `local a="$1" b="$a"`, bash expands BOTH words before
  # `local` assigns either, so `b` silently picks up whatever OUTER `a` leaked from an earlier
  # loop -- which is exactly how these three cases once shared one mislabeled directory.
  local dir="$WORK/cm_$label"
  mkdir -p "$dir"
  printf 'syntax = "proto3";\npackage cmp;\nmessage M { int32 x = 1; }\n' >"$dir/m.proto"
  write_fixture "$dir" imported "rapidproto_generate(gen PROTOS m.proto IMPORT_DIRS . $arg)"
  local out
  if ! out=$(cmake -S "$dir" -B "$dir/b" 2>&1); then
    if [[ "$want" == "REFUSED" ]]; then echo "ok   [cmake $label]"; return; fi
    echo ">> cmake $label: configure failed unexpectedly"; tail -3 <<<"$out"; fail=1; return
  fi
  if [[ "$want" == "REFUSED" ]]; then
    echo ">> cmake $label: accepted a value the CLI refuses"; fail=1; return
  fi
  if grep -rqF -- "$want" "$dir/b" --include=build.ninja --include=build.make --include=link.txt \
       --include='*.make' 2>/dev/null; then
    echo "ok   [cmake $label]"
  else
    echo ">> cmake $label: the generated build system never passes '$want'"; fail=1
  fi
}

cmake_case falsy-value   "NAMESPACE_PREFIX N"  "--namespace-prefix N"
cmake_case ordinary      "NAMESPACE_PREFIX my.decoders" "--namespace-prefix my.decoders"
cmake_case empty-refused 'NAMESPACE_PREFIX ""' REFUSED
# The other one-value keywords refuse an explicit empty the same way: `GENERATOR ${UNSET_VAR}`
# silently generated the arena default, and `OUT_DIR ""` silently used the private build dir.
cmake_case empty-generator 'GENERATOR ""' REFUSED
cmake_case empty-outdir    'OUT_DIR ""' REFUSED

# ── declared outputs (query mode) ──────────────────────────────────────────────────────────────
# With an IMPORTED generator -- every find_package consumer, and this fixture -- the helper asks
# the CLI (`--list-outputs`) for the exact output list, so EVERYTHING the CLI writes must be a
# declared OUTPUT: the listed schemas' headers, an imported schema's, the embedded well-known
# types' transitive closure (api.proto pulls in type/source_context/any), and the runtime copies.
# Both directions are checked end-to-end: a second build must be a no-op (an over-declared OUTPUT
# that is never written leaves the command permanently out of date, which also disarms the delete
# check below), and deleting EACH generated file must have the build put it back.
outputs_dir="$WORK/outputs"
mkdir -p "$outputs_dir/proto"
printf 'syntax = "proto3";\npackage d;\nenum K { K0 = 0; }\nmessage D { int32 x = 1; }\n' \
  >"$outputs_dir/proto/dep.proto"
printf 'syntax = "proto3";\npackage u;\nimport "dep.proto";\nimport "google/protobuf/api.proto";\nmessage U { d.D dd = 1; d.K k = 2; google.protobuf.Api a = 3; }\n' \
  >"$outputs_dir/proto/use.proto"
write_fixture "$outputs_dir" imported \
  'rapidproto_generate(schema PROTOS proto/use.proto IMPORT_DIRS proto GENERATOR both DUMP)'

# Ninja when available: it is the generator whose depfile handling rejects a wrong first OUTPUT
# (rebuild-forever), which Make cannot see -- a Make-only run of this fixture once passed while
# every Ninja consumer rebuilt everything on every build.
gen_flag=()
command -v ninja >/dev/null 2>&1 && gen_flag=(-G Ninja)
if ! cmake "${gen_flag[@]}" -S "$outputs_dir" -B "$outputs_dir/b" >/dev/null 2>&1 ||
   ! cmake --build "$outputs_dir/b" --target schema_generate >/dev/null 2>&1; then
  echo ">> declared outputs: the fixture project does not configure/build"; exit 1
fi
rebuild="$(cmake --build "$outputs_dir/b" --target schema_generate 2>&1)"
if grep -q "rapidproto: schema" <<<"$rebuild"; then
  echo ">> declared outputs: the target regenerates on every build -- a declared OUTPUT is never"
  echo "   written (or the depfile anchor is not the rule's first output), which also makes the"
  echo "   delete check below pass without testing anything"
  fail=1
fi
# ...and after an INPUT edit, exactly one regeneration: an anchor whose mtime does not advance
# with the run (a skip-identical shared header) leaves the target forever older than the edited
# proto, which is the Make spelling of the same defect.
touch "$outputs_dir/proto/use.proto"
cmake --build "$outputs_dir/b" --target schema_generate >/dev/null 2>&1
rebuild="$(cmake --build "$outputs_dir/b" --target schema_generate 2>&1)"
if grep -q "rapidproto: schema" <<<"$rebuild"; then
  echo ">> declared outputs: still regenerating on the SECOND build after an input edit -- the"
  echo "   anchor output's mtime did not advance with the run"
  fail=1
fi
mapfile -t generated < <(cd "$outputs_dir/b" && find . -name '*.hpp' | sort)
# A fixture that generated nothing would pass every check below without testing anything.
if [[ ${#generated[@]} -lt 20 ]]; then
  echo ">> declared outputs: expected the api.proto closure (>=20 headers), found ${#generated[@]}"
  exit 1
fi
for rel in "${generated[@]}"; do
  rm -f "$outputs_dir/b/$rel"
  cmake --build "$outputs_dir/b" --target schema_generate >/dev/null 2>&1
  if [[ ! -f "$outputs_dir/b/$rel" ]]; then
    echo ">> declared outputs: ${rel#./} is written by the CLI but is not a declared OUTPUT --"
    echo "   deleting it leaves the build permanently broken"
    fail=1
    # Restore for the rest of the loop: an undeclared deletion does not dirty the target, so only
    # an input edit re-runs the generator.
    touch "$outputs_dir/proto/use.proto"
    cmake --build "$outputs_dir/b" --target schema_generate >/dev/null 2>&1
  fi
done

# A schema error must surface AT CONFIGURE, with the CLI's own diagnostic: the query runs the real
# resolver, so a build system learns of a broken schema before any build starts.
badschema_dir="$WORK/badschema"
mkdir -p "$badschema_dir/proto"
printf 'syntax = "proto3";\nmessage B { unknown.Type t = 1; }\n' >"$badschema_dir/proto/bad.proto"
write_fixture "$badschema_dir" imported \
  'rapidproto_generate(schema PROTOS proto/bad.proto IMPORT_DIRS proto)'
expect_configure_failure "$badschema_dir" "unresolved type" "query mode, broken schema"

# Two targets sharing one OUT_DIR -- refused LOUDLY at configure, never resolved by whichever
# target happens to build last. DISJOINT schemas on purpose: their only overlap is the runtime
# copies, which are SECONDARY outputs that CMake's own conflict check never sees (it checks only
# a custom command's first OUTPUT) -- so this exact shape once configured cleanly everywhere,
# then Ninja hard-errored at build while Make silently kept whichever target built last.
clash_dir="$WORK/clash"
mkdir -p "$clash_dir/proto"
printf 'syntax = "proto3";\npackage a1;\nmessage A { int32 x = 1; }\n' >"$clash_dir/proto/a.proto"
printf 'syntax = "proto3";\npackage b1;\nmessage B { int32 x = 1; }\n' >"$clash_dir/proto/b.proto"
write_fixture "$clash_dir" imported \
  "rapidproto_generate(t1 PROTOS proto/a.proto IMPORT_DIRS proto OUT_DIR \"$clash_dir/gen\")" \
  "rapidproto_generate(t2 PROTOS proto/b.proto IMPORT_DIRS proto OUT_DIR \"$clash_dir/gen\")"
expect_configure_failure "$clash_dir" "must not share an OUT_DIR" "shared OUT_DIR (query mode)"

# A broken INSTALL -- an imported generator whose binary is missing -- must be a configure error,
# never a silent degrade to the smaller fallback declaration set.
broken_dir="$WORK/broken"
mkdir -p "$broken_dir/proto"
printf 'syntax = "proto3";\npackage k;\nmessage K { int32 x = 1; }\n' >"$broken_dir/proto/k.proto"
write_fixture "$broken_dir" "$WORK/does-not-exist/rapidprotoc" \
  'rapidproto_generate(schema PROTOS proto/k.proto IMPORT_DIRS proto)'
expect_configure_failure "$broken_dir" "broken or was moved" "broken install"

# An entry another build rule produces does not exist at configure, so the query cannot run --
# the helper must fall back (configure succeeds, entry headers predicted) rather than FATAL on a
# shape the fallback has always served.
genentry_dir="$WORK/genentry"
mkdir -p "$genentry_dir"
write_fixture "$genentry_dir" imported \
  'add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/made.proto"' \
  '  COMMAND "${CMAKE_COMMAND}" -E echo_append "" > /dev/null)' \
  'rapidproto_generate(schema PROTOS "${CMAKE_CURRENT_BINARY_DIR}/made.proto" IMPORT_DIRS .)'
if ! cmake -S "$genentry_dir" -B "$genentry_dir/b" >/dev/null 2>&1; then
  echo ">> generated entry: a rule-produced .proto (absent at configure) no longer configures --"
  echo "   the query must fall back for inputs that do not exist yet"
  fail=1
fi

# ── declared outputs (fallback mode) ────────────────────────────────────────────────────────────
# With a NON-IMPORTED generator (the in-tree ALIAS and FetchContent: this buildsystem builds the
# tool, so there is nothing to ask at configure), the helper falls back to declaring the LISTED
# schemas' headers, their commons, and the constant-path runtime copies -- and nothing else. The
# fixture builds the real shape: an ordinary executable target behind the rapidproto alias.
# Configure-only; the declaration set is decided at configure and read back from the build files.
fallback_dir="$WORK/fallback"
mkdir -p "$fallback_dir/proto"
printf 'syntax = "proto3";\npackage d;\nmessage D { int32 x = 1; }\n' >"$fallback_dir/proto/dep.proto"
printf 'syntax = "proto3";\npackage u;\nimport "dep.proto";\nmessage U { d.D d = 1; }\n' >"$fallback_dir/proto/use.proto"
write_fixture "$fallback_dir" alias \
  'rapidproto_generate(schema PROTOS proto/use.proto IMPORT_DIRS proto GENERATOR both)'
if ! cmake -S "$fallback_dir" -B "$fallback_dir/b" >/dev/null 2>&1; then
  echo ">> fallback mode: the fixture does not configure"; fail=1
else
  rules=$(cat "$fallback_dir/b/CMakeFiles/schema_generate.dir/build.make" 2>/dev/null \
            "$fallback_dir/b/build.ninja" 2>/dev/null)
  # ALL the constant runtime drops, not just the first: a fallback that forgot
  # arena_runtime.hpp used to pass this loop on runtime.hpp alone.
  for want in use.rp.hpp use.rp.stream.hpp use.rp.common.hpp \
              rapidproto/runtime.hpp rapidproto/arena_runtime.hpp; do
    if ! grep -q "$want" <<<"$rules"; then
      echo ">> fallback mode: $want is not a declared output"; fail=1
    fi
  done
  if grep -q "dep.rp" <<<"$rules"; then
    echo ">> fallback mode: an IMPORTED schema's header is declared -- the fallback has no way to"
    echo "   know the closure, so a declared import means a resolver mirror grew back"
    fail=1
  fi
fi

# DUMP in fallback mode declares the dump header AND dump_runtime.hpp (the third runtime drop).
fbdump_dir="$WORK/fbdump"
mkdir -p "$fbdump_dir/proto"
printf 'syntax = "proto3";\npackage fd;\nmessage M { int32 x = 1; }\n' >"$fbdump_dir/proto/d.proto"
write_fixture "$fbdump_dir" alias \
  'rapidproto_generate(schema PROTOS proto/d.proto IMPORT_DIRS proto GENERATOR arena DUMP)'
if ! cmake -S "$fbdump_dir" -B "$fbdump_dir/b" >/dev/null 2>&1; then
  echo ">> fallback dump: the fixture does not configure"; fail=1
else
  rules=$(cat "$fbdump_dir/b/CMakeFiles/schema_generate.dir/build.make" 2>/dev/null \
            "$fbdump_dir/b/build.ninja" 2>/dev/null)
  for want in d.rp.hpp d.rp.dump.hpp rapidproto/dump_runtime.hpp; do
    if ! grep -q "$want" <<<"$rules"; then
      echo ">> fallback dump: $want is not a declared output"; fail=1
    fi
  done
fi

# The shared-OUT_DIR refusal holds in FALLBACK mode too (the overlap check sits above the two
# modes' split): two non-imported-generator targets with one OUT_DIR must fail configure.
fbclash_dir="$WORK/fbclash"
mkdir -p "$fbclash_dir/proto"
printf 'syntax = "proto3";\npackage fa;\nmessage A { int32 x = 1; }\n' >"$fbclash_dir/proto/a.proto"
printf 'syntax = "proto3";\npackage fb;\nmessage B { int32 x = 1; }\n' >"$fbclash_dir/proto/b.proto"
write_fixture "$fbclash_dir" alias \
  "rapidproto_generate(t1 PROTOS proto/a.proto IMPORT_DIRS proto OUT_DIR \"$fbclash_dir/gen\")" \
  "rapidproto_generate(t2 PROTOS proto/b.proto IMPORT_DIRS proto OUT_DIR \"$fbclash_dir/gen\")"
expect_configure_failure "$fbclash_dir" "must not share an OUT_DIR" "shared OUT_DIR (fallback mode)"

# ── prefix verbatim + dotted, compiled ──────────────────────────────────────────────────────────
# A member-reserved word as a prefix component is accepted and emitted VERBATIM, and a dotted
# prefix splits into nested namespaces -- the unit tests pin both as strings only. This is the
# compile half: an unbalanced namespace or a mis-qualified reference in any generator would pass
# every substring check while producing uncompilable output.
pfx_cc=g++
command -v "$pfx_cc" >/dev/null 2>&1 || pfx_cc=c++
pfx_dir="$WORK/prefix_compile"
mkdir -p "$pfx_dir"
printf 'syntax = "proto3";\npackage pc;\nenum E { E0 = 0; }\nmessage M { int32 x = 1; E e = 2; M m = 3; }\n' >"$pfx_dir/m.proto"
if ! "$BIN" --arena --stream --dump --namespace-prefix Value.decode -I "$pfx_dir" --out-dir "$pfx_dir/gen" "$pfx_dir/m.proto" >/dev/null 2>&1; then
  echo ">> prefix compile: generation under --namespace-prefix Value.decode failed"; fail=1
else
  printf '#include "m.rp.hpp"\n#include "m.rp.stream.hpp"\n#include "m.rp.dump.hpp"\nint main() { return static_cast<int>(::Value::decode::common::pc::E::E0); }\n' >"$pfx_dir/tu.cpp"
  if ! "$pfx_cc" -std=gnu++17 -fsyntax-only -I "$pfx_dir/gen" "$pfx_dir/tu.cpp" 2>"$pfx_dir/cc.log"; then
    echo ">> prefix compile: output under a dotted member-reserved prefix does not compile:"
    head -5 "$pfx_dir/cc.log"; fail=1
  fi
fi

[[ $fail -eq 0 ]] || exit 1
echo "generate names: ${#cases[@]} entry shapes match the CLI, ${#refusals[@]} ambiguous ones refused, NAMESPACE_PREFIX passed through, ${#generated[@]} queried outputs restored when deleted, fallback + clash + configure-error shapes pinned, prefixed output compiled"
