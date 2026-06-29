#!/usr/bin/env bash
#
# Regenerate the checked-in arena-generator goldens (tests/arenagen_golden/*.rp.hpp) after an
# INTENTIONAL change to the arena generator -- then `./check.sh` to confirm, and review the diff.
#
#   tests/regen_arenagen_goldens.sh
#
# Driven by rapidprotoc (--arena) directly (not the test binary): test_arenagen.cpp `#include`s
# these goldens, so a change that makes the OLD ones no longer compile would block the test build --
# exactly when you need to regenerate. The CLI is build-independent of the goldens.
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS="$(nproc 2>/dev/null || echo 4)"
GOLDEN=tests/arenagen_golden
BIN=build/gcc/rapidprotoc

echo "[1/3] building rapidprotoc ..."
cmake --preset gcc >/dev/null
cmake --build --preset gcc --target rapidprotoc -j"$JOBS" >/dev/null

echo "[2/3] regenerating arenagen goldens via the CLI ..."
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

# Each corpus entry is single-file (no imports) so the CLI writes exactly <stem>.rp.hpp.
for entry in arena_layout arena_manyreq arena_naming proto2 proto3 editions2023 xref; do
  "$BIN" --arena -Itests/corpus --out-dir="$T" "tests/corpus/$entry.proto" >/dev/null
done
"$BIN" --arena -Itests/wire_fixtures --out-dir="$T" tests/wire_fixtures/wire_all.proto >/dev/null
# --unknown-present: a collapsed bool-wrapper must still carry its own has_unknown_fields() -- an extra
# bit in the parent's mask (presence + value + wrapper-unknown). The only golden built WITH the flag, so
# it is its own line; decoded in test_arena_decode.
"$BIN" --arena --unknown-present -Itests/corpus --out-dir="$T" tests/corpus/arena_unknown.proto >/dev/null
# Cross-file imports: main.proto pulls dep/pub/forward (distinct packages) into the closure -- guards
# cross-file message-field decoding (decoding must reach an imported type's decoder).
"$BIN" --arena -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/main.proto >/dev/null
# Same package, two files: guards against the decoder being a single per-package entity (an ODR trap).
"$BIN" --arena -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/samepkg_a.proto >/dev/null
# Weak import: filtered like a standard import (the field type stays usable), as in streamgen.
"$BIN" --arena -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/weakmain.proto >/dev/null
# --namespace-prefix + imports: the prefixed closure into a subdir, so its relative #includes resolve
# to the prefixed siblings (not the unprefixed ones, which share the same filenames).
"$BIN" --arena --namespace-prefix=rp -Itests/corpus/imports --out-dir="$T/prefixed" tests/corpus/imports/main.proto >/dev/null
# xref under a namespace prefix -> its own subdir golden. It must be a subdir (not a flat
# xref_prefixed.rp.hpp) so its prefixed common header (holding rp::xr enums) stays isolated: the decoder
# includes its common by stem ("xref.rp.common.hpp"), which on a flat layout would collide with the
# un-prefixed xref's common of the same name.
"$BIN" --arena -Itests/corpus --namespace-prefix=rp --out-dir="$T/xref_prefixed" tests/corpus/xref.proto >/dev/null

# Copy a fresh version over every checked-in golden; a golden with no fresh version means this script
# needs a new entry above.
miss=0
while IFS= read -r g; do
  rel="${g#"$GOLDEN"/}"
  if [[ -f "$T/$rel" ]]; then
    cp "$T/$rel" "$g"
  else
    echo ">> MISSING in regen: $rel (add its entry to this script)"
    miss=1
  fi
done < <(find "$GOLDEN" -name '*.rp.hpp')
[[ $miss -eq 0 ]] || exit 1

# Co-locate each file's shared common header (the decoder #includes its own) beside the decoder
# goldens, so the compile-smoke resolves it relative to the decoder -- mirroring the CLI's real output
# (decoder + common dropped side by side in one out-dir). Wipe first: unlike the decoders above there's
# no curated golden list to flag orphans against, so a proto dropped from this script must not leave a
# stale common behind.
find "$GOLDEN" -name '*.rp.common.hpp' -delete
while IFS= read -r c; do
  rel="${c#"$T"/}"
  mkdir -p "$GOLDEN/$(dirname "$rel")"
  cp "$c" "$GOLDEN/$rel"
done < <(find "$T" -name '*.rp.common.hpp')

echo "[3/3] done -- $(find "$GOLDEN" -name '*.rp.hpp' | wc -l) arenagen goldens regenerated."
echo "review the diff (git diff), then run ./check.sh to confirm."
