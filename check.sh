#!/usr/bin/env bash
#
# One-stop quality gate for rapidproto: clang-format, build + test on both
# compilers (gcc-13, clang-20), and clang-tidy (strict on the library, relaxed on
# tests). Operates only on our own sources -- never the vendored Catch2 amalgam or
# the thin CLI driver src/main.cpp.
#
#   ./check.sh        # full gate: format check, build+test both compilers, compile-fail, clang-tidy
#   ./check.sh fix    # first apply clang-format, then run the full gate
#   ./check.sh quick  # fast inner loop: apply formatting + gcc build+test only (no clang/tidy)
#   ./check.sh deep   # OPT-IN heavy tier (CI / end-of-phase, NOT the inner loop): ASan+UBSan over the
#                     # full suite, coverage with a line floor, and a fuzz smoke over the three decode
#                     # paths. Slow (three instrumented builds). Override: FUZZ_TIME=120 COV_FLOOR=88.
#
# The independent stages (format, gcc build+test, clang build+test, compile-fail,
# clang-tidy) run concurrently; each build is a parallel build and clang-tidy is
# parallelized across files. Per-stage output is captured and printed in a fixed
# order so nothing interleaves. Exits non-zero if anything is not clean.

set -uo pipefail
cd "$(dirname "$0")"

CLANG_FORMAT=clang-format-20
CLANG_TIDY=clang-tidy-20
JOBS="$(nproc 2>/dev/null || echo 4)"
export CLANG_TIDY

