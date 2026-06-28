#!/usr/bin/env bash
#
# Compile-fail harness for the generated streaming decoder: each snippet below MUST fail to compile.
# The exact-match dispatch gate rejects a wrong-typed or duplicate callback, and a removed/renamed
# field tag simply does not exist -- proving the generated API is hard to misuse. Run by check.sh.
#
#   tests/streamgen_compile_fail.sh [compiler]   # default compiler: c++
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${1:-c++}"
FLAGS=(-std=c++17 -fsyntax-only -I"${ROOT}/include" -I"${ROOT}/tests/streamgen_golden")
fail=0

# expect_fail <name> <expected-substring> <source> -- the snippet MUST fail to compile, and the
# diagnostic MUST contain <want> (so a snippet that fails for an unrelated reason -- e.g. a broken
# include path -- is not mistaken for the intended rejection).
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

# expect_pass <name> <source> -- positive control: a correct snippet MUST compile, so a broken
# setup (missing header, wrong include path) is caught instead of masquerading as a rejection.
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
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}}; (void)m.decode([](p3::stream::Msg::a, std::int32_t) {}); }'

expect_fail wrong-value-type "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}}; (void)m.decode([](p3::stream::Msg::a, double) {}); }'

expect_fail widening-value-type "wrong value type" '
#include "proto2.rp.stream.hpp"
void f() { p2::stream::Scalars s{rapidproto::ByteView{}}; (void)s.decode([](p2::stream::Scalars::u32, std::uint64_t) {}); }'

expect_fail enum-as-int "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}}; (void)m.decode([](p3::stream::Msg::state, int) {}); }'

expect_fail wrapper-of-value "wrong value type" '
#include "proto3.rp.stream.hpp"
#include <optional>
void f() { p3::stream::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](p3::stream::Msg::a, std::optional<std::int32_t>) {}); }'

expect_fail duplicate-callback "more than one callback" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](p3::stream::Msg::a, std::int32_t) {}, [](p3::stream::Msg::a, std::int32_t) {}); }'

expect_fail map-wrong-value-type "wrong value type" '
#include "proto2.rp.stream.hpp"
void f() { p2::stream::Container c{rapidproto::ByteView{}};
           (void)c.decode([](p2::stream::Container::by_id, std::int32_t, int) {}); }'

expect_fail map-missing-value "wrong value type" '
#include "proto2.rp.stream.hpp"
void f() { p2::stream::Container c{rapidproto::ByteView{}};
           (void)c.decode([](p2::stream::Container::by_id, std::int32_t) {}); }'  # forgot the value param

# A partially-generic callback (auto in exactly one position) is rejected in BOTH directions.
expect_fail partial-generic-value "partially generic" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}}; (void)m.decode([](p3::stream::Msg::a, auto) {}); }'

expect_fail partial-generic-tag "partially generic" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}}; (void)m.decode([](auto, std::int32_t) {}); }'

# A catch-all sibling must NOT mask a mistyped concrete callback (per-callback wrong-type guard).
expect_fail catchall-masks-wrong-type "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](p3::stream::Msg::a, double) {}, [](auto, auto) {}); }'

# Positive control: a variadic catch-all (handles regular AND map arities) is a valid catch-all.
expect_pass variadic-catchall '
#include "proto2.rp.stream.hpp"
void f() { p2::stream::Container c{rapidproto::ByteView{}};
           (void)c.decode([](auto, auto&&...) {}); }'

# Positive control: a fixed-arity catch-all may introspect the tag (read kName/kNumber) in C++17.
expect_pass introspecting-catchall '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](auto tag, auto&&) { (void)tag.kNumber; (void)tag.kName; }); }'

# A generic callback with a non-const lvalue-ref value cannot receive the decoded (prvalue) value;
# it is reported, not silently skipped.
expect_fail lvalue-ref-value "partially generic" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}}; (void)m.decode([](auto, auto&) {}); }'

# Two catch-alls both match every field -> an ambiguous dispatch; rejected with a clear message.
expect_fail two-catch-alls "more than one catch-all" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](auto, auto) {}, [](auto, auto) {}); }'

expect_fail scalar-wrong-arity "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](p3::stream::Msg::implicit_i, std::int32_t, std::int32_t) {}); }'

expect_fail removed-field "no_such_field" '
#include "proto3.rp.stream.hpp"
void f() { p3::stream::Msg m{rapidproto::ByteView{}}; (void)m.decode([](p3::stream::Msg::no_such_field, int) {}); }'

if [[ "$fail" == "0" ]]; then
  echo "compile-fail: all snippets correctly rejected"
fi
exit "$fail"
