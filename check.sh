#!/usr/bin/env bash
#
# One-stop quality gate for rapidproto: clang-format, build + test on both
# compilers (gcc-13, clang-20), and clang-tidy (strict on the library, relaxed on
# tests). Operates only on our own sources -- never the vendored Catch2 amalgam or
# the thin CLI driver src/main.cpp.
#
#   ./check.sh        # full gate: format check, doc links, build+test both compilers, compile-fail,
#                     # clang-tidy, and the real-world schema corpus sweep
#   ./check.sh fix    # first apply clang-format, then run the full gate
#   ./check.sh quick  # fast inner loop: apply formatting + gcc build+test only (no clang/tidy)
#   ./check.sh deep   # OPT-IN heavy tier (CI / end-of-phase, NOT the inner loop): ASan+UBSan over the
#                     # full suite, coverage with a line floor, and a fuzz smoke over the four targets
#                     # (three decode paths + the schema front-end). Slow (three instrumented builds).
#                     # Override: FUZZ_TIME=120 COV_FLOOR=88.
#
# Every stage's output is captured to build/gate-logs/<stage> and kept after the run, so a failure
# you piped past can be read back instead of re-running the gate. The summary names the stages that
# failed, how long each took, and how many ran -- `./check.sh | tail -20` is enough to triage.
#
# RAPIDPROTO_GATE_STAGES='gcc tidy' runs a subset (space-separated; an unknown key is an error).
# RAPIDPROTO_GATE_SERIAL=1 runs stages one at a time (the default under GITHUB_ACTIONS).
#
# The independent stages (format, doc-links, fixture-coverage, gcc build+test, clang build+test,
# compile-fail, fuzz-compile, clang-tidy) run concurrently; each build is a parallel build and clang-tidy is
# parallelized across files. The corpus stage is the exception: it consumes the gcc stage's
# rapidprotoc, so it runs after them. Per-stage output is captured and printed in a fixed
# order so nothing interleaves. Exits non-zero if anything is not clean.

set -uo pipefail
cd "$(dirname "$0")"

CLANG_FORMAT=clang-format-20
CLANG_TIDY=clang-tidy-20
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export CLANG_TIDY

