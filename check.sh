#!/usr/bin/env bash
#
# One-stop quality gate for rapidproto: clang-format, build + test on both
# compilers (gcc-13, clang-20), and clang-tidy (strict on the library, relaxed on
# tests). Operates only on our own sources -- never the vendored Catch2 amalgam or
# the thin CLI driver src/main.cpp.
#
#   ./check.sh        # full gate: format check, doc links, nsedge fixture coverage, build+test on
#                     # both compilers, compile-fail, fuzz-compile, clang-tidy, the C++20/23 header
#                     # smoke and the randomized differential. NOT the corpus sweep -- see `deep`.
#   ./check.sh fix    # first apply clang-format, then run the full gate
#   ./check.sh quick  # fast inner loop: apply formatting + gcc build+test only (no clang/tidy)
#   ./check.sh deep   # OPT-IN heavy tier (CI / end-of-phase, NOT the inner loop): ASan+UBSan over the
#                     # full suite, coverage with a line floor, the real-world corpus sweep, and a
#                     # fuzz smoke over the four targets
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
# compile-fail, fuzz-compile, clang-tidy) run concurrently; each build is a parallel build and
# clang-tidy is parallelized across files. Three run after that block: corpus (only when asked for)
# and the differential consume the gcc stage's binaries, and the C++20/23 smoke needs the goldens. Per-stage output is captured and printed in a fixed
# order so nothing interleaves. Exits non-zero if anything is not clean.

set -uo pipefail
cd "$(dirname "$0")"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-20}"   # overridable so the gate can be tested against a broken tool
CLANG_TIDY="${CLANG_TIDY:-clang-tidy-20}"   # overridable so the gate can be tested against a broken tool
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
LIB_SRC=(src/lexer.cpp src/interpret.cpp src/parser.cpp src/features.cpp src/resolve.cpp src/resolver.cpp src/source.cpp src/streamgen/generator.cpp src/codegen/naming.cpp src/arenagen/layout.cpp src/arenagen/modes.cpp src/arenagen/generator.cpp src/dumpgen/generator.cpp src/header_self_contained.cpp)
TEST_SRC=(tests/test_*.cpp)
CLI_SRC=(src/main.cpp src/rapidprotoc/main.cpp tests/diffgen/main.cpp)
EXTRA_SRC=(tests/bench_streamgen.cpp tests/bench_stream_isolated.cpp tests/bench_arena.cpp tests/fuzz/*.cpp examples/*/*.cpp)
# Test-helper headers (the dump / temp_dir utilities our tests #include) -- every tests/*.hpp EXCEPT
# the vendored Catch2 amalgam, so a newly-added helper can't silently escape the format gate.
TEST_HDR=()
for _h in tests/*.hpp; do [[ "$_h" == tests/catch_amalgamated.hpp ]] || TEST_HDR+=("$_h"); done
# SRC_HDR: private headers under src/. There are none today, and the glob is deliberately kept: such
# a header is outside HEADERS (include/-only) and is not a TU, so without naming it here it escapes
# BOTH the format check and clang-tidy's --header-filter. That gap was found the hard way -- a
# private parser header sat unlinted until a review caught it.
SRC_HDR=()
for _h in src/*.hpp src/*/*.hpp; do [[ -f "$_h" ]] && SRC_HDR+=("$_h"); done
FORMAT_FILES=("${HEADERS[@]}" "${SRC_HDR[@]}" "${LIB_SRC[@]}" "${TEST_SRC[@]}" "${CLI_SRC[@]}" "${EXTRA_SRC[@]}" "${TEST_HDR[@]}")

section() { printf '\n=== %s ===\n' "$1"; }

# `./check.sh deeep` used to run the default gate and report ALL GREEN -- the one place a typo gives
# false confidence about which tier ran.
case "${1:-}" in
  ""|fix|quick|deep) ;;
  *) echo ">> unknown argument '$1' -- expected one of: fix, quick, deep (or no argument)" >&2
     exit 2 ;;
