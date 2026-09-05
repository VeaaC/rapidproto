#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
#
# System-compiler build + test: THE one home for what a platform WITHOUT the pinned toolchain
# runs -- ci.yml's macos job and release.yml's macos leg both call this, the same one-home rule
# as check.sh's `compilers` mode for the arm64 legs. A separate script rather than a check.sh
# mode because check.sh needs bash >= 4.3 (namerefs, globstar) and macOS runners ship bash 3.2;
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

echo "=== tests ==="
./build/default/rapidproto_tests || exit 1

echo "=== compile-fail (generated decoders reject misuse) ==="
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