# Our own hand-written sources. EVERYTHING we author is clang-formatted -- formatting is mechanical, so
# nothing is exempt; only VENDORED code (tests/catch_amalgamated.*) and GENERATED code
# (src/wellknown_generated.cpp, the *_golden/ headers) are, since formatting them would fight their
# vendor/generator. clang-tidy runs on the narrower LIB_SRC + TEST_SRC: the thin CLI drivers
# (src/*/main.cpp and the differential's harness generator), the benches (built -O3, non-strict),
# the fuzz harnesses, and the example consumers
# are formatted but NOT tidied -- their argv / measurement / harness patterns trip strict checks for
# no real-bug gain.
HEADERS=(include/rapidproto/*.hpp include/rapidproto/streamgen/*.hpp include/rapidproto/arenagen/*.hpp include/rapidproto/dumpgen/*.hpp include/rapidproto/codegen/*.hpp include/rapidproto/cli/*.hpp)
LIB_SRC=(src/lexer.cpp src/interpret.cpp src/parser.cpp src/parser_enum.cpp src/features.cpp src/resolve.cpp src/resolver.cpp src/source.cpp src/streamgen/generator.cpp src/codegen/naming.cpp src/arenagen/layout.cpp src/arenagen/modes.cpp src/arenagen/generator.cpp src/dumpgen/generator.cpp src/header_self_contained.cpp)
TEST_SRC=(tests/test_*.cpp)
CLI_SRC=(src/main.cpp src/rapidprotoc/main.cpp tests/diffgen/main.cpp)
EXTRA_SRC=(tests/bench_streamgen.cpp tests/bench_stream_isolated.cpp tests/bench_arena.cpp tests/fuzz/*.cpp examples/*/*.cpp)
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

  # Seeds for one target, staged fresh under build/fuzz/seeds/<target>. Staged rather than pointing
  # libFuzzer at the fixture directories themselves, which hold more than seeds: tests/wire_fixtures
  # also carries .txtpb / .proto / .py that would enter the corpus as junk units, and tests/corpus
  # nests subdirectories whose same-named files need flattening to coexist in one directory.
  # Rebuilding each run also means a fixture that moved cannot leave a stale seed behind.
  stage_fuzz_seeds() {
    # `seed` is local: the caller loops over `f`, and an undeclared loop variable here would clobber
    # it -- leaving the caller running ./build/fuzz/fuzz_ with an empty target name.
    local target=$1 dir="build/fuzz/seeds/$1" seed dst
    rm -rf "$dir"; mkdir -p "$dir"
    if [[ "$target" == parser ]]; then
      # Every corpus schema, path-flattened. Flattening is not injective (a future
      # tests/corpus/arena/modes.proto would land on arena_modes.proto), so say so rather than
      # silently staging one file fewer than intended.
      while IFS= read -r seed; do
        dst="$dir/${seed//\//_}"
        [[ -e "$dst" ]] && echo ">> seed name collision: $seed flattens onto an already-staged name"
        cp "$seed" "$dst"
      done < <(find tests/corpus -name '*.proto')
      # The parser's recursion DepthGuard is its one explicit anti-crash mechanism, and no corpus
      # schema nests more than a few levels -- so mutation never assembles a run deep enough to
      # reach it, and deleting the guard goes unnoticed for as long as anyone cares to fuzz. These
      # two start just under the cap, at the two recursion points, for the mutator to extend.
      printf 'option x = %s' "$(printf '[%.0s' $(seq 1 2000))" >"$dir/zz_deep_list.proto"
      printf '%s' "$(printf 'message M{%.0s' $(seq 1 300))" >"$dir/zz_deep_message.proto"
    else
      cp tests/wire_fixtures/*.bin "$dir/" 2>/dev/null || true
      # Payloads the differential kept, if it was run with --write-seeds: thousands of valid
      # messages, reaching decoder arms a handful of fixtures never will.
      if [[ -d build/fuzz/payload-seeds ]]; then
        cp build/fuzz/payload-seeds/* "$dir/" 2>/dev/null || true
      fi
    fi
    # Staging nothing means the target runs cold while the tier still reports green, so say so. A
    # moved fixture directory is the way this happens, and it is silent otherwise.
    if [[ -z "$(ls -A "$dir" 2>/dev/null)" ]]; then
      echo ">> no seeds staged for fuzz_$target (fixtures moved?) -- it will run from an empty corpus"
    fi
    return 0
  }

  section "fuzz smoke (${FUZZ_TIME}s per target)"
  mkdir -p build/fuzz
  # The front-end target links the library TUs; the three decode targets are header-only.
  parser_tus=(src/lexer.cpp src/parser.cpp src/parser_enum.cpp src/features.cpp src/resolve.cpp src/interpret.cpp
              src/source.cpp src/resolver.cpp src/wellknown_generated.cpp)
  for f in wire arena stream parser; do
    extra_tus=()
    [[ "$f" == parser ]] && extra_tus=("${parser_tus[@]}")
    if "$CXX" -std=c++17 -O1 -g -Iinclude -Itests -fsanitize=fuzzer,address,undefined \
         "tests/fuzz/fuzz_$f.cpp" "${extra_tus[@]}" -o "build/fuzz/fuzz_$f" 2>/dev/null; then
      # The corpus directory persists across runs (build/ is gitignored), so each run starts from
      # what earlier runs discovered instead of rediscovering the same coverage from scratch.
      mkdir -p "build/fuzz/corpus/$f"
      stage_fuzz_seeds "$f"
      # DEBUGINFOD_URLS= : libFuzzer symbolizes its NEW_FUNC lines, and where the system points
      # llvm-symbolizer at a debuginfod server (Ubuntu ships /etc/debuginfod/*.urls by default) that
      # lookup blocks ~90s the first time a new function is discovered -- consuming the whole budget.
      # Measured on this box: 27 executions in 90s with it set, 35087 in 31s without. Emptying it
      # only stops the network fetch; local -g debug info still symbolizes.
      if DEBUGINFOD_URLS= "./build/fuzz/fuzz_$f" "build/fuzz/corpus/$f" "build/fuzz/seeds/$f" \
           -max_total_time="$FUZZ_TIME" -timeout=10 -artifact_prefix=build/fuzz/ \
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

  # Moved out of the default gate (see DEFAULT_STAGES). Needs a rapidprotoc: the sanitizer build is
  # the wrong binary to sweep 8000 schemas with, so build the plain one if this is a bare deep run.
  section "real-world schema corpus"
  if [[ ! -x ./build/gcc/rapidprotoc ]]; then
    cmake --preset gcc >/dev/null 2>&1
    cmake --build --preset gcc --target rapidprotoc -j"$JOBS" >/dev/null 2>&1
  fi
  if [[ -x ./build/gcc/rapidprotoc ]]; then
    python3 tests/corpus_gate.py --rapidprotoc ./build/gcc/rapidprotoc --jobs "$JOBS" || deep_fail=1
  else
    echo ">> could not build build/gcc/rapidprotoc for the corpus sweep"; deep_fail=1
  fi

  section "deep summary"
  if [[ "$deep_fail" == 0 ]]; then echo "DEEP ALL GREEN"; else echo "DEEP FAILURES above"; fi
  exit "$deep_fail"
fi

# Kept, not deleted: after a long run the useful next step is almost always "read the stage that
# failed", and a deleted log means re-running the whole gate to see it again. One fixed path so the
# summary can name it (and so a second run does not accumulate directories).
LOG="build/gate-logs"
rm -rf "$LOG"; mkdir -p "$LOG"
# A CI kill (OOM, preemption, cancellation) arrives as SIGTERM and would discard every buffered
# stage log -- exactly when the logs matter most. Dump whatever was captured before dying.
trap 'echo ">> check.sh: killed (SIGTERM/SIGINT) -- dumping captured stage logs"; for f in "$LOG"/*; do [[ -f "$f" ]] && { echo "--- ${f##*/} ---"; cat "$f"; }; done; exit 143' TERM INT

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

