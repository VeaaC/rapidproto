#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
#
# System-compiler build + test: THE one home for what a platform WITHOUT the pinned toolchain
# runs -- ci.yml's macos job and release.yml's macos leg both call this, the same one-home rule
# as check.sh's `compilers` mode for the arm64 legs. A separate script rather than a check.sh
# mode because check.sh needs bash >= 4.4 (namerefs, globstar, mapfile -d) and macOS runners ship bash 3.2;
# everything here is 3.2-clean.
#
#   tests/system_build_test.sh
#
# Configure + build the `default` preset (system compiler, Debug -- asserts active), run the full
# test suite, then the three compile-fail harnesses against `c++`. Deliberately NO warnings gate:
# the system compiler is whatever the image ships (AppleClang on macOS runners), and an unpinned
# compiler's new warnings must not fail CI or block a release -- the pinned gcc-13/clang-20 jobs
# own the warning surface (the same rationale as release.yml's WERROR-off release build).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

echo "=== configure + build (preset: default, system compiler) ==="
cmake --preset default -DRAPIDPROTO_BUILD_TESTS=ON || exit 1
cmake --build --preset default -j"$JOBS" || exit 1

echo "=== tests (ctest: unit suite + both example decodes) ==="
out=$(ctest --preset default 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then printf '%s\n' "$out" | tail -3; else printf '%s\n' "$out"; exit 1; fi
# Anti-vacuity: a dropped or renamed test target shrinks the ctest list silently -- green with
# less run. Floor at the three registered tests (unit + the two consumer-example decodes).
total=$(printf '%s\n' "$out" | sed -n 's/.*tests passed, .* out of \([0-9][0-9]*\).*/\1/p')
if [ -z "$total" ] || [ "$total" -lt 3 ]; then
  echo ">> expected at least 3 ctest tests, saw '${total:-none}' -- a test target went missing"
  exit 1
fi

echo "=== compile-fail (generated decoders reject misuse) ==="
# THIS build's generator, asserted before the loop: without the export the dumpgen script falls
# back to build/gcc/rapidprotoc -- absent on macOS, and a STALE gcc-13 binary on a Linux dev box.
RAPIDPROTOC="$PWD/build/default/rapidprotoc"
export RAPIDPROTOC
[ -x "$RAPIDPROTOC" ] || { echo ">> $RAPIDPROTOC is not executable"; exit 1; }
rc=0
for script in streamgen_compile_fail arenagen_compile_fail dumpgen_compile_fail; do
  # Quiet on success (one summary line), full output on failure -- job_compile_fail's shape.
  if out=$("tests/$script.sh" c++ 2>&1); then
    printf '%s\n' "$out" | tail -1
  else
    printf '%s\n' "$out"
    rc=1
  fi
done
[ "$rc" -eq 0 ] && echo "system-compiler build/test OK"
exit "$rc"
