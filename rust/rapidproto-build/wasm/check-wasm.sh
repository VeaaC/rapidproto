#!/usr/bin/env bash
# Staleness gate for the committed rustgen wasm blob: rebuild it from source and byte-diff against
# the committed rustgen.wasm. A difference means the C++ generator changed but the blob was not
# regenerated -- so the `rapidproto-build` crate would ship stale Rust codegen. The fix is always to
# run build-wasm.sh and commit the result.
#
# The build is byte-reproducible (see build-wasm.sh), so this diff is exact, not fuzzy. Requires
# wasi-sdk (set WASI_SDK, or install at /opt/wasi-sdk).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
committed="$here/rustgen.wasm"
if [[ ! -f "$committed" ]]; then
    echo "error: committed blob $committed is missing" >&2
    exit 1
fi

fresh="$(mktemp --suffix=.wasm)"
trap 'rm -f "$fresh"' EXIT
"$here/build-wasm.sh" "$fresh" >/dev/null

if cmp -s "$committed" "$fresh"; then
    echo "wasm blob is up to date ($(stat -c%s "$committed") bytes)"
    exit 0
fi

echo "error: the committed rustgen.wasm is stale (differs from a fresh build)." >&2
echo "  the C++ generator changed without regenerating the blob. Regenerate and commit it:" >&2
echo "    rust/rapidproto-build/wasm/build-wasm.sh" >&2
exit 1
