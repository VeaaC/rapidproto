# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
#
# Shared machinery for the three regen scripts (regen_goldens.sh, regen_arenagen_goldens.sh,
# regen_dumpgen_goldens.sh). Two blocks used to be copied per script and had already drifted
# (one copy passed -DRAPIDPROTO_BUILD_TESTS=ON, the others relied on the cached default):
# the stale-binary defence around building rapidprotoc, and the sync-the-goldens tail.
# Sourced, not executed; callers run under `set -euo pipefail` from the repo root.

# Build a fresh rapidprotoc, refusing to proceed on a renamed/dropped target. `cmake --build
# --target X` degenerates to `make X` under Makefiles, so a renamed target with build/gcc/X
# still on disk prints "Nothing to be done" and exits 0 -- and every golden would then be
# regenerated from that stale binary. `-DRAPIDPROTO_BUILD_TESTS=ON` explicitly: the option
# defaults ON for a top-level build, but a cache configured otherwise must not silently decide.
ensure_rapidprotoc() {
  cmake --preset gcc -DRAPIDPROTO_BUILD_TESTS=ON >/dev/null
  if ! grep -qE '(^|\.\.\. )rapidprotoc$' <<<"$(cmake --build --preset gcc --target help 2>/dev/null)"; then
    echo ">> 'rapidprotoc' is not a target of build/gcc -- the goldens would be regenerated from a" >&2
    echo "   stale binary. Re-run cmake --preset gcc." >&2
    exit 1
  fi
  cmake --build --preset gcc --target rapidprotoc -j"${JOBS:-$(nproc)}" >/dev/null
}

# Sync freshly generated files over the checked-in goldens:
#   sync_goldens <golden-dir> <tmp-dir> <primary-glob> [colocate-glob...]
# Copies a fresh version over every checked-in PRIMARY golden (subdirs preserved), failing loudly
# when a checked-in golden was not regenerated (the script needs a new entry) and when the find
# matched NOTHING (a moved directory or stale pattern used to report "0 goldens regenerated" and
# exit 0). Then wipes and re-copies the COLOCATE files (shared commons, the dump goldens' arena
# siblings): those have no curated list to flag orphans against, so a proto dropped from the
# script must not leave a stale sibling behind.
sync_goldens() {
  local golden=$1 tmp=$2 primary=$3
  shift 3
  local miss=0 g rel c
  while IFS= read -r g; do
    rel="${g#"$golden"/}"
    if [[ -f "$tmp/$rel" ]]; then
      cp "$tmp/$rel" "$g"
    else
      echo ">> MISSING in regen: $rel (add its entry to this script)"
      miss=1
    fi
  done < <(find "$golden" -name "$primary")
  if [[ $(find "$golden" -name "$primary" | wc -l) -eq 0 ]]; then
    echo ">> no goldens matching $primary found under $golden -- nothing was regenerated" >&2
    return 1
  fi
  [[ $miss -eq 0 ]] || return 1
  if [[ $# -gt 0 ]]; then
    local find_expr=(-name "$1")
    shift
    local glob
    for glob in "$@"; do
      find_expr+=(-o -name "$glob")
    done
    find "$golden" \( "${find_expr[@]}" \) -delete
    while IFS= read -r c; do
      rel="${c#"$tmp"/}"
      mkdir -p "$golden/$(dirname "$rel")"
      cp "$c" "$golden/$rel"
    done < <(find "$tmp" \( "${find_expr[@]}" \))
  fi
}
