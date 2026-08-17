#!/usr/bin/env bash
#
# Regenerate every checked-in golden after an INTENTIONAL change to the generator, the AST dumper, or
# the wire dumper -- then `./check.sh` to confirm, and review the diff.
#
#   tests/regen_goldens.sh
#
# Why a script instead of just `RAPIDPROTO_REGEN_GOLDEN=1 ./build/gcc/rapidproto_tests`:
# test_streamgen.cpp `#include`s the generated headers, so when a change makes the OLD streamgen
# goldens no longer compile (e.g. a runtime symbol was renamed), the test binary won't build --
# exactly when you need to regenerate. This drives rapidprotoc (--stream) DIRECTLY (build-independent) for
# those goldens, then runs the test binary for the AST/wire goldens (which it can build once the
# streamgen goldens are fresh).
#
# NOTE: the AST goldens have no behavioral backstop (see test_golden.cpp) -- review the diff.
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS="$(nproc 2>/dev/null || echo 4)"
GOLDEN=tests/streamgen_golden
BIN=build/gcc/rapidprotoc

echo "[1/5] building rapidprotoc ..."
cmake --preset gcc -DRAPIDPROTO_BUILD_TESTS=ON >/dev/null
# Target checked before building: `cmake --build --target X` degenerates to `make X` under
# Makefiles, so a renamed target with build/gcc/X still on disk prints "Nothing to be done" and
# exits 0 -- and every golden below would then be regenerated from that stale binary.
if ! grep -qE '(^|\.\.\. )rapidprotoc$' <<<"$(cmake --build --preset gcc --target help 2>/dev/null)"; then
  echo ">> 'rapidprotoc' is not a target of build/gcc -- the goldens would be regenerated from a" >&2
  echo "   stale binary. Re-run cmake --preset gcc." >&2
  exit 1
fi
cmake --build --preset gcc --target rapidprotoc -j"$JOBS" >/dev/null

echo "[2/5] regenerating streamgen goldens via the CLI ..."
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

# Generate each entry's whole import closure into $T. Shared imports are regenerated (identically) by
# every entry that depends on them; the WKT closure comes from usewkt; cross-file deps from main /
# weakmain. xref_prefixed is xref re-generated under a namespace prefix into its own subdir.
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/proto2.proto >/dev/null
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/proto3.proto >/dev/null
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/xref.proto >/dev/null
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/naming.proto >/dev/null
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/usewkt.proto >/dev/null
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/packed.proto >/dev/null
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/editions2023.proto >/dev/null
"$BIN" --stream -Itests/corpus --out-dir="$T" tests/corpus/editions2024.proto >/dev/null
"$BIN" --stream -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/main.proto >/dev/null
"$BIN" --stream -Itests/corpus/imports --out-dir="$T" tests/corpus/imports/weakmain.proto >/dev/null
# Package SHAPES the rest of the corpus never exercises (every other entry has a single-component
# package): a dotted package -> namespace com::example::deep::stream, and NO package at all -> a
# top-level `namespace stream`. xpkg pulls deep, pinning a cross-file reference into a dotted package.
"$BIN" --stream -Itests/corpus/nsedge --out-dir="$T" tests/corpus/nsedge/nopkg.proto >/dev/null
"$BIN" --stream -Itests/corpus/nsedge --out-dir="$T" tests/corpus/nsedge/xpkg.proto >/dev/null
# A package named `std` -> `namespace std_`, never `namespace std` (which would be UB).
"$BIN" --stream -Itests/corpus/nsedge --out-dir="$T" tests/corpus/nsedge/stdpkg.proto >/dev/null
# A package named `rapidproto` -> `namespace rapidproto_`; unescaped it merges the schema's
# types into the runtime's own namespace.
"$BIN" --stream -Itests/corpus/nsedge --out-dir="$T" tests/corpus/nsedge/rppkg.proto >/dev/null
"$BIN" --stream -Itests/wire_fixtures --out-dir="$T" tests/wire_fixtures/wire_all.proto >/dev/null
# xref under a namespace prefix -> its own subdir golden, isolating its prefixed common header (rp::xr
# enums) from the un-prefixed xref's common of the same stem (see regen_arenagen_goldens.sh).
"$BIN" --stream -Itests/corpus --namespace-prefix=pfx --out-dir="$T/xref_prefixed" tests/corpus/xref.proto >/dev/null

# Copy a fresh version over every currently-checked-in golden (preserving subdirs). Fail loudly if an
# existing golden was not regenerated -- a new golden means this script needs a new entry above.
miss=0
while IFS= read -r g; do
  rel="${g#"$GOLDEN"/}"
  if [[ -f "$T/$rel" ]]; then
    cp "$T/$rel" "$g"
  else
    echo ">> MISSING in regen: $rel (add its entry to this script)"
    miss=1
  fi
