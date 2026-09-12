#!/usr/bin/env bash
# THROWAWAY perf-ladder campaign driver. One rung per invocation:
#   ./run-ladder.sh R0 | R1 | ... | R8 | LOO-threading | LOO-flatten | LOO-strings
# Reconfigures build/ladder with the rung's defines (flag change -> rapidprotoc rebuilds ->
# headers regenerate via the TARGET_FILE dependency), validates the emitted shape and the
# behavioral gates, then runs the full quiesced bench into bench_snapshots/ladder-<rung>.ndjson.
set -euo pipefail
cd "$(dirname "$0")/../../../../../.."  # -> /home/cvetter/rapidproto (adjust if moved)
cd /home/cvetter/rapidproto

RUNG="$1"
ALL_OFF="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES -DRP_LADDER_NO_FUSED_TAG \
 -DRP_LADDER_NO_PACKED_PRESIZE -DRP_LADDER_NO_LAYOUT -DRP_LADDER_NO_SWAR \
 -DRP_LADDER_COPY_STRINGS -DRP_FLATTEN= -DRP_NOINLINE="
case "$RUNG" in
  # Cumulative ladder: each rung REMOVES its own off-switch and everything below it.
  R0) DEFS="$ALL_OFF" ;;
  R1) DEFS="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES -DRP_LADDER_NO_FUSED_TAG -DRP_LADDER_NO_PACKED_PRESIZE -DRP_LADDER_NO_LAYOUT -DRP_LADDER_NO_SWAR -DRP_FLATTEN= -DRP_NOINLINE=" ;;
  R2) DEFS="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES -DRP_LADDER_NO_FUSED_TAG -DRP_LADDER_NO_PACKED_PRESIZE -DRP_LADDER_NO_SWAR -DRP_FLATTEN= -DRP_NOINLINE=" ;;
  R3) DEFS="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES -DRP_LADDER_NO_FUSED_TAG -DRP_LADDER_NO_SWAR -DRP_FLATTEN= -DRP_NOINLINE=" ;;
  R4) DEFS="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES -DRP_LADDER_NO_FUSED_TAG -DRP_LADDER_NO_SWAR" ;;
  R5) DEFS="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES -DRP_LADDER_NO_SWAR" ;;
  R6) DEFS="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES" ;;
  R7) DEFS="-DRP_LADDER_NO_PROBES" ;;
  R8) DEFS="" ;;
  # Leave-one-out cross-checks: full build minus exactly one thing.
  LOO-threading) DEFS="-DRP_LADDER_NO_THREADING -DRP_LADDER_NO_PROBES" ;;  # == R6 by def; alias kept for the article's table
  LOO-probes)    DEFS="-DRP_LADDER_NO_PROBES" ;;                            # == R7
  LOO-flatten)   DEFS="-DRP_FLATTEN= -DRP_NOINLINE=" ;;
  LOO-strings)   DEFS="-DRP_LADDER_COPY_STRINGS" ;;
  *) echo "unknown rung $RUNG" >&2; exit 2 ;;
esac

echo "=== $RUNG: configure + build ==="
cmake -S . -B build/ladder \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_CXX_FLAGS="-gsplit-dwarf $DEFS" \
  -DCMAKE_PREFIX_PATH=/home/cvetter/rapidproto/build/deps/protobuf-v25 >/dev/null
cmake --build build/ladder --target rapidprotoc rapidproto_diffgen -j"$(nproc)" >/dev/null

echo "=== $RUNG: emitted-shape assertions ==="
T=$(mktemp -d)
./build/ladder/rapidprotoc --arena --stream -Itests/corpus --out-dir="$T" tests/corpus/proto3.proto
want() { # want <count> <pattern>  (in the arena header)
  local got; got=$(grep -c "$2" "$T/proto3.rp.hpp" || true)
  if [ "$1" = "0" ] && [ "$got" != "0" ]; then echo "SHAPE FAIL: $2 present"; exit 1; fi
  if [ "$1" = "+" ] && [ "$got" = "0" ]; then echo "SHAPE FAIL: $2 absent"; exit 1; fi
}
case "$DEFS" in *NO_THREADING*) want 0 "rp_do_" ;; *) want + "rp_do_" ;; esac
case "$DEFS" in *NO_FUSED_TAG*) want 0 "read_tag_or_end" ;; *) want + "read_tag_or_end" ;; esac
case "$DEFS" in *NO_PACKED_PRESIZE*) want 0 "shrink_last" ;; *) want + "shrink_last" ;; esac
if [[ "$DEFS" != *NO_THREADING* ]]; then
  case "$DEFS" in *NO_PROBES*) want 0 "another element" ;; *) want + "another element" ;; esac
fi
rm -rf "$T"

echo "=== $RUNG: behavioral gates ==="
python3 tests/differential.py --build-dir build/ladder --cxx clang++-20 --messages 2000
cmake --build build/ladder --target rapidproto_tests -j"$(nproc)" >/dev/null
# Behavioral cases only: golden text-compare cases fail by design on a de-optimized generator.
# One combined spec (comma = OR of tag~exclusion terms): [shipgen] marks cases that compare
# against SHIPPED generation (fail by design on a de-optimized generator); [borrowpin] is the
# strings-borrow pin, excluded only on COPY_STRINGS rungs.
X="~[shipgen]"
case "$DEFS" in *COPY_STRINGS*) X="~[shipgen]~[borrowpin]" ;; esac
SPEC="[arena-decode]$X,[decode]$X,[streamgen]$X,[wire]$X"
TOUT=$(./build/ladder/rapidproto_tests "$SPEC" | grep -E "passed|failed" | tail -1)
echo "$TOUT"
[[ "$TOUT" == *"All tests passed"* ]] || { echo "TESTS FAIL"; exit 1; }

echo "=== $RUNG: quiesced bench ==="
python3 tests/bench.py run --build-dir build/ladder \
  --out "bench_snapshots/ladder-$RUNG.ndjson"
echo "=== $RUNG done ==="
