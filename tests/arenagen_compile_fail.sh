#!/usr/bin/env bash
#
# Compile-fail harness for the generated arena decoder: each snippet below MUST fail to compile. The
# generated tree is READ-ONLY -- accessors return by value / const, there are no setters, storage is
# private, and a removed/renamed field simply does not exist. Run by check.sh.
#
#   tests/arenagen_compile_fail.sh [compiler]   # default compiler: c++
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${1:-c++}"
FLAGS=(-std=c++17 -fsyntax-only -I"${ROOT}/include" -I"${ROOT}/tests/arenagen_golden")
fail=0

# expect_fail <name> <expected-substring> <source> -- MUST fail to compile, and the diagnostic MUST
# contain <want> (so a snippet that fails for an unrelated reason is not mistaken for the rejection).
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

# expect_pass <name> <source> -- positive control: a correct snippet MUST compile.
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

expect_pass control-correct '
#include "proto3.rp.hpp"
void f(rapidproto::ByteView b, rapidproto::Arena& a) {
  const p3::Msg* m = p3::Msg::decode(b, a); (void)m->implicit_i(); }'

# A value accessor returns a prvalue -- it cannot be assigned to (the tree is read-only).
expect_fail assign-scalar "assign" '
#include "proto3.rp.hpp"
void f(rapidproto::ByteView b, rapidproto::Arena& a) {
  const p3::Msg* m = p3::Msg::decode(b, a); m->implicit_i() = 5; }'

# A repeated element is a const reference into the arena -- it cannot be mutated.
expect_fail mutate-repeated "assign" '
#include "proto3.rp.hpp"
void f(rapidproto::ByteView b, rapidproto::Arena& a) {
  const p3::Msg* m = p3::Msg::decode(b, a); m->nums()[0] = 9; }'

# There are no setters.
expect_fail no-setter "set_implicit_i" '
#include "proto3.rp.hpp"
void f(rapidproto::ByteView b, rapidproto::Arena& a) {
  p3::Msg* m = const_cast<p3::Msg*>(p3::Msg::decode(b, a)); m->set_implicit_i(5); }'

# A removed/renamed field does not exist.
expect_fail removed-field "no_such_field" '
#include "proto3.rp.hpp"
void f(rapidproto::ByteView b, rapidproto::Arena& a) {
  const p3::Msg* m = p3::Msg::decode(b, a); (void)m->no_such_field(); }'

# Storage is private -- it cannot be read directly (forces use of the accessor API).
expect_fail private-storage "m_implicit_i" '
#include "proto3.rp.hpp"
void f(rapidproto::ByteView b, rapidproto::Arena& a) {
  const p3::Msg* m = p3::Msg::decode(b, a); (void)m->m_implicit_i; }'

if [[ "$fail" == "0" ]]; then
  echo "arenagen compile-fail: all snippets correctly rejected"
fi
exit "$fail"
