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
# Shared expect_fail / expect_pass (and the CXX convention): tests/compile_fail_lib.sh.
source "$(dirname "$0")/compile_fail_lib.sh"
FLAGS=(-std=c++17 -fsyntax-only -I"${ROOT}/include" -I"${ROOT}/tests/streamgen_golden")
fail=0

# expect_fail <name> <expected-substring> <source> -- the snippet MUST fail to compile, and the
# diagnostic MUST contain <want> (so a snippet that fails for an unrelated reason -- e.g. a broken
# include path -- is not mistaken for the intended rejection).
# expect_pass <name> <source> -- positive control: a correct snippet MUST compile, so a broken
# setup (missing header, wrong include path) is caught instead of masquerading as a rejection.
expect_pass control-correct '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}}; (void)m.decode([](rp::stream::p3::Msg::a, std::int32_t) {}); }'

expect_fail wrong-value-type "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}}; (void)m.decode([](rp::stream::p3::Msg::a, double) {}); }'

expect_fail widening-value-type "wrong value type" '
#include "proto2.rp.stream.hpp"
void f() { rp::stream::p2::Scalars s{rapidproto::ByteView{}}; (void)s.decode([](rp::stream::p2::Scalars::u32, std::uint64_t) {}); }'

expect_fail enum-as-int "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}}; (void)m.decode([](rp::stream::p3::Msg::state, int) {}); }'

expect_fail wrapper-of-value "wrong value type" '
#include "proto3.rp.stream.hpp"
#include <optional>
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](rp::stream::p3::Msg::a, std::optional<std::int32_t>) {}); }'

expect_fail duplicate-callback "more than one callback" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](rp::stream::p3::Msg::a, std::int32_t) {}, [](rp::stream::p3::Msg::a, std::int32_t) {}); }'

expect_fail map-wrong-value-type "wrong value type" '
#include "proto2.rp.stream.hpp"
void f() { rp::stream::p2::Container c{rapidproto::ByteView{}};
           (void)c.decode([](rp::stream::p2::Container::by_id, std::int32_t, int) {}); }'

expect_fail map-missing-value "wrong value type" '
#include "proto2.rp.stream.hpp"
void f() { rp::stream::p2::Container c{rapidproto::ByteView{}};
           (void)c.decode([](rp::stream::p2::Container::by_id, std::int32_t) {}); }'  # forgot the value param

# A partially-generic callback (auto in exactly one position) is rejected in BOTH directions.
expect_fail partial-generic-value "partially generic" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}}; (void)m.decode([](rp::stream::p3::Msg::a, auto) {}); }'

expect_fail partial-generic-tag "partially generic" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}}; (void)m.decode([](auto, std::int32_t) {}); }'

# A catch-all sibling must NOT mask a mistyped concrete callback (per-callback wrong-type guard).
expect_fail catchall-masks-wrong-type "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](rp::stream::p3::Msg::a, double) {}, [](auto, auto) {}); }'

# Positive control: a variadic catch-all (handles regular AND map arities) is a valid catch-all.
expect_pass variadic-catchall '
#include "proto2.rp.stream.hpp"
void f() { rp::stream::p2::Container c{rapidproto::ByteView{}};
           (void)c.decode([](auto, auto&&...) {}); }'

# Positive control: a fixed-arity catch-all may introspect the tag (read kName/kNumber) in C++17.
expect_pass introspecting-catchall '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](auto tag, auto&&) { (void)tag.kNumber; (void)tag.kName; }); }'

# A generic callback with a non-const lvalue-ref value cannot receive the decoded (prvalue) value;
# it is reported, not silently skipped.
expect_fail lvalue-ref-value "partially generic" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}}; (void)m.decode([](auto, auto&) {}); }'

# Two catch-alls both match every field -> an ambiguous dispatch; rejected with a clear message.
expect_fail two-catch-alls "more than one catch-all" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](auto, auto) {}, [](auto, auto) {}); }'

expect_fail scalar-wrong-arity "wrong value type" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](rp::stream::p3::Msg::implicit_i, std::int32_t, std::int32_t) {}); }'

expect_fail removed-field "no_such_field" '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}}; (void)m.decode([](rp::stream::p3::Msg::no_such_field, int) {}); }'

# A callback for ANOTHER message's field can never fire here; rejected instead of silently ignored.
expect_fail cross-message-callback "matches no field" '
#include "proto2.rp.stream.hpp"
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](rp::stream::p2::Scalars::u32, std::uint32_t) {}); }'

# Positive control: an UnknownField-only handler is a valid (non-stray) callback set.
expect_pass unknown-only '
#include "proto3.rp.stream.hpp"
void f() { rp::stream::p3::Msg m{rapidproto::ByteView{}};
           (void)m.decode([](rapidproto::UnknownField) {}); }'

if [[ "$fail" == "0" ]]; then
  echo "compile-fail: all snippets correctly rejected"
fi
exit "$fail"