# The markdown docs are split across README / docs/ / architecture.md with heavy cross-linking, so a
# renamed heading or a moved page rots silently; validate every relative link (file + anchor).
job_doc_links() {
  python3 tests/check_doc_links.py
}

job_fixtures() {
  tests/check_fixture_coverage.sh
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
  if [[ -x "./build/$preset/examples/consumer/rapidproto_example_lean" ]]; then
    if "./build/$preset/examples/consumer/rapidproto_example_lean" >/dev/null 2>&1; then
      echo "lean consumer example: decoded OK ($preset)"
    else
      echo ">> lean consumer example failed ($preset)"
      return 1
    fi
  fi
  # The gcc build also produced rapidprotoc; compile-check the dispatch-gate worst case
  # (a many-field x many-callback decoder builds). Timing stays manual (streamgen_compile_bench.sh).
  if [[ "$preset" == gcc ]]; then
    local stress_out link_out
    if ! stress_out=$(tests/streamgen_compile_bench.sh --check clang++-20 2>&1); then
      echo "$stress_out"
      return 1
    fi
    tail -1 <<<"$stress_out"
    # Field-modes ODR guard: same-profile TUs link, mixed-profile TUs must FAIL to link.
    if ! link_out=$(tests/arena_modes_link.sh ./build/gcc/rapidprotoc clang++-20 2>&1); then
      echo "$link_out"
      return 1
    fi
    tail -1 <<<"$link_out"
  fi
}

job_compile_fail() {
  local cf_cxx=clang++-20 out rc=0
  command -v "$cf_cxx" >/dev/null 2>&1 || cf_cxx=c++
  if out=$(tests/streamgen_compile_fail.sh "$cf_cxx" 2>&1); then tail -1 <<<"$out"; else echo "$out"; rc=1; fi
  if out=$(tests/arenagen_compile_fail.sh "$cf_cxx" 2>&1); then tail -1 <<<"$out"; else echo "$out"; rc=1; fi
  return "$rc"
}

