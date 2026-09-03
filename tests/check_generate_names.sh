#!/usr/bin/env bash
#
# The CMake helper must predict the header path the CLI actually writes.
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

# ...and the helper passes NAMESPACE_PREFIX through as the user wrote it. Nothing else in the tree
# uses that keyword, so both halves of it were unexercised: the value reaching the CLI at all (it was
# tested for TRUTH, and CMake reads `N`, `no`, `off`, `0` as false, so those were silently dropped
# and generation fell back to the default), and the refusal of an explicit empty one.
#
# Configure-only, then read the generated build system: that is what the flag ends up in, and it
# needs no compiler. A full build would cost minutes to check an argument.
cmake_case() {
  local label="$1" arg="$2" want="$3" dir="$WORK/cm_$label"
  mkdir -p "$dir"
  printf 'syntax = "proto3";\npackage cmp;\nmessage M { int32 x = 1; }\n' >"$dir/m.proto"
  {
    echo 'cmake_minimum_required(VERSION 3.16)'
    echo 'project(cmp CXX)'
    echo "include(\"$ROOT/cmake/rapidproto-generate.cmake\")"
    echo "add_executable(rapidprotoc IMPORTED GLOBAL)"
    echo "set_target_properties(rapidprotoc PROPERTIES IMPORTED_LOCATION \"$BIN\")"
    echo "rapidproto_generate(gen PROTOS m.proto IMPORT_DIRS . $arg)"
  } >"$dir/CMakeLists.txt"
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

# ── declared outputs ────────────────────────────────────────────────────────────────────────────
# Every file the CLI writes must be a declared OUTPUT of the custom command. The cases above check
# WHERE a header lands; this checks that the helper declares ALL of them. Two sets have no entry in
# PROTOS and so were missed: an IMPORTED schema gets a full header set of its own, and the CLI drops
# its runtime beside the headers on every run.
#
# Deleting an undeclared output is unrecoverable in a way that reads like a broken checkout: the
# build fails on the missing include and keeps failing, because nothing tells the build system that
# generation should re-run. So the check is end-to-end rather than a comparison of two lists --
# delete each generated file in turn and require the build to put it back. That covers any cause,
# including ones no list comparison would model.
#
# The fixture imports a well-known type too -- and specifically api.proto, the embedded type with
# TRANSITIVE imports: the CLI writes header sets for type.proto, source_context.proto and (through
# type) any.proto as well, which the helper can only learn by reading the shipped wellknown
# sources. timestamp.proto would pass with that scan broken, being the shape with no imports.
#
# Both directions are checked. Deleting a file catches an UNDER-declaration; it cannot catch an
# OVER-declaration, and worse, an over-declared path disarms the delete loop entirely -- a declared
# output that is never written leaves the command permanently out of date, so every build re-runs it
# and restores whatever was deleted, whether or not it was declared. The second build below is the
# other half: it must do nothing.
outputs_dir="$WORK/outputs"
mkdir -p "$outputs_dir/proto"
printf 'syntax = "proto3";\npackage d;\nenum K { K0 = 0; }\nmessage D { int32 x = 1; }\n' \
  >"$outputs_dir/proto/dep.proto"
printf 'syntax = "proto3";\npackage u;\nimport "dep.proto";\nimport "google/protobuf/api.proto";\nmessage U { d.D dd = 1; d.K k = 2; google.protobuf.Api a = 3; }\n' \
  >"$outputs_dir/proto/use.proto"
{
  echo 'cmake_minimum_required(VERSION 3.16)'
  echo 'project(outputs CXX)'
  # The NAMESPACED name the helper actually invokes: unnamespaced, CMake leaves it unresolved and
  # the literal `rapidproto::rapidprotoc` reaches the makefile, where its colons are a syntax error.
  echo "add_executable(rapidproto::rapidprotoc IMPORTED GLOBAL)"
  echo "set_target_properties(rapidproto::rapidprotoc PROPERTIES IMPORTED_LOCATION \"$BIN\")"
  echo "include(\"$ROOT/cmake/rapidproto-generate.cmake\")"
  echo 'rapidproto_generate(schema PROTOS proto/use.proto IMPORT_DIRS proto GENERATOR both DUMP)'
} >"$outputs_dir/CMakeLists.txt"

if ! cmake -S "$outputs_dir" -B "$outputs_dir/b" >/dev/null 2>&1 ||
   ! cmake --build "$outputs_dir/b" --target schema_generate >/dev/null 2>&1; then
  echo ">> declared outputs: the fixture project does not configure/build"; exit 1
fi
# An over-declared output makes the command permanently out of date. Checked BEFORE the delete
# loop, because that is exactly what would make the loop pass vacuously.
rebuild="$(cmake --build "$outputs_dir/b" --target schema_generate 2>&1)"
if grep -q "rapidproto: schema" <<<"$rebuild"; then
  echo ">> declared outputs: the target regenerates on every build -- a declared OUTPUT is never"
  echo "   written, which also makes the delete check below pass without testing anything"
  fail=1
fi
mapfile -t generated < <(cd "$outputs_dir/b" && find . -name '*.hpp' | sort)
# A fixture that generated nothing would pass every check below without testing anything.
if [[ ${#generated[@]} -lt 8 ]]; then
  echo ">> declared outputs: expected at least 8 generated headers, found ${#generated[@]}"; exit 1
fi
for rel in "${generated[@]}"; do
  rm -f "$outputs_dir/b/$rel"
  cmake --build "$outputs_dir/b" --target schema_generate >/dev/null 2>&1
  if [[ ! -f "$outputs_dir/b/$rel" ]]; then
    echo ">> declared outputs: ${rel#./} is written by the CLI but is not a declared OUTPUT --"
    echo "   deleting it leaves the build permanently broken"
    fail=1
    # Restore for the rest of the loop. An input edit, not a bare rebuild: the deleted file being
    # UNDECLARED is the very failure just reported, so nothing is out of date and a plain rebuild
    # does not re-run the generator.
    touch "$outputs_dir/proto/use.proto"
    cmake --build "$outputs_dir/b" --target schema_generate >/dev/null 2>&1
  fi
done

# ── import-scanner shapes ───────────────────────────────────────────────────────────────────────
# The closure scan must read imports the way the CLI's lexer does. Three shapes pin it, each of
# which broke a different way with a regex-only scan: `//` INSIDE an import string is path, not
# comment (a stripper that cannot tell ate to end of line and spliced the next import into a
# phantom declared output -- a permanently out-of-date target); a single-quoted import is valid
# proto (invisible to a `"`-only match, so its headers went undeclared); and a block-commented
# import is NOT an import (declaring its headers is the phantom-output failure again). Checked
# end-to-end like everything above: the second build must be a no-op, deleted headers must come
# back, and the ghost must not exist.
scanner_dir="$WORK/scanner"
mkdir -p "$scanner_dir/proto/sub"
printf 'syntax = "proto3";\npackage sx;\nmessage X { int32 a = 1; }\n' >"$scanner_dir/proto/sub/x.proto"
printf 'syntax = "proto3";\npackage sq;\nmessage Q { int32 a = 1; }\n' >"$scanner_dir/proto/sq.proto"
{
  printf 'syntax = "proto3";\npackage u;\n'
  printf '/* a commented-out import:\nimport "ghost.proto";\n*/\n'
  printf 'import "sub//x.proto";\n'
  printf "import 'sq.proto';\n"
  printf 'message U { sx.X x = 1; sq.Q q = 2; }\n'
} >"$scanner_dir/proto/use.proto"
{
  echo 'cmake_minimum_required(VERSION 3.16)'
  echo 'project(scanner CXX)'
  echo "add_executable(rapidproto::rapidprotoc IMPORTED GLOBAL)"
  echo "set_target_properties(rapidproto::rapidprotoc PROPERTIES IMPORTED_LOCATION \"$BIN\")"
  echo "include(\"$ROOT/cmake/rapidproto-generate.cmake\")"
  echo 'rapidproto_generate(schema PROTOS proto/use.proto IMPORT_DIRS proto)'
} >"$scanner_dir/CMakeLists.txt"
if ! cmake -S "$scanner_dir" -B "$scanner_dir/b" >/dev/null 2>&1 ||
   ! cmake --build "$scanner_dir/b" --target schema_generate >/dev/null 2>&1; then
  echo ">> import scanner: the fixture project does not configure/build"; exit 1
fi
if cmake --build "$scanner_dir/b" --target schema_generate 2>&1 | grep -q "rapidproto: schema"; then
  echo ">> import scanner: the target regenerates on every build -- a scanner shape declared an"
  echo "   output the CLI never writes (a comment-eaten or ghost import)"
  fail=1
fi
for rel in rapidproto/schema/sub/x.rp.hpp rapidproto/schema/sq.rp.hpp; do
  rm -f "$scanner_dir/b/$rel"
  cmake --build "$scanner_dir/b" --target schema_generate >/dev/null 2>&1
  if [[ ! -f "$scanner_dir/b/$rel" ]]; then
    echo ">> import scanner: $rel was not restored -- its import spelling is invisible to the scan"
    fail=1
  fi
done
if compgen -G "$scanner_dir/b/rapidproto/schema/ghost*" >/dev/null; then
  echo ">> import scanner: a block-commented import produced output"; fail=1
fi

# ── shared out-dir ──────────────────────────────────────────────────────────────────────────────
# Two targets writing one OUT_DIR: legitimate sharing (same import, same flags) must configure,
# declare each shared file ONCE, and restore a deleted shared file when EITHER claimant is built
# -- while a stem COLLISION (same output path, different source or flags) must be a configure
# error, not a silent last-writer-wins overwrite.
share_dir="$WORK/share"
mkdir -p "$share_dir/proto"
printf 'syntax = "proto3";\npackage d;\nmessage D { int32 x = 1; }\n' >"$share_dir/proto/dep.proto"
printf 'syntax = "proto3";\npackage pa1;\nimport "dep.proto";\nmessage A { d.D d = 1; }\n' >"$share_dir/proto/a.proto"
printf 'syntax = "proto3";\npackage pb1;\nimport "dep.proto";\nmessage B { d.D d = 1; }\n' >"$share_dir/proto/b.proto"
{
  echo 'cmake_minimum_required(VERSION 3.16)'
  echo 'project(share CXX)'
  echo "add_executable(rapidproto::rapidprotoc IMPORTED GLOBAL)"
  echo "set_target_properties(rapidproto::rapidprotoc PROPERTIES IMPORTED_LOCATION \"$BIN\")"
  echo "include(\"$ROOT/cmake/rapidproto-generate.cmake\")"
  echo "rapidproto_generate(t1 PROTOS proto/a.proto IMPORT_DIRS proto OUT_DIR \"$share_dir/gen\")"
  echo "rapidproto_generate(t2 PROTOS proto/b.proto IMPORT_DIRS proto OUT_DIR \"$share_dir/gen\")"
} >"$share_dir/CMakeLists.txt"
if ! cmake -S "$share_dir" -B "$share_dir/b" >/dev/null 2>&1 ||
   ! cmake --build "$share_dir/b" >/dev/null 2>&1; then
  echo ">> shared out-dir: two targets legitimately sharing one OUT_DIR fail to configure/build"
  fail=1
else
  rm -f "$share_dir/gen/dep.rp.hpp" "$share_dir/gen/rapidproto/runtime.hpp"
  # Build only the SECOND claimant: the shared files belong to t1's command, so this passes only
  # through the cross-target dependency on the owner.
  cmake --build "$share_dir/b" --target t2_generate >/dev/null 2>&1
  for f in dep.rp.hpp rapidproto/runtime.hpp; do
    if [[ ! -f "$share_dir/gen/$f" ]]; then
      echo ">> shared out-dir: $f (owned by t1) was not restored by building t2 alone"
      fail=1
    fi
  done
fi
# The collision half: same stem, different schemas. Configure must refuse.
clash_dir="$WORK/clash"
mkdir -p "$clash_dir/c1" "$clash_dir/c2"
printf 'syntax = "proto3";\npackage m1;\nmessage M1 { int32 x = 1; }\n' >"$clash_dir/c1/dup.proto"
printf 'syntax = "proto3";\npackage m2;\nmessage M2 { int32 x = 1; }\n' >"$clash_dir/c2/dup.proto"
{
  echo 'cmake_minimum_required(VERSION 3.16)'
  echo 'project(clash CXX)'
  echo "add_executable(rapidproto::rapidprotoc IMPORTED GLOBAL)"
  echo "set_target_properties(rapidproto::rapidprotoc PROPERTIES IMPORTED_LOCATION \"$BIN\")"
  echo "include(\"$ROOT/cmake/rapidproto-generate.cmake\")"
  echo "rapidproto_generate(t1 PROTOS c1/dup.proto IMPORT_DIRS c1 OUT_DIR \"$clash_dir/gen\")"
  echo "rapidproto_generate(t2 PROTOS c2/dup.proto IMPORT_DIRS c2 OUT_DIR \"$clash_dir/gen\")"
} >"$clash_dir/CMakeLists.txt"
if clash_out=$(cmake -S "$clash_dir" -B "$clash_dir/b" 2>&1); then
  echo ">> shared out-dir: two DIFFERENT schemas colliding on one output path configured cleanly --"
  echo "   whichever target builds last silently overwrites the other's header"
  fail=1
elif ! grep -q "already generated by target" <<<"$clash_out"; then
  echo ">> shared out-dir: the stem collision was refused, but not by the claim registry:"
  tail -3 <<<"$clash_out"
  fail=1
fi

[[ $fail -eq 0 ]] || exit 1
echo "generate names: ${#cases[@]} entry shapes match the CLI, ${#refusals[@]} ambiguous ones refused, NAMESPACE_PREFIX passed through, ${#generated[@]} declared outputs restored when deleted, scanner + shared-out-dir shapes pinned"
