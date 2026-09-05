#!/usr/bin/env bash
#
# The dumper's misuse guard: `rapidproto::dump` dispatches through a `dump_detail::dumper<T>`
# specialization the generated header supplies, and the primary template is DEFINED with a
# static_assert so a type without one reads as a sentence rather than "incomplete type ... used in
# nested name specifier". That message is the only thing standing between a user and a template
# error, so it needs a test that fails when it stops firing -- or when someone turns the primary
# back into a declaration.
#
# Headers are generated fresh rather than taken from tests/*_golden/: reaching a streaming type AND
# a dump header means two golden dirs on one include path, and their shared `*.rp.common.hpp` are
# byte-identical, which clang rejects as a redefinition (see tests/check_fixture_coverage.sh).
#
#   tests/dumpgen_compile_fail.sh [compiler]   # default compiler: c++
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${1:-c++}"
# Shared expect_fail / expect_pass (and the CXX convention): tests/compile_fail_lib.sh.
source "$(dirname "$0")/compile_fail_lib.sh"
# Overridable so a caller that built elsewhere tests ITS binary, not a stale build/gcc one --
# tests/system_build_test.sh points this at build/default (same shape as check_generate_names.sh).
BIN="${RAPIDPROTOC:-$ROOT/build/gcc/rapidprotoc}"

if [[ ! -x "$BIN" ]]; then
  echo ">> $BIN is not executable (build rapidprotoc first)"; exit 1
fi
# A template, not bare `mktemp -d`: the no-argument form is a GNU extension; BSD/macOS mktemp
# requires one, and this script is on the macOS CI path.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/rp-dumpgen.XXXXXX")" || { echo ">> cannot create a work dir"; exit 1; }
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/d.proto" <<'PROTO'
syntax = "proto3";
package df;
message M { int32 x = 1; }
PROTO
"$BIN" --arena --stream --dump -I "$WORK" --out-dir "$WORK/gen" "$WORK/d.proto" >/dev/null || {
  echo ">> generation failed"; exit 1; }

# ONLY the out-dir: the CLI drops a complete runtime beside the generated headers, so adding the
# repo's include/ too puts two copies of every runtime header on the path. gcc's content-keyed
# `#pragma once` collapses them; clang rejects the second as a redefinition. The out-dir alone is
# what a consumer actually compiles against.
FLAGS=(-std=c++17 -fsyntax-only -I"$WORK/gen")
fail=0

# A streaming type has no dumper: the models are one root segment apart, so this is easy to reach.
expect_fail streaming-type "no generated dumper for this type" '
#include "d.rp.hpp"
#include "d.rp.stream.hpp"
#include "d.rp.dump.hpp"
void f(rp::stream::df::M m) { (void)rapidproto::dump(m); }
'

# The likelier slip: arena decode() returns a POINTER, and dump takes the message.
expect_fail pointer-not-reference "no generated dumper for this type" '
#include "d.rp.hpp"
#include "d.rp.dump.hpp"
void f(const rp::arena::df::M* p) { (void)rapidproto::dump(p); }
'

# The positive control: without it, a harness that never compiles anything reports all-clear.
expect_pass control-correct '
#include "d.rp.hpp"
#include "d.rp.dump.hpp"
void f(const rp::arena::df::M& m) { (void)rapidproto::dump(m); }'

[[ $fail -eq 0 ]] || exit 1
echo "dumpgen compile-fail: misuse rejected with the intended message"