esac
if [[ $# -gt 1 ]]; then   # otherwise `./check.sh deep --fast` silently runs plain `deep`
  echo ">> check.sh takes at most one argument; got: $*" >&2
  exit 2
fi

if [[ "${1:-}" == "fix" ]]; then
  section "clang-format (apply)"
  if ! "$CLANG_FORMAT" -i "${FORMAT_FILES[@]}"; then
    echo ">> $CLANG_FORMAT exited non-zero -- the tree may be PARTIALLY formatted (a stale path in"
    echo "   LIB_SRC/CLI_SRC does this: clang-format rewrites the good files, then fails on the bad)"
    exit 1
  fi
  echo "formatted ${#FORMAT_FILES[@]} files"
fi

# Fast inner loop: apply formatting, then gcc build + test only (skips clang, clang-tidy,
# compile-fail, and the stress compile). Run the full gate (`./check.sh`) before committing.
if [[ "${1:-}" == "quick" ]]; then
  section "clang-format (apply)"
  if ! "$CLANG_FORMAT" -i "${FORMAT_FILES[@]}"; then
    echo ">> $CLANG_FORMAT exited non-zero -- the tree may be PARTIALLY formatted (a stale path in"
    echo "   LIB_SRC/CLI_SRC does this: clang-format rewrites the good files, then fails on the bad)"
    exit 1
  fi
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
  test_out=$(./build/gcc/rapidproto_tests  2>&1); test_rc=$?
  # BOTH: Catch2 prints its summary before the process exits, so an at-exit failure (LeakSanitizer,
  # a static-destructor crash, an atexit abort) leaves "All tests passed" in the output of a run
  # that returned non-zero. Text alone called a segfaulting binary clean.
  if [[ $test_rc -eq 0 ]] && grep -qE 'All tests passed' <<<"$test_out"; then
    grep -oE 'All tests passed.*' <<<"$test_out"
    echo ">> quick OK (gcc only -- run ./check.sh for the full gate before committing)"
    exit 0
  fi
  echo ">> tests failed (gcc):"
  # -A3: the value lines FOLLOW 'with expansion:', so matching the header alone logged the failing
  # file:line but never the actual-vs-expected. Also catch a binary that died before Catch2 printed.
  # Per-failure detail is capped; the totals line is printed separately so the cap can never eat it
  # (-A3 multiplies every match by four, which silently truncated the summary before).
  grep -E -A3 'FAILED|with expansion' <<<"$test_out" | head -40
  grep -E '^[[:space:]]*(test cases|assertions):' <<<"$test_out"
  # Same three shapes as job_build_test: assertion failure, at-exit failure after a clean summary,
  # or no report at all. The middle one is invisible in Catch2's output, so name it explicitly.
  if grep -qE 'All tests passed' <<<"$test_out"; then
    echo "   (Catch2 reported success, then the binary exited $test_rc -- at-exit failure:)"
    tail -10 <<<"$test_out"
  elif [[ -z "$(grep -E 'FAILED|assertions:' <<<"$test_out")" ]]; then
    echo "   (no Catch2 output -- the binary exited $test_rc without reporting; last lines:)"
    tail -10 <<<"$test_out"
  fi
  exit 1
fi

# Opt-in heavy tier: the dynamic-analysis tooling (sanitizers, coverage, fuzzing). Deliberately NOT
# part of the default gate -- three instrumented builds + a fuzz run are too slow for every small
# change. Run it in CI or at the end of a phase. Requires clang-20 + llvm-{cov,profdata}-20.
# Build what this stage consumes, rather than trying to DETECT whether it is stale. Two mtime
# heuristics and one build-plan grep all got that wrong -- the grep both false-positived (any
# checkout path containing `/c++` matched the plan text and wedged the stage forever) and
# false-negatived (header dependencies live in compiler_depend.make, which is empty until a second
# build's `depend` step, so `make -n` cannot see them). Building is a no-op of a second or two when
# up to date, and afterwards the binaries are fresh BY CONSTRUCTION rather than by inference.
ensure_gcc_binaries() {  # $@ = the targets THIS stage consumes
  local out target
  # `cmake --build --target X` is NOT a target-existence check: under Makefiles it degenerates to
  # `make X`, and if X is no longer a target but build/gcc/X still exists as a file, make prints
  # "Nothing to be done" and exits 0. A renamed target -- or one disabled by a stale
  # -DRAPIDPROTO_BUILD_TESTS=OFF configure -- would then freeze the binary and pass forever.
  # Capture then grep -- `... | grep -q` under `set -o pipefail` reports failure on a MATCH (grep
  # exits early, upstream takes SIGPIPE). That inversion has now bitten this file twice.
  local targets
  targets=$(cmake --build --preset gcc --target help 2>/dev/null)
  for target in "$@"; do
    if ! grep -qE "(^|\.\.\. )$target\$" <<<"$targets"; then
      echo ">> '$target' is not a target of build/gcc: the configure is stale or the target was"
      echo "   renamed, and this stage would run against a frozen binary (re-run cmake --preset gcc)"
      return 1
    fi
  done
  if ! out=$(cmake --build --preset gcc --target "$@" -j"$JOBS" 2>&1); then
    echo ">> could not build $*: this stage would otherwise run against a stale or missing binary"
    tail -20 <<<"$out"
    return 1
  fi
}

if [[ "${1:-}" == "deep" ]]; then
  CXX=clang++-20; CC=clang-20
  FUZZ_TIME=${FUZZ_TIME:-30}   # seconds per fuzz target
  COV_FLOOR=${COV_FLOOR:-85}   # minimum library line-coverage %
  deep_fail=0

  section "ASan + UBSan (full suite)"
  # -DRAPIDPROTO_BUILD_TESTS=ON explicitly: a cached OFF from an earlier configure left the target
  # absent, and under the Makefiles generator `cmake --build --target X` then exits 0 doing nothing
  # WHEN a file of that name already exists (without one it fails loudly) -- so a leftover binary
  # from an earlier run was silently re-tested. Output is kept so a failure says which step broke.
  if san_build=$(cmake -S . -B build/san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$CC" \
       -DCMAKE_CXX_COMPILER="$CXX" -DRAPIDPROTO_SANITIZE=ON -DRAPIDPROTO_BUILD_TESTS=ON 2>&1) \
     && san_build=$(cmake --build build/san --target rapidproto_tests -j"$JOBS" 2>&1); then
    # DEBUGINFOD_URLS= for the same reason as the fuzz section: LeakSanitizer symbolizes its report
    # through llvm-symbolizer, which issues one debuginfod lookup per module whose local DWARF does
    # not cover a frame (Ubuntu ships /etc/debuginfod/*.urls and exports it into every shell). Where
    # the server is unreachable that lookup burns the full 90s DEBUGINFOD_TIMEOUT rather than 404ing
    # fast: measured on a leaking suite, 1m40s inherited vs 10s cleared. Only a FAILING run pays it,
    # i.e. exactly when the report is needed.
    san_out=$(DEBUGINFOD_URLS= UBSAN_OPTIONS=print_stacktrace=1 ASAN_OPTIONS=detect_leaks=1 \
      ./build/san/rapidproto_tests 2>&1); san_rc=$?
    # BOTH conditions. The exit status is not redundant with the Catch2 line: LeakSanitizer reports
    # at exit, AFTER "All tests passed" is printed, so a leaking run matches the text and only the
    # non-zero status catches it. (The old `| grep -q` form got this right by accident -- pipefail
    # propagated the binary's rc -- and rewriting it as a bare grep silently disabled leak detection,
    # which is the project's only leak gate.)
    if [[ $san_rc -eq 0 ]] && grep -qE 'All tests passed' <<<"$san_out"; then
      echo "sanitizers clean"
    else
      # Print it: a CI runner cannot "re-run the binary" after the job ends.
      echo ">> ASan/UBSan finding or test failure (exit $san_rc):"
      # head, not tail: ASan prints ERROR/stack/SUMMARY first and 20 lines of shadow-byte legend
      # last, so `tail -30` showed only the legend for every memory error. Add the SUMMARY line back
      # explicitly for leaks, whose report comes last.
      head -40 <<<"$san_out"
      grep -E '^(SUMMARY|==[0-9]+==ERROR)' <<<"$san_out" | head -5
      deep_fail=1
    fi
  else
    echo ">> sanitizer build failed:"; tail -20 <<<"$san_build"; deep_fail=1
  fi

  section "coverage (library line floor ${COV_FLOOR}%)"
  # -DRAPIDPROTO_BUILD_TESTS=ON for the same reason as build/san: a cached OFF leaves the target
  # absent, and with a leftover build/cov/rapidproto_tests on disk the build then exits 0 doing
  # nothing, so the floor was graded against whatever that stale binary happened to cover.
  if cov_build=$(cmake -S . -B build/cov -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$CC" \
       -DCMAKE_CXX_COMPILER="$CXX" -DRAPIDPROTO_COVERAGE=ON -DRAPIDPROTO_BUILD_TESTS=ON 2>&1) \
     && cov_build=$(cmake --build build/cov --target rapidproto_tests -j"$JOBS" 2>&1); then
    # Status checked, not discarded: a suite that crashes here used to be noticed only if coverage
    # happened to drop below the floor, which is not a test result.
    cov_out=$(LLVM_PROFILE_FILE=build/cov/cov.profraw ./build/cov/rapidproto_tests 2>&1); cov_rc=$?
    if [[ $cov_rc -ne 0 ]]; then
      echo ">> coverage run failed (exit $cov_rc):"; tail -20 <<<"$cov_out"; deep_fail=1
    fi
    # Status checked, and the grading is INSIDE the else: a failed merge (unwritten, corrupt or
    # version-mismatched .profraw) leaves the PREVIOUS run's cov.profdata on disk, so reporting past
    # it printed a confident percentage read from a profile this run never produced.
    if ! merge_out=$(llvm-profdata-20 merge -sparse build/cov/cov.profraw \
         -o build/cov/cov.profdata 2>&1); then
      echo ">> llvm-profdata merge failed -- coverage not graded (the profile on disk is stale):"
      tail -10 <<<"$merge_out"; deep_fail=1
    else
      cov=$(llvm-cov-20 report ./build/cov/rapidproto_tests -instr-profile=build/cov/cov.profdata \
              -ignore-filename-regex='(tests/|build/|wellknown_generated|catch_amalgamated)' 2>/dev/null \
              | awk '/^TOTAL/{print $10}')
      echo "library line coverage: ${cov:-unknown}"
      if awk -v c="${cov%\%}" -v f="$COV_FLOOR" 'BEGIN{exit !(c+0 >= f+0)}'; then
        echo "at or above floor (${COV_FLOOR}%)"
      else
        echo ">> coverage ${cov} below floor ${COV_FLOOR}%"; deep_fail=1
      fi
    fi
  else
    echo ">> coverage build failed:"; tail -20 <<<"$cov_build"; deep_fail=1
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
  parser_tus=(src/lexer.cpp src/parser.cpp src/features.cpp src/resolve.cpp src/interpret.cpp
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
  cmake --preset gcc >/dev/null 2>&1
  # Same construction as job_corpus: build, do not guess. The deep tier had no freshness handling at
  # all, so it swept 8018 schemas with whatever binary happened to be on disk.
  ensure_gcc_binaries rapidprotoc rapidproto_tests || deep_fail=1
  if [[ -x ./build/gcc/rapidprotoc ]]; then
    corpus_rc=0
    python3 tests/corpus_gate.py --rapidprotoc ./build/gcc/rapidprotoc --jobs "$JOBS" || corpus_rc=$?
    # 77 = not fetched: a skip, not a failure. CI's deep job never fetches the corpus, so treating
    # it as a failure would have turned that job red on every PR.
    [[ $corpus_rc -ne 0 && $corpus_rc -ne 77 ]] && deep_fail=1
    # job_corpus runs the sweep AND the [corpus] resolver cases; both moved, not just the sweep.
    if [[ -x ./build/gcc/rapidproto_tests ]]; then
      corpus_cases_rc=0
      corpus_out=$(./build/gcc/rapidproto_tests "[corpus]~[sweep]" 2>&1) || corpus_cases_rc=$?
      if [[ $corpus_cases_rc -ne 0 && $corpus_cases_rc -ne 4 ]]; then
        echo ">> corpus resolver cases exited $corpus_cases_rc:"; tail -5 <<<"$corpus_out"
        deep_fail=1
      fi
      if grep -qE '^[[:space:]]*(assertions|test cases):.*[0-9]+ failed|FAILED' <<<"$corpus_out"; then
        echo ">> corpus resolver cases failed:"; tail -20 <<<"$corpus_out"; deep_fail=1
      else
        tail -1 <<<"$corpus_out"
      fi
    fi
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
# Per-run directory, symlinked to a stable name. A single fixed path let a second, overlapping run
# `rm -rf` the first's results: the first then printed the second's logs and reported ALL GREEN with
# its own failure erased. The symlink keeps "read build/gate-logs" true for the last run started.
mkdir -p build
LOG="$(mktemp -d build/gate-logs.XXXXXX)"   # mktemp, not $$: PIDs repeat in containers
rm -rf build/gate-logs   # a plain `ln -sfn` into an existing directory links INSIDE it
ln -sfn "$(basename "$LOG")" build/gate-logs
# Keep the last few runs, not every run since the epoch. Skip any directory still marked live: a
# running gate's mtime goes quiet during a long stage (tidy is 5+ min of silence), so an mtime sort
# happily reaps a concurrent run's logs out from under it.
echo $$ >"$LOG/.live"   # PID, not a bare marker: a SIGKILLed run runs no trap to clear it
while IFS= read -r old_log; do
  if [[ -f "$old_log/.live" ]] && kill -0 "$(cat "$old_log/.live" 2>/dev/null)" 2>/dev/null; then
    continue   # a genuinely running gate -- leave its logs alone
  fi
  rm -rf "$old_log"   # stale marker from a hard-killed run: reap it, or the bound decays forever
done < <(ls -dt build/gate-logs.* 2>/dev/null | tail -n +4)
# A CI kill (OOM, preemption, cancellation) arrives as SIGTERM and would discard every buffered
# stage log -- exactly when the logs matter most. Dump whatever was captured before dying.
trap 'echo ">> check.sh: killed (SIGTERM/SIGINT) -- dumping captured stage logs"; rm -f "$LOG/.live"; for f in "$LOG"/*; do [[ -f "$f" ]] && { echo "--- ${f##*/} ---"; cat "$f"; }; done; exit 143' TERM INT
trap 'rm -f "$LOG/.live"' EXIT

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
  if ! "$CLANG_FORMAT" --version >/dev/null 2>&1; then
    echo ">> $CLANG_FORMAT is missing or not runnable: the format stage cannot run"
    return 1
  fi
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
  local preset=$1 build_out test_out rc test_rc
  build_out=$(cmake --build --preset "$preset" -j"$JOBS" 2>&1); rc=$?
  if [[ $rc -ne 0 ]] || grep -qE 'error:|warning:' <<<"$build_out"; then
    echo ">> build problems ($preset):"
    grep -E 'error:|warning:' <<<"$build_out" | head -30
    return 1
  fi
  echo "build clean ($preset)"
  test_out=$(./build/"$preset"/rapidproto_tests  2>&1); test_rc=$?
  # BOTH: Catch2 prints its summary before the process exits, so an at-exit failure (LeakSanitizer,
  # a static-destructor crash, an atexit abort) leaves "All tests passed" in the output of a run
  # that returned non-zero. Text alone called a segfaulting binary clean.
  if [[ $test_rc -eq 0 ]] && grep -qE 'All tests passed' <<<"$test_out"; then
    grep -oE 'All tests passed.*' <<<"$test_out"
  else
    echo ">> tests failed ($preset):"
    # -A3 so the value lines that FOLLOW 'with expansion:' survive; the totals are printed OUTSIDE
    # the cap, because -A3 multiplies every match by four and used to push them past `head`.
    grep -E -A3 'FAILED|with expansion' <<<"$test_out" | head -40
    grep -E '^[[:space:]]*(test cases|assertions):' <<<"$test_out"
    # Every failure branch must print SOMETHING. Catch2's own summary explains an assertion
    # failure, but the two other shapes are invisible in it: a run that reported success and then
    # died at exit (LeakSanitizer, a static destructor, an atexit abort -- the report follows the
    # summary), and a run that never reported at all.
    if grep -qE 'All tests passed' <<<"$test_out"; then
      echo "   (Catch2 reported success, then the binary exited $test_rc -- at-exit failure:)"
      tail -10 <<<"$test_out"
    elif [[ -z "$(grep -E 'FAILED|assertions:' <<<"$test_out")" ]]; then
      echo "   (no Catch2 output -- the binary exited $test_rc without reporting; last lines:)"
      tail -10 <<<"$test_out"
    fi
    return 1
  fi
  # The consumer example (examples/consumer) is built alongside via rapidproto_generate(); run it to
  # confirm the helper-generated decoders (arena + streaming, in one TU) decode at runtime here.
  local example out
  for example in rapidproto_example_consumer rapidproto_example_lean; do
    local path="./build/$preset/examples/consumer/$example"
    [[ -x "$path" ]] || continue
    if out=$("$path" 2>&1); then
      echo "$example: decoded OK ($preset)"
    else
      # Print what it said. Discarding this left the log as a single ">> ... failed" line with no
      # reason, on a stage whose whole job is to explain itself.
      echo ">> $example failed ($preset), rc=$? -- output:"
      head -20 <<<"$out"
      return 1
    fi
  done
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
  # (see .clang-tidy for why a bare 'rapidproto/.*' is wrong), plus src/ so a PRIVATE header there is
  # linted too -- without that alternative its contents are checked by nothing at all.
  local rc
  out=$("$CLANG_TIDY" -p build/clang --quiet \
    --header-filter='(include/rapidproto|src)/.*\.hpp' "$f" 2>/dev/null); rc=$?
  # A crash or a bad invocation exits non-zero while printing nothing a diagnostic grep would catch;
  # without this the TU silently counted as clean. Only a non-zero status with nothing to show is
  # fatal: some clang-tidy versions exit non-zero alongside real diagnostics, and this repo's
  # clang-tidy-20 exits 0 with them, so the status alone decides nothing.
  if [[ $rc -ne 0 && -z "$(grep -E 'warning:|error:' <<<"$out")" ]]; then
    # `|| return 1`: an unwritable TIDY_D silently dropped this report, which reads as clean. A
    # non-zero return makes xargs fail the fan-out instead.
    { printf '>> %s\n' "$f"
      echo "   $CLANG_TIDY exited $rc with no diagnostics -- treating as a failure, not as clean"
    } >"$TIDY_D/$(tr / _ <<<"$f")" || return 1
    return 0
  fi
  out=$(grep -E 'warning:|error:' <<<"$out")
  if [[ -n "$out" ]]; then
    { printf '>> %s\n' "$f"; head -20 <<<"$out"; } >"$TIDY_D/$(tr / _ <<<"$f")" || return 1
  fi
}
export -f tidy_one

job_tidy() {
  # A missing binary produces no stdout and tidy_one greps stdout only -- so without this the stage
  # reported `tidy clean` and rc 0 on a box with no clang-tidy at all: a silent pass, not a skip.
  # Run it, do not merely look it up: `command -v` answers "is there a candidate", not "does it
  # work", and returns non-executable candidates too. A mode-644 file, a broken wrapper that exits
  # non-zero, and one that writes only to stderr all reported `tidy clean` before this.
  if ! "$CLANG_TIDY" --version >/dev/null 2>&1; then
    echo ">> $CLANG_TIDY is missing or not runnable: the tidy stage cannot run (install it, or drop"
    echo "   'tidy' from RAPIDPROTO_GATE_STAGES to skip it deliberately)"
    return 1
  fi
  if [[ ! -f build/clang/compile_commands.json ]]; then
    echo ">> build/clang/compile_commands.json missing"
    return 1
  fi
  # Checked because this file runs without `set -e`. This catches only "cannot create"; an existing
  # directory that is unwritable passes mkdir -p and is caught in tidy_one, where the write is.
  TIDY_D="$LOG/tidy.d"
  if ! mkdir -p "$TIDY_D"; then echo ">> cannot create $TIDY_D"; return 1; fi
  export TIDY_D
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
  # An empty selection is a broken invocation, not a clean lint: it reported `tidy clean (0 TUs)`.
  if [[ ${#tus[@]} -eq 0 ]]; then
    echo ">> shard $shard selects no TUs out of $tu_index -- nothing was linted"; return 1
  fi
  # Status checked: tidy_one reports by WRITING a log, so "no logs" means "clean" -- which is also
  # what a fan-out that never ran looks like. Measured: a bad -P argument exits 1, a child killed by
  # a signal makes xargs exit 125, and a child that cannot write its log exits 123 -- all of which
  # used to be read as clean.
  if ! printf '%s\n' "${tus[@]}" | xargs -P"$JOBS" -I{} bash -c 'tidy_one "$@"' _ {}; then
    echo ">> clang-tidy fan-out failed -- TUs may not have been linted"
    cat "$TIDY_D"/* 2>/dev/null
    return 1
  fi
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
# A stage that consumes build/gcc's binaries must not silently test a STALE one: `rapidprotoc`
# embeds the runtime headers at build time, so with an out-of-date binary the differential happily
# reported 0 mismatches against a decoder bug that was already in the tree. Existence is not enough.
job_corpus() {
  local rc=0 out
  if [[ ! -x ./build/gcc/rapidprotoc || ! -x ./build/gcc/rapidproto_tests ]]; then
    echo ">> build/gcc missing: the corpus stage needs the gcc stage's binaries"
    echo "   (run ./check.sh, or include 'gcc' in RAPIDPROTO_GATE_STAGES)"
    return 1
  fi
  ensure_gcc_binaries rapidprotoc rapidproto_tests || return 1
  local sweep_rc=0
  python3 tests/corpus_gate.py --rapidprotoc ./build/gcc/rapidprotoc --jobs "$JOBS" || sweep_rc=$?
  # 77 = "corpus not fetched", the documented skip -- propagate it so the stage self-skips instead
  # of failing. Nobody should have to fetch ~100 MB to run an unrelated stage.
  [[ $sweep_rc -eq 77 ]] && return 77
  [[ $sweep_rc -ne 0 ]] && rc=1
  # Key on Catch2's own verdict, never on a substring like "skipped": a schema PATH containing
  # that word (googleapis is ~8000 files) would otherwise turn a failing run green.
  local cases_rc=0
  out=$(./build/gcc/rapidproto_tests "[corpus]~[sweep]" 2>&1) || cases_rc=$?
  # Absence of failure evidence is not a pass: a renamed [corpus] tag makes Catch2 exit 2 with
  # "No tests ran", which matched no failure pattern and reported green while checking nothing.
  # 4 is Catch2's AllTestsSkippedExitCode -- the legitimate "corpus not fetched" outcome. Keying on
  # the VALUE, not on the `assertions: - none -` text, which laundered any other status the same way.
  if [[ $cases_rc -ne 0 && $cases_rc -ne 4 ]]; then
    echo ">> corpus resolver cases exited $cases_rc:"; tail -5 <<<"$out"; rc=1
  fi
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
  ensure_gcc_binaries rapidprotoc rapidproto_diffgen || return 1
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
    # Not optional: without it, a key added to STAGE_KEYS but not here falls out of the case with
    # status 0 and an empty log -- a stage that can only ever report success, which is the exact
    # failure this table was introduced to remove.
    *) echo ">> no job defined for stage '$1'"; return 1 ;;
  esac
}

# Which stages run (default: everything except corpus -- see DEFAULT_STAGES). CI splits them
# across runner jobs -- the build/test stages in one,
# tidy shards in a matrix -- so wall-clock is the slowest runner. An unknown key is a hard error: a
# comma instead of a space, or one typo, used to skip EVERY stage and report ALL GREEN in a second.
# Normalise first: `for want in $VAR` splits on all of IFS, but stage_enabled matches on literal
# SPACES -- so a tab- or newline-separated list validated fine and then enabled nothing, reporting
# `ran 0/11 stages` and ALL GREEN. Same defect class as the comma case this validation was added for.
if [[ -n "${RAPIDPROTO_GATE_STAGES:-}" ]]; then
  RAPIDPROTO_GATE_STAGES="$(tr -s '[:space:]' ' ' <<<"$RAPIDPROTO_GATE_STAGES" | sed 's/^ *//; s/ *$//')"
  if [[ -z "$RAPIDPROTO_GATE_STAGES" ]]; then
    echo ">> RAPIDPROTO_GATE_STAGES is set but empty: that would run nothing and report success" >&2
    echo ">> valid stages (space-separated): ${STAGE_KEYS[*]}" >&2
    exit 2
  fi
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
  # 77 = the stage ran but could not do its work for a documented reason (no protoc, no protobuf
  # bindings, corpus unfetched). Recorded distinctly so the summary can say so: it is not a pass,
  # and it is not a failure. Anything else non-zero is a failure.
  # 77 means "ran, but could not do its work for a documented reason" -- and ONLY corpus and
  # differential document it (corpus_gate.py / differential.py). Honouring it everywhere would let
  # any stage launder an unrelated 77 into a green self-skip.
  if [[ $rc -eq 77 && ( $key == corpus || $key == differential ) ]]; then
    echo selfskip >"$LOG/$key.rc"
    echo "$((SECONDS - start))" >"$LOG/$key.dur"
    return 0
  fi
  echo "$rc" >"$LOG/$key.rc"
  echo "$((SECONDS - start))" >"$LOG/$key.dur"
  # Never leave a failing stage with an empty log: the summary would name it with nothing to read.
  [[ $rc -ne 0 && ! -s "$LOG/$key" ]] &&
    echo ">> stage '$key' exited $rc without producing any output" >"$LOG/$key"
  return "$rc"
}

# --- run all stages, capturing each to its own log ------------------------------------------------
# Concurrent by default: on a dev box, wall-clock = the slowest stage. On memory-tight runners the
# stages' COMBINED peak OOMs the whole job (CI's private runners: 2 cores/7 GB -- reproduced locally
# under a 7 GB cgroup: SIGTERM, no output), so under GITHUB_ACTIONS the stages run one at a time --
# identical checks, bounded peak. Override either way with RAPIDPROTO_GATE_SERIAL=1/0.

serial_gate=$([[ "${RAPIDPROTO_GATE_SERIAL:-${GITHUB_ACTIONS:+1}}" == "1" ]] && echo 1 || echo 0)
if [[ "$serial_gate" == 1 ]]; then
  # Progress lines go straight to stdout (stage output stays buffered): if the runner kills the
  # job anyway, the last line names the guilty stage.
  for key in format docs fixtures gcc clang cf fuzz tidy; do
    stage_enabled "$key" && echo "serial gate: $key"   # no progress line for a stage we skip
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
[[ "$serial_gate" == 1 ]] && stage_enabled corpus && echo "serial gate: corpus"
run_stage corpus
# Needs the goldens on disk (not a build product), so it can run any time after them.
[[ "$serial_gate" == 1 ]] && stage_enabled cxx20 && echo "serial gate: cxx20"
run_stage cxx20
# Also consumes build/gcc's binaries, and compiles a harness per schema, so it runs alone at the end.
[[ "$serial_gate" == 1 ]] && stage_enabled differential && echo "serial gate: differential"
run_stage differential

# --- print each stage's output in a fixed order (already captured, so never interleaved) ----------

for key in cxx20 format docs fixtures gcc clang cf fuzz tidy corpus differential; do
  section "$(stage_title "$key")"
  cat "$LOG/$key"
done

fail=0
failed_stages=(); ran=0; skipped=(); selfskipped=()
for key in "${STAGE_KEYS[@]}"; do
  rc="$(cat "$LOG/$key.rc" 2>/dev/null || echo missing)"
  case "$rc" in
    0)        ran=$((ran + 1)) ;;
    skipped)  skipped+=("$key") ;;
    selfskip) ran=$((ran + 1)); selfskipped+=("$key") ;;
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
[[ ${#selfskipped[@]} -gt 0 ]] && echo "ran but self-skipped (missing tool or data): ${selfskipped[*]}"
echo "stage logs: $LOG"
if [[ "$fail" == "0" ]]; then
  echo "ALL GREEN"
else
  echo "FAILURES: ${failed_stages[*]}"
fi
exit "$fail"
