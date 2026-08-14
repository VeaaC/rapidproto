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
  if ! cli_log=$("$BIN" --arena "${cli_includes[@]}" --out-dir "$out" "$proto" 2>&1); then
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

[[ $fail -eq 0 ]] || exit 1
echo "generate names: ${#cases[@]} entry shapes, helper prediction matches the CLI"
