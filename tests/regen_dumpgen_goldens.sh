#!/usr/bin/env bash
#
# Regenerate the checked-in debug-dumper goldens (tests/dumpgen_golden/*.rp.dump.hpp) after an
# INTENTIONAL change to the debug generator -- then `./check.sh` to confirm, and review the diff.
#
#   tests/regen_dumpgen_goldens.sh
#
# Driven by rapidprotoc (--dump, which implies --arena) directly (not the test binary):
# test_dumpgen.cpp `#include`s these goldens (and they transitively #include the arena goldens they
# sit beside), so a change that makes the OLD ones no longer compile would block the test build --
# exactly when you need to regenerate. The CLI is build-independent of the goldens.
#
# --dump drops three files per entry into the out-dir: the debug dumper (.rp.dump.hpp), the arena
# decoder it #includes (.rp.hpp), and their shared common (.rp.common.hpp). All three are co-located
# in tests/dumpgen_golden/ so the compile-smoke's `#include "<stem>.rp.hpp"` (relative to the debug
# header) and `#include "<stem>.rp.common.hpp"` (relative to the arena header) both resolve here.
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS="$(nproc 2>/dev/null || echo 4)"
GOLDEN=tests/dumpgen_golden
BIN=build/gcc/rapidprotoc

echo "[1/3] building rapidprotoc ..."
cmake --preset gcc >/dev/null
cmake --build --preset gcc --target rapidprotoc -j"$JOBS" >/dev/null

echo "[2/3] regenerating dumpgen goldens via the CLI ..."
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

# Each corpus entry is single-file (no imports) so the CLI writes exactly <stem>.rp.dump.hpp (+ the
# arena/common it drops beside it).
for entry in arena_layout arena_manyreq arena_naming proto2 proto3 editions2023 editions2024 xref; do
  "$BIN" --dump -Itests/corpus --out-dir="$T" "tests/corpus/$entry.proto" >/dev/null
done
"$BIN" --dump -Itests/wire_fixtures --out-dir="$T" tests/wire_fixtures/wire_all.proto >/dev/null
# --unknown-present: every message reserves its has_unknown_fields() bit, so the dumper emits the
# "has_unknown_fields": true marker. The only golden built WITH the flag; decoded in test_dumpgen.
"$BIN" --dump --unknown-present -Itests/corpus --out-dir="$T" tests/corpus/arena_unknown.proto >/dev/null
# Field modes: generated under THE decode profile (tests/corpus/arena_modes.modes, which mirrors
# tests/arena_modes_profile.hpp) -- the same profile the arena golden uses; the dumper walks the
# resulting accessors (dropped fields gone, raw payloads as bytes).
"$BIN" --dump --field-modes=tests/corpus/arena_modes.modes -Itests/corpus --out-dir="$T" tests/corpus/arena_modes.proto >/dev/null
# Cross-file imports: main.proto pulls dep/pub/forward (distinct packages) into the closure -- each
# gets its own debug dumper (a debug header only #includes its own arena header, which transitively
# pulls the deps' arena headers).
"$BIN" --dump -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/main.proto >/dev/null
# Same package, two files: guards against the dumper being a single per-package entity (an ODR trap).
"$BIN" --dump -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/samepkg_a.proto >/dev/null
# Weak import: filtered like a standard import.
"$BIN" --dump -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/weakmain.proto >/dev/null
# --namespace-prefix + imports: the prefixed closure into a subdir, so its relative #includes resolve
# to the prefixed siblings (not the unprefixed ones, which share the same filenames).
"$BIN" --dump --namespace-prefix=rp -Itests/corpus/imports --out-dir="$T/prefixed" tests/corpus/imports/main.proto >/dev/null
# xref under a namespace prefix -> its own subdir golden, isolating its prefixed common header from
# the un-prefixed xref's common of the same stem (see regen_arenagen_goldens.sh).
"$BIN" --dump -Itests/corpus --namespace-prefix=rp --out-dir="$T/xref_prefixed" tests/corpus/xref.proto >/dev/null

# Copy a fresh version over every checked-in debug golden; a golden with no fresh version means this
# script needs a new entry above.
miss=0
while IFS= read -r g; do
  rel="${g#"$GOLDEN"/}"
  if [[ -f "$T/$rel" ]]; then
    cp "$T/$rel" "$g"
  else
    echo ">> MISSING in regen: $rel (add its entry to this script)"
    miss=1
  fi
done < <(find "$GOLDEN" -name '*.rp.dump.hpp')
[[ $miss -eq 0 ]] || exit 1

# Co-locate each debug header's arena decoder + shared common beside it (the debug header #includes
# the arena header, which #includes the common), so the compile-smoke resolves them relative to the
# debug header -- mirroring the CLI's real output (all three dropped side by side in one out-dir).
# Wipe first: unlike the curated .rp.dump.hpp list there's nothing to flag orphans against, so a
# proto dropped from this script must not leave a stale arena/common header behind.
find "$GOLDEN" \( -name '*.rp.hpp' -o -name '*.rp.common.hpp' \) -delete
while IFS= read -r c; do
  rel="${c#"$T"/}"
  mkdir -p "$GOLDEN/$(dirname "$rel")"
  cp "$c" "$GOLDEN/$rel"
done < <(find "$T" \( -name '*.rp.hpp' -o -name '*.rp.common.hpp' \))

echo "[3/3] done -- $(find "$GOLDEN" -name '*.rp.dump.hpp' | wc -l) dumpgen goldens regenerated."
echo "review the diff (git diff), then run ./check.sh to confirm."