# Compile-check the fuzz harnesses (tests/fuzz/*.cpp). The cmake build never compiles them -- they are
# LINKED only in the deep tier (they need the libFuzzer driver) -- so an API break in a harness would
# slip past the default gate and surface only in CI's fuzz job. A syntax-only compile here (no fuzzer,
# no sanitizer, no link) is enough to catch it. The benches and example consumers ARE built by the
# cmake build above, so they need no separate check.
# A generated header is compiled in the CONSUMER's translation unit, which is commonly newer than
# the C++17 this library targets -- and a field named `concept` or `requires` is merely a warning at
# 17 but a hard ERROR at 20. Nothing else in the gate compiles generated code at a newer standard,
# so without this the C++20 half of codegen's reserved-identifier set is unenforceable.
job_cxx20_smoke() {
  local cxx=g++-13 rc=0 out
  command -v "$cxx" >/dev/null 2>&1 || cxx=c++
  local tu="build/cxx20_smoke.cpp"
  mkdir -p build
  {
    echo '#include "arenagen_golden/arena_naming.rp.hpp"   // the C++ keyword fixture lives here'
    echo '#include "streamgen_golden/naming.rp.stream.hpp"'
    echo 'int main() {}'
  } >"$tu"
  for std in c++20 c++23; do
    if ! out=$("$cxx" -std=$std -Iinclude -Itests -Ibuild/gcc/generated/include -fsyntax-only "$tu" 2>&1); then
      echo ">> generated headers do not compile at -std=$std:"
      head -10 <<<"$out"
      rc=1
    fi
  done
  [[ $rc -eq 0 ]] && echo "generated headers compile at c++20 and c++23"
  return $rc
}

