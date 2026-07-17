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
cmake --preset gcc >/dev/null
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
"$BIN" --stream -Itests/wire_fixtures --out-dir="$T" tests/wire_fixtures/wire_all.proto >/dev/null
# xref under a namespace prefix -> its own subdir golden, isolating its prefixed common header (rp::xr
# enums) from the un-prefixed xref's common of the same stem (see regen_arenagen_goldens.sh).
"$BIN" --stream -Itests/corpus --namespace-prefix=rp --out-dir="$T/xref_prefixed" tests/corpus/xref.proto >/dev/null

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

echo "[3/5] regenerating arenagen + debuggen goldens via rapidprotoc --arena / --debug ..."
# Same chicken-and-egg as streamgen (test_arenagen.cpp / test_debuggen.cpp #include these), so drive
# the CLI directly.
tests/regen_arenagen_goldens.sh >/dev/null
tests/regen_debuggen_goldens.sh >/dev/null

echo "[4/5] building the test binary (the fresh streamgen + arenagen + debuggen goldens now compile) ..."
cmake --build --preset gcc --target rapidproto_tests -j"$JOBS" >/dev/null

echo "[5/5] regenerating AST + wire + arena-layout + common goldens via the test binary ..."
RAPIDPROTO_REGEN_GOLDEN=1 ./build/gcc/rapidproto_tests "[golden],[wire-golden],[arena-layout],[common]" 2>&1 |
  grep -i "regenerated" || true

echo "done -- review the diff (git diff), then run ./check.sh to confirm."