# Our own hand-written sources. EVERYTHING we author is clang-formatted -- formatting is mechanical, so
# nothing is exempt; only VENDORED code (tests/catch_amalgamated.*) and GENERATED code
# (src/wellknown_generated.cpp, the *_golden/ headers) are, since formatting them would fight their
# vendor/generator. clang-tidy runs on the narrower LIB_SRC + TEST_SRC: the thin CLI drivers
# (src/*/main.cpp), the benches (built -O3, non-strict), the fuzz harnesses, and the example consumers
# are formatted but NOT tidied -- their argv / measurement / harness patterns trip strict checks for
# no real-bug gain.
HEADERS=(include/rapidproto/*.hpp include/rapidproto/streamgen/*.hpp include/rapidproto/arenagen/*.hpp include/rapidproto/codegen/*.hpp include/rapidproto/cli/*.hpp)
LIB_SRC=(src/lexer.cpp src/interpret.cpp src/parser.cpp src/features.cpp src/resolve.cpp src/resolver.cpp src/source.cpp src/streamgen/generator.cpp src/codegen/naming.cpp src/arenagen/layout.cpp src/arenagen/generator.cpp src/header_self_contained.cpp)
TEST_SRC=(tests/test_*.cpp)
CLI_SRC=(src/main.cpp src/rapidprotoc/main.cpp)
EXTRA_SRC=(tests/bench_streamgen.cpp tests/bench_arena.cpp tests/fuzz/*.cpp examples/*/*.cpp)
# Test-helper headers (the dump / temp_dir utilities our tests #include) -- every tests/*.hpp EXCEPT
# the vendored Catch2 amalgam, so a newly-added helper can't silently escape the format gate.
TEST_HDR=()
for _h in tests/*.hpp; do [[ "$_h" == tests/catch_amalgamated.hpp ]] || TEST_HDR+=("$_h"); done
FORMAT_FILES=("${HEADERS[@]}" "${LIB_SRC[@]}" "${TEST_SRC[@]}" "${CLI_SRC[@]}" "${EXTRA_SRC[@]}" "${TEST_HDR[@]}")

section() { printf '\n=== %s ===\n' "$1"; }

if [[ "${1:-}" == "fix" ]]; then
  section "clang-format (apply)"
  "$CLANG_FORMAT" -i "${FORMAT_FILES[@]}"
  echo "formatted ${#FORMAT_FILES[@]} files"
fi

# Fast inner loop: apply formatting, then gcc build + test only (skips clang, clang-tidy,
# compile-fail, and the stress compile). Run the full gate (`./check.sh`) before committing.
if [[ "${1:-}" == "quick" ]]; then
  section "clang-format (apply)"
  "$CLANG_FORMAT" -i "${FORMAT_FILES[@]}"
  echo "formatted ${#FORMAT_FILES[@]} files"
  section "build + test (gcc)"
  if ! cmake --preset gcc >/dev/null 2>&1; then
    echo ">> cmake configure failed"
    exit 1
  fi
  build_out=$(cmake --build --preset gcc -j"$JOBS" 2>&1)
  if [[ $? -ne 0 ]] || grep -qE 'error:|warning:' <<<"$build_out"; then
    echo ">> build problems (gcc):"
    grep -E 'error:|warning:' <<<"$build_out" | head -30
    exit 1
  fi
  echo "build clean (gcc)"
  test_out=$(./build/gcc/rapidproto_tests 2>&1)
  if grep -qE 'All tests passed' <<<"$test_out"; then
    grep -oE 'All tests passed.*' <<<"$test_out"
    echo ">> quick OK (gcc only -- run ./check.sh for the full gate before committing)"
    exit 0
  fi
  echo ">> tests failed (gcc):"
  grep -E 'FAILED|with expansion|assertions:' <<<"$test_out" | head -30
  exit 1
fi

# Opt-in heavy tier: the dynamic-analysis tooling (sanitizers, coverage, fuzzing). Deliberately NOT
# part of the default gate -- three instrumented builds + a fuzz run are too slow for every small
# change. Run it in CI or at the end of a phase. Requires clang-20 + llvm-{cov,profdata}-20.
if [[ "${1:-}" == "deep" ]]; then
  CXX=clang++-20; CC=clang-20
  FUZZ_TIME=${FUZZ_TIME:-30}   # seconds per fuzz target
  COV_FLOOR=${COV_FLOOR:-85}   # minimum library line-coverage %
  deep_fail=0

  section "ASan + UBSan (full suite)"
  if cmake -S . -B build/san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$CC" \
       -DCMAKE_CXX_COMPILER="$CXX" -DRAPIDPROTO_SANITIZE=ON >/dev/null 2>&1 \
     && cmake --build build/san --target rapidproto_tests -j"$JOBS" >/dev/null 2>&1; then
    if UBSAN_OPTIONS=print_stacktrace=1 ASAN_OPTIONS=detect_leaks=1 \
         ./build/san/rapidproto_tests 2>&1 | grep -qE 'All tests passed'; then
      echo "sanitizers clean"
    else
      echo ">> ASan/UBSan finding or test failure (re-run ./build/san/rapidproto_tests)"; deep_fail=1
    fi
  else
    echo ">> sanitizer build failed"; deep_fail=1
  fi

  section "coverage (library line floor ${COV_FLOOR}%)"
  if cmake -S . -B build/cov -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$CC" \
       -DCMAKE_CXX_COMPILER="$CXX" -DRAPIDPROTO_COVERAGE=ON >/dev/null 2>&1 \
     && cmake --build build/cov --target rapidproto_tests -j"$JOBS" >/dev/null 2>&1; then
    LLVM_PROFILE_FILE=build/cov/cov.profraw ./build/cov/rapidproto_tests >/dev/null 2>&1
    llvm-profdata-20 merge -sparse build/cov/cov.profraw -o build/cov/cov.profdata 2>/dev/null
    cov=$(llvm-cov-20 report ./build/cov/rapidproto_tests -instr-profile=build/cov/cov.profdata \
            -ignore-filename-regex='(tests/|build/|wellknown_generated|catch_amalgamated)' 2>/dev/null \
            | awk '/^TOTAL/{print $10}')
    echo "library line coverage: ${cov:-unknown}"
    if awk -v c="${cov%\%}" -v f="$COV_FLOOR" 'BEGIN{exit !(c+0 >= f+0)}'; then
      echo "at or above floor (${COV_FLOOR}%)"
    else
      echo ">> coverage ${cov} below floor ${COV_FLOOR}%"; deep_fail=1
    fi
  else
    echo ">> coverage build failed"; deep_fail=1
  fi

  section "fuzz smoke (${FUZZ_TIME}s per target)"
  mkdir -p build/fuzz
  for f in wire arena stream; do
    if "$CXX" -std=c++17 -O1 -g -Iinclude -Itests -fsanitize=fuzzer,address,undefined \
         "tests/fuzz/fuzz_$f.cpp" -o "build/fuzz/fuzz_$f" 2>/dev/null; then
      if "./build/fuzz/fuzz_$f" -max_total_time="$FUZZ_TIME" -timeout=10 \
           >"build/fuzz/log_$f" 2>&1; then
        echo "fuzz_$f: clean ($(grep -oE 'cov: [0-9]+ ' "build/fuzz/log_$f" | tail -1))"
      else
        echo ">> fuzz_$f CRASH (input saved by libFuzzer):"
        grep -iE 'ERROR|runtime error|SUMMARY' "build/fuzz/log_$f" | head -5; deep_fail=1
      fi
    else
      echo ">> fuzz_$f build failed"; deep_fail=1
    fi
  done

  section "deep summary"
  if [[ "$deep_fail" == 0 ]]; then echo "DEEP ALL GREEN"; else echo "DEEP FAILURES above"; fi
  exit "$deep_fail"
fi

LOG="$(mktemp -d)"
trap 'rm -rf "$LOG"' EXIT

# Configure both presets up front (each build dir once) so the concurrent build and clang-tidy jobs
# never race on the same build directory. Configuration is cheap.
cmake --preset gcc   >"$LOG/cfg-gcc"   2>&1 & cfg_gcc=$!
cmake --preset clang >"$LOG/cfg-clang" 2>&1 & cfg_clang=$!
wait "$cfg_gcc"; rc_cfg_gcc=$?
wait "$cfg_clang"; rc_cfg_clang=$?
if [[ $rc_cfg_gcc -ne 0 || $rc_cfg_clang -ne 0 ]]; then
  section "cmake configure"
  cat "$LOG/cfg-gcc" "$LOG/cfg-clang"
  echo ">> cmake configure failed"
  exit 1
fi

# --- stages: each writes its full output to a log and exits 0 (clean) / 1 (problem) -------------

job_format() {
  if "$CLANG_FORMAT" --dry-run --Werror "${FORMAT_FILES[@]}"; then
    echo "format clean"
  else
    echo ">> formatting violations (run: ./check.sh fix)"
    return 1
  fi
}

job_build_test() {  # $1 = preset; parallel build, then run the test binary
  local preset=$1 build_out test_out rc
  build_out=$(cmake --build --preset "$preset" -j"$JOBS" 2>&1); rc=$?
  if [[ $rc -ne 0 ]] || grep -qE 'error:|warning:' <<<"$build_out"; then
    echo ">> build problems ($preset):"
    grep -E 'error:|warning:' <<<"$build_out" | head -30
    return 1
  fi
  echo "build clean ($preset)"
  test_out=$(./build/"$preset"/rapidproto_tests 2>&1)
  if grep -qE 'All tests passed' <<<"$test_out"; then
    grep -oE 'All tests passed.*' <<<"$test_out"
  else
    echo ">> tests failed ($preset):"
    grep -E 'FAILED|with expansion|assertions:' <<<"$test_out" | head -30
    return 1
  fi
  # The consumer example (examples/consumer) is built alongside via rapidproto_generate(); run it to
  # confirm the helper-generated decoders (arena + streaming, in one TU) decode at runtime here.
  if [[ -x "./build/$preset/examples/consumer/rapidproto_example_consumer" ]]; then
    if "./build/$preset/examples/consumer/rapidproto_example_consumer" >/dev/null 2>&1; then
      echo "consumer example: decoded OK ($preset)"
    else
      echo ">> consumer example failed ($preset)"
      return 1
    fi
  fi
  # The gcc build also produced rapidprotoc; compile-check the dispatch-gate worst case
  # (a many-field x many-callback decoder builds). Timing stays manual (streamgen_compile_bench.sh).
  if [[ "$preset" == gcc ]]; then
    local stress_out
    if ! stress_out=$(tests/streamgen_compile_bench.sh --check clang++-20 2>&1); then
      echo "$stress_out"
      return 1
    fi
    tail -1 <<<"$stress_out"
  fi
}

job_compile_fail() {
  local cf_cxx=clang++-20 out rc=0
  command -v "$cf_cxx" >/dev/null 2>&1 || cf_cxx=c++
  if out=$(tests/streamgen_compile_fail.sh "$cf_cxx" 2>&1); then tail -1 <<<"$out"; else echo "$out"; rc=1; fi
  if out=$(tests/arenagen_compile_fail.sh "$cf_cxx" 2>&1); then tail -1 <<<"$out"; else echo "$out"; rc=1; fi
  return "$rc"
}

# Run clang-tidy on one file; on diagnostics, write them to a per-file log under $TIDY_D.
tidy_one() {
  local f=$1 out
  out=$("$CLANG_TIDY" -p build/clang --quiet "$f" 2>/dev/null | grep -E 'warning:|error:')
  if [[ -n "$out" ]]; then
    { printf '>> %s\n' "$f"; head -20 <<<"$out"; } >"$TIDY_D/$(tr / _ <<<"$f")"
  fi
}
export -f tidy_one

job_tidy() {
  if [[ ! -f build/clang/compile_commands.json ]]; then
    echo ">> build/clang/compile_commands.json missing"
    return 1
  fi
  TIDY_D="$LOG/tidy.d"; mkdir -p "$TIDY_D"; export TIDY_D
  # Lint every TU in parallel (each writes its own log), then aggregate.
  printf '%s\n' "${LIB_SRC[@]}" "${TEST_SRC[@]}" \
    | xargs -P"$JOBS" -I{} bash -c 'tidy_one "$@"' _ {}
  if compgen -G "$TIDY_D/*" >/dev/null; then
    cat "$TIDY_D"/*
    echo ">> clang-tidy diagnostics above"
    return 1
  fi
  echo "tidy clean"
}

# --- run all stages concurrently, capturing each to its own log -----------------------------------

job_format       >"$LOG/format" 2>&1 & p_format=$!
job_build_test gcc   >"$LOG/gcc"   2>&1 & p_gcc=$!
job_build_test clang >"$LOG/clang" 2>&1 & p_clang=$!
job_compile_fail >"$LOG/cf"     2>&1 & p_cf=$!
job_tidy         >"$LOG/tidy"   2>&1 & p_tidy=$!

wait "$p_format"; rc_format=$?
wait "$p_gcc";    rc_gcc=$?
wait "$p_clang";  rc_clang=$?
wait "$p_cf";     rc_cf=$?
wait "$p_tidy";   rc_tidy=$?

# --- print each stage's output in a fixed order (already captured, so never interleaved) ----------

section "clang-format (check)";                       cat "$LOG/format"
section "build + test (gcc)";                         cat "$LOG/gcc"
section "build + test (clang)";                       cat "$LOG/clang"
section "compile-fail (generated decoder rejects misuse)"; cat "$LOG/cf"
section "clang-tidy (library = strict, tests = relaxed)";  cat "$LOG/tidy"

fail=0
for rc in "$rc_format" "$rc_gcc" "$rc_clang" "$rc_cf" "$rc_tidy"; do
  [[ "$rc" -ne 0 ]] && fail=1
done

section "summary"
if [[ "$fail" == "0" ]]; then
  echo "ALL GREEN"
else
  echo "FAILURES above"
fi
exit "$fail"