job_fuzz_compile() {
  local cxx=clang++-20 rc=0 out
  command -v "$cxx" >/dev/null 2>&1 || cxx=c++
  for f in tests/fuzz/*.cpp; do
    if ! out=$("$cxx" -std=c++17 -Iinclude -Itests -fsyntax-only "$f" 2>&1); then
      echo ">> $f does not compile:"
      head -15 <<<"$out"
      rc=1
    fi
  done
  [[ $rc -eq 0 ]] && echo "fuzz harnesses compile"
  return "$rc"
}

# Run clang-tidy on one file; on diagnostics, write them to a per-file log under $TIDY_D.
tidy_one() {
  local f=$1 out
  # --header-filter on the COMMAND LINE, not (only) the config file: clang-tidy < 20.1.8 ignores
  # the config's HeaderFilterRegex, silently skipping all header diagnostics -- the gate must
  # lint headers identically on every toolchain point release. Anchored to include/rapidproto
  # (see .clang-tidy for why a bare 'rapidproto/.*' is wrong).
  out=$("$CLANG_TIDY" -p build/clang --quiet --header-filter='include/rapidproto/.*' "$f" 2>/dev/null \
    | grep -E 'warning:|error:')
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
  # Lint every TU in parallel (each writes its own log), then aggregate. RAPIDPROTO_TIDY_SHARD=i/N
  # keeps every Nth TU (1-based): tidy dominates the gate's CPU, so CI fans it out across runner
  # jobs; unset (the local default) lints everything.
  local shard="${RAPIDPROTO_TIDY_SHARD:-1/1}" tu_index=0 tu
  local shard_index="${shard%/*}" shard_count="${shard#*/}"
  local tus=()
  for tu in "${LIB_SRC[@]}" "${TEST_SRC[@]}"; do
    tu_index=$((tu_index + 1))
    [[ $((tu_index % shard_count)) -eq $((shard_index % shard_count)) ]] && tus+=("$tu")
  done
  printf '%s\n' "${tus[@]}" | xargs -P"$JOBS" -I{} bash -c 'tidy_one "$@"' _ {}
  if compgen -G "$TIDY_D/*" >/dev/null; then
    cat "$TIDY_D"/*
    echo ">> clang-tidy diagnostics above"
    return 1
  fi
  echo "tidy clean (${#tus[@]} TUs, shard $shard)"
}

# The real-world compatibility check (see tests/corpus_gate.py for WHY it is load-bearing): every
# fetched schema through rapidprotoc, plus the [corpus] resolver cases. The `~[sweep]` filter drops
# the lex+parse sweep -- rapidprotoc already parsed all 8018, so re-parsing them costs ~47s to
# re-prove a strict subset. Runs AFTER the build stages: it consumes their rapidprotoc.
#
# A missing build is a FAILURE, not a skip: if this quietly returned 0 whenever build/gcc was
# absent, dropping `gcc` from a stage list would silently disarm the gate instead of breaking it.
# (Absence of the corpus itself IS a legitimate skip -- nobody must fetch ~100 MB to run the gate --
# and corpus_gate.py decides that, distinguishing "nothing fetched" from "partially fetched".)
job_corpus() {
  local rc=0 out
  if [[ ! -x ./build/gcc/rapidprotoc || ! -x ./build/gcc/rapidproto_tests ]]; then
    echo ">> build/gcc missing: the corpus stage needs the gcc stage's binaries"
    echo "   (run ./check.sh, or include 'gcc' in RAPIDPROTO_GATE_STAGES)"
    return 1
  fi
  python3 tests/corpus_gate.py --rapidprotoc ./build/gcc/rapidprotoc --jobs "$JOBS" || rc=1
  # Key on Catch2's own verdict, never on a substring like "skipped": a schema PATH containing
  # that word (googleapis is ~8000 files) would otherwise turn a failing run green.
  out=$(./build/gcc/rapidproto_tests "[corpus]~[sweep]" 2>&1)
  if grep -qE '^[[:space:]]*(assertions|test cases):.*[0-9]+ failed|FAILED' <<<"$out"; then
    echo ">> corpus tests failed:"
    grep -E 'FAILED|with expansion|assertions:' <<<"$out" | head -30
    rc=1
  else
    grep -oE '(All tests passed.*|test cases:.*)' <<<"$out" | head -1
  fi
  return "$rc"
}

# Randomized differential against protobuf: build random messages with protobuf, decode the same
# bytes both ways, compare every field (tests/differential.py). Needs protoc + the protobuf Python
# bindings and skips itself when either is absent -- they are dev-only, like protozero and Catch2 --
# so this must not be the only thing standing between a change and the gate.
job_differential() {
  if [[ ! -x ./build/gcc/rapidprotoc || ! -x ./build/gcc/rapidproto_diffgen ]]; then
    echo ">> build/gcc missing: the differential stage needs the gcc stage's binaries"
    echo "   (run ./check.sh, or include 'gcc' in RAPIDPROTO_GATE_STAGES)"
    return 1
  fi
  python3 tests/differential.py --build-dir ./build/gcc --jobs "$JOBS"
}

# THE stage table. Adding a gate stage means one key here, one title, one job -- and nothing else:
# the run loops, the log capture, the printing and the pass/fail aggregation are all driven off this
# list. The previous shape needed six separate edits, two of which (the default allow-list and the
# rc_<name> aggregation) failed SILENTLY when missed, leaving a stage that could only report success.
readonly STAGE_KEYS=(format docs fixtures gcc clang cf fuzz tidy corpus cxx20 differential)

# What a bare ./check.sh runs. `corpus` is deliberately absent: sweeping ~8000 third-party schemas is
# a COMPATIBILITY check, not a fast-feedback one -- the library's own behaviour is covered by the
# explicit tests -- and at ~163s it was 30% of the gate. It moved to the deep tier (which gates every
# PR) and keeps its own CI runner, so nothing stopped watching it; it just left the inner loop.
readonly DEFAULT_STAGES=(format docs fixtures gcc clang cf fuzz tidy cxx20 differential)

stage_title() {
  case $1 in
    format)       echo "clang-format (check)" ;;
    docs)         echo "doc links" ;;
    fixtures)     echo "corpus fixture coverage" ;;
    gcc)          echo "build + test (gcc)" ;;
    clang)        echo "build + test (clang)" ;;
    cf)           echo "compile-fail (generated decoder rejects misuse)" ;;
    fuzz)         echo "fuzz harness compile-check" ;;
    tidy)         echo "clang-tidy (library = strict, tests = relaxed)" ;;
    corpus)       echo "real-world schema corpus" ;;
    cxx20)        echo "generated headers at c++20/c++23" ;;
    differential) echo "randomized differential vs protobuf" ;;
  esac
}

stage_job() {
  case $1 in
    format)       job_format ;;
    docs)         job_doc_links ;;
    fixtures)     job_fixtures ;;
    gcc)          job_build_test gcc ;;
    clang)        job_build_test clang ;;
    cf)           job_compile_fail ;;
    fuzz)         job_fuzz_compile ;;
    tidy)         job_tidy ;;
    corpus)       job_corpus ;;
    cxx20)        job_cxx20_smoke ;;
    differential) job_differential ;;
  esac
}

# Which stages run (default: all). CI splits them across runner jobs -- the build/test stages in one,
# tidy shards in a matrix -- so wall-clock is the slowest runner. An unknown key is a hard error: a
# comma instead of a space, or one typo, used to skip EVERY stage and report ALL GREEN in a second.
if [[ -n "${RAPIDPROTO_GATE_STAGES:-}" ]]; then
  for want in $RAPIDPROTO_GATE_STAGES; do
    known=0
    for key in "${STAGE_KEYS[@]}"; do [[ "$want" == "$key" ]] && known=1; done
    if [[ $known -eq 0 ]]; then
      echo ">> unknown gate stage '$want' in RAPIDPROTO_GATE_STAGES" >&2
      echo ">> valid stages (space-separated): ${STAGE_KEYS[*]}" >&2
      exit 2
    fi
  done
fi

stage_enabled() {
  [[ " ${RAPIDPROTO_GATE_STAGES:-${DEFAULT_STAGES[*]}} " == *" $1 "* ]]
}

# Records the outcome and duration BESIDE the log, because a concurrent stage runs in a subshell and
# cannot assign to a parent variable -- which is exactly how a hand-written rc_<name> came to be
# forgotten. Reading it back from disk means an unrecorded stage is visibly absent, not silently 0.
run_stage() {  # $1 = stage key
  local key=$1 start rc
  if ! stage_enabled "$key"; then
    echo "stage skipped (RAPIDPROTO_GATE_STAGES)" >"$LOG/$key"
    echo skipped >"$LOG/$key.rc"
    return 0
  fi
  start=$SECONDS
  stage_job "$key" >"$LOG/$key" 2>&1
  rc=$?
  echo "$rc" >"$LOG/$key.rc"
  echo "$((SECONDS - start))" >"$LOG/$key.dur"
  return "$rc"
}

# --- run all stages, capturing each to its own log ------------------------------------------------
# Concurrent by default: on a dev box, wall-clock = the slowest stage. On memory-tight runners the
# stages' COMBINED peak OOMs the whole job (CI's private runners: 2 cores/7 GB -- reproduced locally
# under a 7 GB cgroup: SIGTERM, no output), so under GITHUB_ACTIONS the stages run one at a time --
# identical checks, bounded peak. Override either way with RAPIDPROTO_GATE_SERIAL=1/0.

if [[ "${RAPIDPROTO_GATE_SERIAL:-${GITHUB_ACTIONS:+1}}" == "1" ]]; then
  # Progress lines go straight to stdout (stage output stays buffered): if the runner kills the
  # job anyway, the last line names the guilty stage.
  for key in format docs fixtures gcc clang cf fuzz tidy; do
    echo "serial gate: $key"
    run_stage "$key"
  done
else
  stage_pids=()
  for key in format docs fixtures gcc clang cf fuzz tidy; do
    run_stage "$key" & stage_pids+=("$!")
  done
  # Outcomes come from $LOG/<key>.rc, not from wait: each stage records its result where every
  # consumer reads it, so there is no second place to keep in sync.
  for pid in "${stage_pids[@]}"; do wait "$pid" || true; done
fi

# After the build stages, never alongside them: this one consumes build/gcc's rapidprotoc.
run_stage corpus
# Needs the goldens on disk (not a build product), so it can run any time after them.
run_stage cxx20
# Also consumes build/gcc's binaries, and compiles a harness per schema, so it runs alone at the end.
run_stage differential

# --- print each stage's output in a fixed order (already captured, so never interleaved) ----------

for key in cxx20 format docs fixtures gcc clang cf fuzz tidy corpus differential; do
  section "$(stage_title "$key")"
  cat "$LOG/$key"
done

fail=0
failed_stages=(); ran=0; skipped=()
for key in "${STAGE_KEYS[@]}"; do
  rc="$(cat "$LOG/$key.rc" 2>/dev/null || echo missing)"
  case "$rc" in
    0)       ran=$((ran + 1)) ;;
    skipped) skipped+=("$key") ;;
    # "missing" lands here too: a stage that never recorded a result must not read as a pass.
    *)       ran=$((ran + 1)); fail=1; failed_stages+=("$key") ;;
  esac
done

section "summary"
for key in "${STAGE_KEYS[@]}"; do
  dur="$(cat "$LOG/$key.dur" 2>/dev/null || true)"
  [[ -n "$dur" ]] && printf '  %-13s %4ss\n' "$key" "$dur"
done
echo "ran $ran/${#STAGE_KEYS[@]} stages${skipped:+ (skipped: ${skipped[*]})}"
echo "stage logs: $LOG"
if [[ "$fail" == "0" ]]; then
  echo "ALL GREEN"
else
  echo "FAILURES: ${failed_stages[*]}"
fi
exit "$fail"
