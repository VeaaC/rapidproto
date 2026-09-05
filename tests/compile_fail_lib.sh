# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
#
# Shared helpers for the three compile-fail scripts (arenagen/streamgen/dumpgen_compile_fail.sh).
# `expect_fail` was byte-identical in all three; `expect_pass` had reached only two -- dumpgen
# hand-rolled its positive control WITHOUT capturing compiler output (a successful control still
# spilled chatter into the caller's log) and defaulted CXX differently (g++ where the others
# took the system c++; unification settled all three on ${1:-c++}, a behavior change for a
# bare dumpgen_compile_fail.sh run on hosts where c++ is not g++ -- the gate always passes an
# explicit compiler). Sourced, not executed; callers provide $CXX, $FLAGS[] and $fail.

# expect_fail <name> <expected-message-fragment> <source> -- the snippet must FAIL to compile,
# and with the intended diagnostic (an unrelated error must not pass for the guard firing).
expect_fail() {
  local name="$1" want="$2" src="$3" err
  if err=$(printf '%s\n' "$src" | "$CXX" "${FLAGS[@]}" -xc++ - 2>&1); then
    echo "FAIL [$name]: expected a compile error, but it compiled"
    fail=1
  elif ! grep -qF "$want" <<<"$err"; then
    echo "FAIL [$name]: failed to compile but without the expected message '$want'"
    fail=1
  else
    echo "ok   [$name]"
  fi
}

# expect_pass <name> <source> -- positive control: a correct snippet MUST compile. Without one, a
# harness that never compiles anything reports all-clear.
expect_pass() {
  local name="$1" src="$2" err
  if err=$(printf '%s\n' "$src" | "$CXX" "${FLAGS[@]}" -xc++ - 2>&1); then
    echo "ok   [$name]"
  else
    echo "FAIL [$name]: a correct snippet failed to compile (broken setup?):"
    echo "$err" | head -3
    fail=1
  fi
}
