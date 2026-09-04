#!/usr/bin/env bash
#
# Compile-time stress benchmark for the generated dispatch gate. The gate emits ~O(fields *
# callbacks) compile-time work (a fold of static_asserts per field over the whole callback pack), and
# decode() is instantiated per call-site, so a message decoded with many specific callbacks is the
# worst case. This builds a message with N int32 fields and a consumer that decodes it with N
# specific callbacks, then either times the compile (default, MANUAL) or just checks it builds.
#
#   tests/streamgen_compile_bench.sh [compiler]          # compile + report wall-clock (manual)
#   tests/streamgen_compile_bench.sh --check [compiler]  # compile-only, for the gate (no timing)
#   N=128 tests/streamgen_compile_bench.sh               # scale the field/callback count
#
# Uniform int32 fields are intentional: the cost being measured is the dispatch cross-product
# (fields x callbacks), not value-type variety (the proto2/proto3/wire_all goldens cover that).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/gcc/rapidprotoc"
N="${N:-48}"
# Validated: N is what makes this a STRESS compile. `N=1 ./check.sh` printed "1-field x 1-callback
# decoder builds" and passed green with the dispatch-gate check doing nothing.
if [[ ! $N =~ ^[0-9]+$ ]] || [[ $((10#$N)) -lt 8 ]]; then
  echo "stress-bench: N must be an integer >= 8 (got '$N')" >&2
  exit 1
fi
N=$((10#$N))

mode="time"
CXX="c++"
for arg in "$@"; do
  case "$arg" in
    --check) mode="check" ;;
    *) CXX="$arg" ;;
  esac
done
command -v "$CXX" >/dev/null 2>&1 || CXX="c++"

if [[ ! -x "$BIN" ]]; then
  echo "stress-bench: $BIN not built (run the gcc build first)" >&2
  exit 1
fi

# Checked: this script runs under `set -uo pipefail` without -e, and it is called from the gate's
# gcc stage -- an unusable TMPDIR otherwise turned every later step into its own error against a
# path of `/`, burying the cause under four unrelated failures.
if ! tmp="$(mktemp -d)"; then
  echo "stress-bench: cannot create a temporary directory" >&2
  exit 1
fi
trap 'rm -rf "$tmp"' EXIT

# A message with N int32 fields.
{
  echo 'syntax = "proto3";'
  echo 'package stress;'
  echo 'message Stress {'
  for i in $(seq 1 "$N"); do echo "  int32 f$i = $i;"; done
  echo '}'
} >"$tmp/stress.proto"

if ! "$BIN" --stream -I"$tmp" --out-dir="$tmp/out" "$tmp/stress.proto" >/dev/null 2>"$tmp/gen_err"; then
  echo "stress-bench: failed to generate stress header:" >&2
  cat "$tmp/gen_err" >&2
  exit 1
fi

# A consumer that decodes Stress with one specific callback per field.
{
  echo '#include "stress.rp.stream.hpp"'
  echo '#include <cstdint>'
  echo 'long consume(rapidproto::ByteView b) {'
  echo '    long sum = 0;'
  echo '    (void)rp::stream::stress::Stress{b}.decode('
  for i in $(seq 1 "$N"); do
    printf '        [&](rp::stream::stress::Stress::f%d, std::int32_t v) { sum += v; }' "$i"
    if [[ "$i" -lt "$N" ]]; then echo ','; else echo ');'; fi
  done
  echo '    return sum;'
  echo '}'
} >"$tmp/consumer.cpp"

FLAGS=(-std=c++17 -fsyntax-only -I"$ROOT/include" -I"$tmp/out")

if [[ "$mode" == "check" ]]; then
  if "$CXX" "${FLAGS[@]}" "$tmp/consumer.cpp" 2>"$tmp/err"; then
    echo "stress-compile: ${N}-field x ${N}-callback decoder builds"
  else
    echo "stress-compile FAILED (${N} fields x ${N} callbacks):"
    head -20 "$tmp/err"
    exit 1
  fi
else
  echo "compiling a ${N}-field x ${N}-callback stress decoder with $CXX ..."
  if command -v /usr/bin/time >/dev/null 2>&1; then
    /usr/bin/time -v "$CXX" "${FLAGS[@]}" "$tmp/consumer.cpp" 2>&1 |
      grep -E "Elapsed \(wall clock\)|Maximum resident set size"
  else
    time "$CXX" "${FLAGS[@]}" "$tmp/consumer.cpp"
  fi
  echo "(scale with N=<count>; add -ftime-report to FLAGS for a per-pass breakdown)"
fi
