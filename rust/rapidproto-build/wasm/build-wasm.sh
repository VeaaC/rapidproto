#!/usr/bin/env bash
# Build the Rust generator the way rapidproto-build ships it, from the C++ sources in this repository:
#   * wasm/rustgen.wasm, a standalone wasm32-wasi command module (the `wasm` feature), and
#   * cpp/, the translation units and project headers its build script compiles (the `native`
#     feature), so the crate builds natively wherever it comes from.
# Both are committed; check-wasm.sh rebuilds them in CI and diffs them against the committed ones.
#
# The blob is byte-reproducible -- same inputs, same bytes. Reproducibility rests on:
#   * -ffile-prefix-map, which rewrites the embedded repo and sysroot paths to fixed stand-ins, and
#   * -Wl,--strip-all, which drops the timestamps/paths a symbol table would carry.
# wasmi (the build crate's interpreter) has no wasm exception-handling, so the front-end is built
# -fno-exceptions (it already uses Result, never throws).
#
# Usage: build-wasm.sh [OUT_DIR]   (default: the crate directory)
# Env:   WASI_SDK   wasi-sdk install prefix (default: /opt/wasi-sdk)
set -euo pipefail

# The pinned wasi-sdk. The blob bytes are tied to this toolchain, so the build must use exactly it:
# a different wasi-sdk produces different bytes, which would otherwise surface downstream as a
# confusing "blob is stale" (it is not -- the generator source is fine, the toolchain differs).
# Bumping this (or project(VERSION), which the blob embeds) changes the blob, so regenerate + commit.
EXPECTED_WASI_SDK="34.0"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
wasi="${WASI_SDK:-/opt/wasi-sdk}"
out_dir="${1:-$(dirname "$here")}"
cxx="$wasi/bin/clang++"

if [[ ! -x "$cxx" ]]; then
    echo "error: wasi-sdk clang++ not found at $cxx (set WASI_SDK to your wasi-sdk prefix)" >&2
    exit 1
fi

# Version file is one line, e.g. "34.0" (first field only; later lines carry component hashes).
have_wasi="$(sed -n '1p' "$wasi/VERSION" 2>/dev/null | cut -d' ' -f1)"
if [[ "$have_wasi" != "$EXPECTED_WASI_SDK" ]]; then
    echo "error: wasi-sdk at $wasi is version '${have_wasi:-unknown}', but the blob is pinned to" \
         "$EXPECTED_WASI_SDK." >&2
    echo "  install wasi-sdk $EXPECTED_WASI_SDK (or point WASI_SDK at it). A mismatched toolchain" \
         "would produce a blob that fails the staleness gate for the wrong reason." >&2
    exit 1
fi

# The generator's translation units. The blob adds the wasm entry point, the native build the C ABI
# one.
generator=(
    src/lexer.cpp
    src/interpret.cpp
    src/parser.cpp
    src/features.cpp
    src/resolve.cpp
    src/resolver.cpp
    src/source.cpp
    src/wellknown_generated.cpp
    src/codegen/naming.cpp
    src/arenagen/layout.cpp
    src/arenagen/modes.cpp
    src/rustgen/generator.cpp
    src/rustgen/frame.cpp
)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Synthesize the one cmake-generated header the closure needs (version.hpp) from the same two
# sources cmake uses -- the template and the VERSION in project() -- so there is a single source of
# truth and the build needs no cmake configure step.
version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")"
if [[ -z "$version" ]]; then
    echo "error: could not read project(VERSION ...) from CMakeLists.txt" >&2
    exit 1
fi
mkdir -p "$work/gen-include/rapidproto"
sed "s/@PROJECT_VERSION@/$version/" "$repo/cmake/version.hpp.in" \
    > "$work/gen-include/rapidproto/version.hpp"

flags=(
    -std=c++17 -O2 -DNDEBUG -fno-exceptions -fno-rtti
    -I "$repo/include" -I "$work/gen-include"
    -ffile-prefix-map="$repo=." -ffile-prefix-map="$wasi=/wasi"
    -ffile-prefix-map="$work/gen-include=." -ffile-prefix-map="$work=/build"
)

objs=()
for tu in "${generator[@]}" src/rustgen/wasm_entry.cpp; do
    obj="$work/$(echo "$tu" | tr '/' '_').o"
    "$cxx" "${flags[@]}" -c "$repo/$tu" -o "$obj"
    objs+=("$obj")
done
# Default wasm stack is 64 KiB -- too small for the parser/resolver recursion on a deep schema. Give
# it 8 MiB (matches a typical native thread stack); the depth caps (parser, resolver) stay the real
# safety floor, this just keeps a legal-but-deep schema from tripping the guard page first.
blob="$out_dir/wasm/rustgen.wasm"
mkdir -p "$(dirname "$blob")"
"$cxx" "${flags[@]}" -Wl,--strip-all -Wl,-z,stack-size=8388608 "${objs[@]}" -o "$blob"
echo "built $blob ($(stat -c%s "$blob") bytes, version $version)"

# The native build's sources, at their repository paths: its translation units and every project
# header they include, as the preprocessor finds them (-MM leaves out system headers).
native=("${generator[@]/#/$repo/}" "$repo/src/rustgen/native_entry.cpp")
"$cxx" "${flags[@]}" -MM "${native[@]}" > "$work/deps"
cpp="$out_dir/cpp"
rm -rf "$cpp"
count=0
while read -r dep; do
    case "$dep" in
        "$repo"/*) rel="${dep#"$repo"/}" ;;
        "$work/gen-include"/*) rel="include/${dep#"$work/gen-include"/}" ;;
        *) echo "error: unexpected dependency $dep" >&2; exit 1 ;;
    esac
    mkdir -p "$(dirname "$cpp/$rel")"
    cp "$dep" "$cpp/$rel"
    count=$((count + 1))
done < <(tr -s ' \\' '\n\n' < "$work/deps" | grep -v -e ':$' -e '^$' | sort -u)
echo "copied $count files to $cpp"
