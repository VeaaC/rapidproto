#!/usr/bin/env bash
#
# Every tests/test_*.cpp must CONTRIBUTE at least one test case to the built suite binary.
#
# The fixtures stage proves each file is LISTED and compiled; nothing proved it contributes:
# `: > tests/test_decode.cpp` stayed listed, compiled, formatted and linted while its cases and
# assertions left the suite -- ALL GREEN, exit 0. The same shape covers a deleted TEST_CASE and an
# `#if 0` around one. A per-FILE floor rather than a global count, so it never needs updating when
# tests are added.
#
#   tests/check_test_contribution.sh <rapidproto_tests>
#
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
bin="${1:?usage: check_test_contribution.sh <rapidproto_tests>}"

# The spec is a FILTER, and any filter includes hidden tests -- Catch2 excludes hidden ones only
# when NO filter parses. `*` alone would therefore suffice; the `[.]` spells the intent out. The
# hazard is the degraded path: an unparseable spec does not error, it silently falls back to the
# hidden-EXCLUDING default -- under which the all-hidden test_integration.cpp ([.corpus]) reads as
# contributing nothing, the exact false positive this check exists to avoid. Hence the positive
# assertion below rather than trust in the flag.
if ! listing=$("$bin" --list-tests -r xml '*,[.]' 2>&1); then
  echo ">> $bin --list-tests failed -- the contribution check checked nothing:"
  tail -3 <<<"$listing"
  exit 1
fi
mapfile -t contributing < <(sed -n 's|.*<File>\(.*\)</File>.*|\1|p' <<<"$listing" | sort -u)
# Anti-vacuity, both sides: an empty or reshaped listing (Catch2 changing its XML element names)
# and an empty source glob must both read as "the scan broke", never as findings. FIRST, before
# the hidden-tag assertion below -- an empty listing satisfies neither and must be blamed on the
# listing, not on the spec.
if [[ ${#contributing[@]} -lt 10 ]]; then
  echo ">> the listing names only ${#contributing[@]} source files -- the XML shape changed or the"
  echo "   binary listed nothing; either way this check compared nothing"
  exit 1
fi
# Anchored to the Tags element: a test NAMED with the literal text `[.]` must not satisfy the
# assertion the way an actual hidden case does.
if ! grep -q '<Tags>[^<]*\[\.\]' <<<"$listing"; then
  echo ">> the listing contains no hidden-tagged case: the '*,[.]' spec degraded to the"
  echo "   hidden-excluding default, so the all-hidden test source would be misreported."
  echo "   (If the suite deliberately lost its last hidden case, update this assertion.)"
  exit 1
fi

missing=()
total=0
for src in tests/test_*.cpp; do
  total=$((total + 1))
  base=$(basename "$src")
  hit=0
  for f in "${contributing[@]}"; do
    if [[ "$f" == */tests/"$base" ]]; then
      hit=1
      break
    fi
  done
  [[ $hit -eq 1 ]] || missing+=("$src")
done
if [[ $total -lt 10 ]]; then
  echo ">> too few test sources matched tests/test_*.cpp ($total) -- this check is looking in"
  echo "   the wrong place"
  exit 1
fi
# EVERY source coming back caseless is a broken comparison (the listing's path spelling no longer
# ends in tests/<basename>), not a simultaneous loss of the whole suite -- diagnose the scan.
if [[ ${#missing[@]} -eq $total ]]; then
  echo ">> no test source matched any listed path -- the listing's path shape and this check's"
  echo "   suffix match disagree; nothing was actually compared"
  exit 1
fi
# The -gt guard, not a bare expansion: "${missing[@]}" on an empty array errors under set -u on
# bash 4.0-4.3 (mapfile above already rules out anything older).
[[ ${#missing[@]} -gt 0 ]] && for src in "${missing[@]}"; do
  echo ">> $src contributes NO test case to the suite -- emptied, #if 0'd, its cases deleted,"
  echo "   or never added to the binary (the fixtures stage names that last cause separately)"
done
[[ ${#missing[@]} -eq 0 ]] || exit 1
echo "test contribution: all $total test sources contribute cases"