done < <(find "$GOLDEN" -name '*.rp.stream.hpp')
# A zero-match find regenerates nothing and reports success: with the goldens moved or the
# name pattern stale, this printed "0 streamgen goldens regenerated" and exited 0.
if [[ $(find "$GOLDEN" -name '*.rp.stream.hpp' | wc -l) -eq 0 ]]; then
  echo ">> no streamgen goldens found under $GOLDEN -- nothing was regenerated" >&2
  exit 1
fi
[[ $miss -eq 0 ]] || exit 1
echo "    $(find "$GOLDEN" -name '*.rp.stream.hpp' | wc -l) streamgen goldens regenerated"

# Co-locate each streaming decoder's shared common header beside it (the decoder #includes its own), so
# the compile-smoke resolves it -- mirroring the CLI's real output (decoder + common side by side in one
# out-dir). Wipe first: unlike the decoders above there's no curated list to flag orphans against, so a
# proto dropped from this script must not leave a stale common behind.
find "$GOLDEN" -name '*.rp.common.hpp' -delete
while IFS= read -r c; do
  rel="${c#"$T"/}"
  mkdir -p "$GOLDEN/$(dirname "$rel")"
  cp "$c" "$GOLDEN/$rel"
done < <(find "$T" -name '*.rp.common.hpp')

echo "[3/5] regenerating arenagen + dumpgen goldens via rapidprotoc --arena / --dump ..."
# Same chicken-and-egg as streamgen (test_arenagen.cpp / test_dumpgen.cpp #include these), so drive
# the CLI directly.
tests/regen_arenagen_goldens.sh >/dev/null
tests/regen_dumpgen_goldens.sh >/dev/null

echo "[4/5] building the test binary (the fresh streamgen + arenagen + dumpgen goldens now compile) ..."
# Target checked before building: `cmake --build --target X` degenerates to `make X` under
# Makefiles, so a renamed target with build/gcc/X still on disk prints "Nothing to be done" and
# exits 0 -- and this script would then rewrite EVERY golden from that stale binary.
if ! grep -qE '(^|\.\.\. )rapidproto_tests$' <<<"$(cmake --build --preset gcc --target help 2>/dev/null)"; then
  echo ">> 'rapidproto_tests' is not a target of build/gcc -- the goldens would be regenerated" >&2
  echo "   from a stale binary. Re-run cmake --preset gcc." >&2
  exit 1
fi
cmake --build --preset gcc --target rapidproto_tests -j"$JOBS" >/dev/null

echo "[5/5] regenerating AST + wire + arena-layout + common goldens via the test binary ..."
# Status kept: `| grep -i regenerated || true` discarded it, so a suite that crashed part-way
# through regeneration reported the goldens as regenerated and left the rest at their old contents.
# `|| regen_rc=$?` and not a following `regen_rc=$?`: this script runs under errexit, which aborts
# at a failing assignment, so the separate-statement form made the branch below unreachable.
regen_rc=0
regen_out=$(RAPIDPROTO_REGEN_GOLDEN=1 ./build/gcc/rapidproto_tests \
  "[golden],[wire-golden],[arena-layout],[common]" 2>&1) || regen_rc=$?
grep -i "regenerated" <<<"$regen_out" || true
if [[ $regen_rc -ne 0 ]]; then
  echo ">> golden regeneration failed (exit $regen_rc) -- goldens are incomplete:"
  tail -20 <<<"$regen_out"
  exit 1
fi

# [4b/5] Coexistence goldens: BOTH models from ONE invocation into ONE dir, which is what a consumer
# does and the only shape with a single shared common header. Generating them per-model dir instead
# gives two copies of that header, and a TU including both fails on duplicate enum definitions --
# which is exactly what tests/test_coexistence.cpp exists to hold in one TU.
echo "[4b/5] coexistence goldens (both models, one out-dir)"
rm -rf tests/coexist_golden
# Listed one per line, not looped over stems: check_fixture_coverage.sh greps this file for each
# fixture's path, and a loop variable would hide them from it.
for entry in tests/corpus/nsedge/rootnames.proto \
             tests/corpus/nsedge/rootenum.proto \
             tests/corpus/nsedge/sibparent.proto \
             tests/corpus/nsedge/sibpkg.proto; do
  "$BIN" --arena --stream -Itests/corpus/nsedge --out-dir=tests/coexist_golden "$entry" >/dev/null
done
rm -rf tests/coexist_golden/rapidproto  # the runtime copy; the test TU uses the repo's own headers

# The reverse of the orphan check at [2/5], which only flags a golden that exists but was not
# regenerated: this catches a fixture with no golden at all. Shared with check.sh (the `fixtures`
# gate stage, which CI runs) so it holds even when nobody runs this script.
tests/check_fixture_coverage.sh

echo "done -- review the diff (git diff), then run ./check.sh to confirm."
