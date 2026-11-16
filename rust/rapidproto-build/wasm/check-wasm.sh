#!/usr/bin/env bash
# Staleness gate for the generator rapidproto-build ships: rebuild the wasm blob and the C++ copy
# (build-wasm.sh) into a scratch directory and diff them against the committed ones. A difference
# means the C++ generator changed without them being regenerated, so the crate would ship stale
# codegen. The fix is always to run build-wasm.sh and commit the result.
#
# The blob build is byte-reproducible, so the diff is exact. Requires wasi-sdk (set WASI_SDK, or
# install at /opt/wasi-sdk).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(dirname "$here")"

fresh="$(mktemp -d)"
trap 'rm -rf "$fresh"' EXIT
"$here/build-wasm.sh" "$fresh" >/dev/null

stale=()
cmp -s "$crate/wasm/rustgen.wasm" "$fresh/wasm/rustgen.wasm" || stale+=(wasm/rustgen.wasm)
diff -rq "$crate/cpp" "$fresh/cpp" >&2 || stale+=(cpp/)
if [[ ${#stale[@]} -eq 0 ]]; then
    echo "wasm blob and C++ copy are up to date"
    exit 0
fi

echo "error: stale in rust/rapidproto-build: ${stale[*]}" >&2
echo "  the C++ generator changed without regenerating them. Regenerate and commit:" >&2
echo "    rust/rapidproto-build/wasm/build-wasm.sh" >&2
exit 1
