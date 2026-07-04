#!/usr/bin/env bash
#
# Link harness for the field-modes ODR guard: two TUs that exchange a generated type MUST link
# when generated under the SAME decode profile and MUST FAIL to link under different profiles --
# the inline `rp_modes_<id>` namespace makes the profile part of the mangled type identity, so a
# mismatch surfaces as an undefined reference instead of a silent ODR violation.
#
#   tests/arena_modes_link.sh <rapidprotoc> [compiler]   # default compiler: c++
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$1"
CXX="${2:-c++}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail=0

# Two generations of the SAME schema under different profiles: the checked-in `lean` profile
# (named), and a smaller inline profile (unnamed -> hashed id), so the two ids cannot collide.
"$BIN" --arena --field-modes="$ROOT/tests/corpus/arena_modes.modes" \
  -I"$ROOT/tests/corpus" --out-dir="$T/lean" "$ROOT/tests/corpus/arena_modes.proto" >/dev/null
"$BIN" --arena '--drop=fm.Holder.debug' '--raw=fm.Blob' \
  -I"$ROOT/tests/corpus" --out-dir="$T/other" "$ROOT/tests/corpus/arena_modes.proto" >/dev/null

# provider.cpp defines holder_must(const fm::Holder*); consumer.cpp decodes (the decoder is
# header-inline, so each TU has its own) and passes the tree across. The Holder must be a
# PARAMETER: mangling ignores an ordinary function's return type, so only a parameter carries
# the rp_modes namespace into the symbol. The shared prototype spells the type as fm::Holder,
# which each TU resolves through its own header's inline namespace.
cat >"$T/provider.cpp" <<'EOF'
#include "arena_modes.rp.hpp"
int holder_must(const fm::Holder* h) { return h != nullptr ? h->must() : -1; }
EOF
cat >"$T/consumer.cpp" <<'EOF'
#include "arena_modes.rp.hpp"
int holder_must(const fm::Holder* h);
int main() {
    rapidproto::Arena arena;
    // must=1 and a req_blob record: the minimal wire bytes a Holder decode accepts.
    const char wire[] = {0x28, 0x01, 0x6A, 0x00};
    const fm::Holder* h = fm::Holder::decode(rapidproto::ByteView(wire, sizeof wire), arena);
    return holder_must(h) == 1 ? 0 : 1;
}
EOF

FLAGS=(-std=c++17 -I"$ROOT/include")
"$CXX" "${FLAGS[@]}" -I"$T/lean" -c "$T/provider.cpp" -o "$T/provider.o"
"$CXX" "${FLAGS[@]}" -I"$T/lean" -c "$T/consumer.cpp" -o "$T/consumer_lean.o"
"$CXX" "${FLAGS[@]}" -I"$T/other" -c "$T/consumer.cpp" -o "$T/consumer_other.o"

# Same profile: must link AND decode.
if "$CXX" "$T/provider.o" "$T/consumer_lean.o" -o "$T/same" 2>"$T/same.err" && "$T/same"; then
  echo "ok   [same-profile links and decodes]"
else
  echo "FAIL [same-profile]: expected link + decode to succeed"
  head -3 "$T/same.err"
  fail=1
fi

# Mixed profiles: the link MUST fail, and on the exchanged symbol (the rp_modes id in the
# mangled name is the expected cause).
if "$CXX" "$T/provider.o" "$T/consumer_other.o" -o "$T/mixed" 2>"$T/mixed.err"; then
  echo "FAIL [mixed-profile]: expected a link error, but it linked"
  fail=1
elif ! grep -q 'holder_must' "$T/mixed.err"; then
  echo "FAIL [mixed-profile]: link failed but not on the exchanged symbol"
  head -3 "$T/mixed.err"
  fail=1
else
  echo "ok   [mixed-profile link fails on the exchanged symbol]"
fi

if [[ "$fail" == "0" ]]; then
  echo "arena modes link: profile identity enforced at link time"
fi
exit "$fail"
