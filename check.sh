#!/usr/bin/env bash
#
# One-stop quality gate for rapidproto: clang-format, build + test on both
# compilers (gcc-13, clang-20), and clang-tidy (strict on the library, relaxed on
# tests). Operates only on our own sources -- never the vendored Catch2 amalgam or
# the thin CLI driver src/main.cpp.
#
#   ./check.sh        # full gate: format check, doc links, script syntax + fixture coverage, build+test on
#                     # both compilers, compile-fail, fuzz-compile, clang-tidy, the C++20/23 header
#                     # smoke, the CMake-helper name check and the randomized differential. NOT the
#                     # corpus sweep -- see `deep`.
#   ./check.sh fix    # first apply clang-format, then run the full gate
#   ./check.sh quick  # fast inner loop: apply formatting + gcc build+test only (no clang/tidy)
#   ./check.sh compilers  # the architecture-sensitive stages only (build+test on both compilers,
#                     # compile-fail, fuzz-compile) -- what ci.yml's arm64 job and release.yml's
#                     # arm64 leg run; see below
#   ./check.sh deep   # OPT-IN heavy tier (CI / end-of-phase, NOT the inner loop): ASan+UBSan over the
#                     # full suite, coverage with a line floor, the real-world corpus sweep plus a
#                     # bounded corpus-compile sample, a goldens-reproduce-from-the-regen-scripts
#                     # leg, and a fuzz smoke over the four targets (three decode paths + the schema
#                     # front-end). Slow (three instrumented builds).
#                     # Override: FUZZ_TIME=120 COV_FLOOR=88.
#
# Every stage's output is captured to build/gate-logs/<stage> and kept after the run, so a failure
# you piped past can be read back instead of re-running the gate. The summary names the stages that
# failed, how long each took, and how many ran -- `./check.sh | tail -20` is enough to triage.
#
# RAPIDPROTO_GATE_STAGES='gcc tidy' runs a subset; RAPIDPROTO_GATE_SKIP='tidy' runs the default
# stages minus these (how CI's gate job derives its list). Space-separated; an unknown key, an
# empty value, or setting both is an error.
# RAPIDPROTO_GATE_SERIAL=1 runs stages one at a time (the default under GITHUB_ACTIONS).
#
# The independent stages (format, doc-links, fixture-coverage, gcc build+test, clang build+test,
# fuzz-compile, clang-tidy) run concurrently; each build is a parallel build and clang-tidy is
# parallelized across files. Five run after that block: compile-fail, corpus, the CMake-helper name
# check and the differential all consume the gcc stage's binaries (corpus only when asked for), and
# the C++20/23 smoke needs the goldens. Per-stage output is captured and printed in a fixed order so
# nothing interleaves. Exits non-zero if anything is not clean.

set -uo pipefail

# The floor is a fact three comments state; enforce it so a stock-macOS bash 3.2 gets this
# sentence instead of what it otherwise does: globstar fails NON-fatally, the ** globs silently
# under-match, mapfile is command-not-found, and `local -n` errors at runtime hundreds of lines
# later. (4.4: namerefs, globstar, mapfile -d.)
if ((BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 4))); then
  echo ">> check.sh needs bash >= 4.4 (this is ${BASH_VERSION}). On macOS, run" >&2
  echo "   tests/system_build_test.sh instead -- the system-compiler build/test sequence." >&2
  exit 2
fi
cd "$(dirname "$0")"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-20}"   # overridable so the gate can be tested against a broken tool
CLANG_TIDY="${CLANG_TIDY:-clang-tidy-20}"   # overridable so the gate can be tested against a broken tool
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export CLANG_TIDY

# Our own hand-written sources, DERIVED by glob rather than hand-listed -- the rule ("EVERYTHING
# we author is clang-formatted; formatting is mechanical, so nothing is exempt") lives in the
# exemption list, not in per-file enumeration. A new file under any of these roots is covered the
# moment it exists; previously two hand lists (LIB_SRC's 14 paths, HEADERS' six subdirs) meant a
# new directory or source silently escaped BOTH format and tidy -- the exact gap the SRC_HDR
# comment below records having hit once already. Exempt because formatting would fight their
# vendor/generator: FORMAT_EXEMPT filters ONLY the tests/*.hpp walk (catch_amalgamated.hpp is the
# entry that fires today; the .cpp and wellknown_generated.cpp entries are insurance for a widened
# glob -- the former matches no current glob, the latter is dropped from LIB_SRC by
# TIDY_EXEMPT_SRC). The golden dirs and build/ stay out simply by not being globbed.
shopt -s globstar nullglob
FORMAT_EXEMPT=(tests/catch_amalgamated.hpp tests/catch_amalgamated.cpp src/wellknown_generated.cpp)
# clang-tidy runs on the narrower LIB_SRC + TEST_SRC: the thin CLI drivers (src/*/main.cpp and the
# differential's harness generator), the benches (built -O3, non-strict), the fuzz harnesses, and
# the example consumers are formatted but NOT tidied -- their argv / measurement / harness
# patterns trip strict checks for no real-bug gain.
TIDY_EXEMPT_SRC=(src/main.cpp src/rapidprotoc/main.cpp src/wellknown_generated.cpp)
_exempt() {  # is $1 in the given exemption list ($2...)?
  local f="$1"; shift
  local e
  for e in "$@"; do [[ "$f" == "$e" ]] && return 0; done
  return 1
}
HEADERS=(include/rapidproto/**/*.hpp)
LIB_SRC=()
for _s in src/**/*.cpp; do _exempt "$_s" "${TIDY_EXEMPT_SRC[@]}" || LIB_SRC+=("$_s"); done
TEST_SRC=(tests/test_*.cpp)
CLI_SRC=(src/main.cpp src/rapidprotoc/main.cpp tests/diffgen/main.cpp)
EXTRA_SRC=(tests/bench_*.cpp tests/fuzz/**/*.cpp examples/**/*.cpp)
# Test-helper headers (the dump / temp_dir utilities our tests #include) -- every tests/*.hpp
# except the exempt amalgam, so a newly-added helper can't silently escape the format gate.
TEST_HDR=()
for _h in tests/*.hpp; do
  _exempt "$_h" "${FORMAT_EXEMPT[@]}" || TEST_HDR+=("$_h")
done
# SRC_HDR: private headers under src/. There are none today, and the glob is deliberately kept: such
# a header is outside HEADERS (include/-only) and is not a TU, so without naming it here it escapes
# BOTH the format check and clang-tidy's --header-filter. That gap was found the hard way -- a
# private parser header sat unlinted until a review caught it.
SRC_HDR=(src/**/*.hpp)
FORMAT_FILES=("${HEADERS[@]}" "${SRC_HDR[@]}" "${LIB_SRC[@]}" "${TEST_SRC[@]}" "${CLI_SRC[@]}" "${EXTRA_SRC[@]}" "${TEST_HDR[@]}")
# BOTH options off again: nullglob in particular must not leak into the rest of the file --
# under it, `for f in tests/fuzz/*.cpp` over a moved directory iterates zero times and the fuzz
# stage prints its success line, where the unmatched literal pattern used to fail the compile.
shopt -u globstar nullglob

section() { printf '\n=== %s ===\n' "$1"; }

# `./check.sh deeep` used to run the default gate and report ALL GREEN -- the one place a typo gives
# false confidence about which tier ran.
case "${1:-}" in
  ""|fix|quick|deep|compilers) ;;
  *) echo ">> unknown argument '$1' -- expected one of: fix, quick, deep, compilers (or no argument)" >&2
     exit 2 ;;
esac
if [[ $# -gt 1 ]]; then   # otherwise `./check.sh deep --fast` silently runs plain `deep`
  echo ">> check.sh takes at most one argument; got: $*" >&2
  exit 2
fi

# `compilers` = THE home of the architecture-sensitive stage subset, spelled once here for both
# CI arm64 legs (ci.yml's arm64 job, release.yml's arm64 leg) -- a stage list hand-copied into a
# workflow once left `names` running locally and never in CI. Implemented as a translation to
# RAPIDPROTO_GATE_STAGES so it composes with the normal stage validation; setting the variable
# AND asking for the mode is two stage selections, refused like GATE_STAGES + GATE_SKIP.
GATE_SELECTION_DESC="RAPIDPROTO_GATE_STAGES"   # names the selection in skip logs; modes retitle it
if [[ "${1:-}" == "compilers" ]]; then
  # Both conflicts refused HERE with the mode named -- the generic both-set check further down
  # would blame RAPIDPROTO_GATE_STAGES, a variable this invoker never touched.
  if [[ -n "${RAPIDPROTO_GATE_STAGES+set}" ]]; then
    echo ">> './check.sh compilers' and RAPIDPROTO_GATE_STAGES are both stage selections -- use one" >&2
    exit 2
  fi
  if [[ -n "${RAPIDPROTO_GATE_SKIP+set}" ]]; then
    echo ">> './check.sh compilers' and RAPIDPROTO_GATE_SKIP are both stage selections -- use one" >&2
    exit 2
  fi
  export RAPIDPROTO_GATE_STAGES='gcc clang cf fuzz'
  GATE_SELECTION_DESC="./check.sh compilers"
fi

ensure_targets() {  # $1 = build dir; $@ = the targets THIS stage consumes
  local dir=$1; shift
  local out target targets
  # `cmake --build --target X` is NOT a target-existence check: under Makefiles it degenerates to
  # `make X`, and if X is no longer a target but <dir>/X still exists as a file, make prints
  # "Nothing to be done" and exits 0. A renamed target -- or one disabled by a stale
  # -DRAPIDPROTO_BUILD_TESTS=OFF configure -- would then freeze the binary and pass forever.
  # Capture then grep, never `... | grep -q`: under `set -o pipefail` that reports failure on a
  # MATCH when the writer is still going (grep exits early, upstream takes SIGPIPE). The inversion
  # has bitten this file twice, and `help` output being short enough to fit a pipe buffer today is
  # not a reason to write the fragile form.
  targets=$(cmake --build "$dir" --target help 2>/dev/null)
  for target in "$@"; do
    if ! grep -qE "(^|\.\.\. )$target\$" <<<"$targets"; then
      echo ">> '$target' is not a target of $dir: the configure is stale or the target was"
      echo "   renamed, and this stage would run against a frozen binary (re-run cmake)"
      return 1
    fi
  done
  # Built explicitly, not merely listed: a target excluded from `all` (EXCLUDE_FROM_ALL) stays in
  # the help output and is never rebuilt by a plain `cmake --build`, so the stage would keep
  # running whatever binary an earlier run left behind while every check above passed.
  if ! out=$(cmake --build "$dir" --target "$@" -j"$JOBS" 2>&1); then
    echo ">> could not build $* in $dir: this stage would otherwise run a stale or missing binary"
    tail -20 <<<"$out"
    return 1
  fi
}

ensure_gcc_binaries() { ensure_targets build/gcc "$@"; }

# Above the fix/quick/deep branches on purpose: quick and deep run the test binary and return, and
# fix falls through to a gate that runs it, so a guard further down covered only the default gate. With this variable set, the binary REWRITES the
# goldens it is meant to verify -- a corrupted fixture is overwritten to match the code, ~100
# assertions disappear, and the run reports green. `-v` and not `-n`: the tests check presence with
# getenv() != nullptr, so an empty value regenerates too and a value test misses it entirely.
if [[ -v RAPIDPROTO_REGEN_GOLDEN ]]; then
  echo ">> RAPIDPROTO_REGEN_GOLDEN is set; ignoring it -- the gate verifies goldens, it never" >&2
  echo "   rewrites them (use tests/regen_*_goldens.sh for that)" >&2
  unset RAPIDPROTO_REGEN_GOLDEN
fi

# Apply clang-format to every authored file; shared by `fix` and `quick`.
apply_format() {
  section "clang-format (apply)"
  if ! "$CLANG_FORMAT" -i "${FORMAT_FILES[@]}"; then
    echo ">> $CLANG_FORMAT exited non-zero -- the tree may be PARTIALLY formatted (a stale path in"
    echo "   the source lists does this: clang-format rewrites the good files, then fails on the bad)"
    return 1
  fi
  echo "formatted ${#FORMAT_FILES[@]} files"
}

# Run a built test binary and grade it -- ONE implementation of the three-shape diagnosis
# (assertion failure / at-exit failure after a clean summary / no report at all) plus the
# per-source contribution floor, shared by `quick` and job_build_test. The two used to carry
# byte-similar copies that had already drifted in how they captured the exit status.
run_test_binary() {  # $1 = binary path, $2 = label for messages
  local binary=$1 label=$2 test_out test_rc contrib_out
  test_out=$("$binary" 2>&1); test_rc=$?
  # BOTH: Catch2 prints its summary before the process exits, so an at-exit failure (LeakSanitizer,
  # a static-destructor crash, an atexit abort) leaves "All tests passed" in the output of a run
  # that returned non-zero. Text alone called a segfaulting binary clean.
  if [[ $test_rc -eq 0 ]] && grep -qE 'All tests passed' <<<"$test_out"; then
    grep -oE 'All tests passed.*' <<<"$test_out"
    # A green run proves the cases that EXIST pass; this proves every test source still supplies
    # some -- an emptied or #if 0'd tests/test_*.cpp stays listed, compiled and green otherwise.
    if ! contrib_out=$(tests/check_test_contribution.sh "$binary" 2>&1); then
      echo "$contrib_out"
      return 1
    fi
    tail -1 <<<"$contrib_out"
    return 0
  fi
  echo ">> tests failed ($label):"
  # -A3 so the value lines that FOLLOW 'with expansion:' survive; the totals are printed OUTSIDE
  # the cap, because -A3 multiplies every match by four and used to push them past `head`.
  grep -E -A3 'FAILED|with expansion' <<<"$test_out" | head -40
  grep -E '^[[:space:]]*(test cases|assertions):' <<<"$test_out"
  # Every failure branch must print SOMETHING. Catch2's own summary explains an assertion
  # failure, but the two other shapes are invisible in it.
  if grep -qE 'All tests passed' <<<"$test_out"; then
    echo "   (Catch2 reported success, then the binary exited $test_rc -- at-exit failure:)"
    tail -10 <<<"$test_out"
  elif [[ -z "$(grep -E 'FAILED|assertions:' <<<"$test_out")" ]]; then
    echo "   (no Catch2 output -- the binary exited $test_rc without reporting; last lines:)"
    tail -10 <<<"$test_out"
  fi
  return 1
}

if [[ "${1:-}" == "fix" ]]; then
  apply_format || exit 1
fi

# Fast inner loop: apply formatting, then gcc build + test only (skips clang, clang-tidy,
# compile-fail, and the stress compile). Run the full gate (`./check.sh`) before committing.
if [[ "${1:-}" == "quick" ]]; then
  apply_format || exit 1
  section "build + test (gcc)"
  if ! cmake --preset gcc -DRAPIDPROTO_BUILD_TESTS=ON >/dev/null 2>&1; then
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
  # AFTER the build, same order as job_build_test: ensure_targets itself builds the named target,
  # so running it first would compile the tree with its output swallowed and leave the warning
  # grep above nothing to see. After a full clean build it is a no-op that only proves the TARGET
  # still exists (a renamed target hands the run a stale binary).
  ensure_targets build/gcc rapidproto_tests || exit 1
  run_test_binary ./build/gcc/rapidproto_tests gcc || exit 1
  echo ">> quick OK (gcc only -- run ./check.sh for the full gate before committing)"
  exit 0
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

if [[ "${1:-}" == "deep" ]]; then
  CXX=clang++-20; CC=clang-20
  FUZZ_TIME=${FUZZ_TIME:-30}   # seconds per fuzz target
  # Validated for the same reason as COV_FLOOR below: libFuzzer reads a non-numeric
  # -max_total_time as 0, which means NO limit, so a typo runs each target until the CI job's
  # six-hour ceiling kills it. A literal 0 is refused for the same reason -- it IS that no-limit
  # value, so admitting it would let a truncated env var do exactly what this check exists to stop.
  if ! [[ "$FUZZ_TIME" =~ ^[1-9][0-9]*$ ]]; then
    echo ">> FUZZ_TIME='$FUZZ_TIME' is not a positive number (libFuzzer reads 0, or a value it cannot parse, as 'no limit')" >&2
    exit 2
  fi
  COV_FLOOR=${COV_FLOOR:-85}   # minimum library line-coverage %
  # Validated: awk reads a non-numeric floor as 0, which every coverage figure clears, so the gate
  # printed "at or above floor" while enforcing nothing.
  if ! [[ "$COV_FLOOR" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo ">> COV_FLOOR='$COV_FLOOR' is not a number: the coverage floor would enforce nothing" >&2
    exit 2
  fi
  deep_fail=0
  deep_skipped=()   # legs that ran but could not do their work (exit 77)

  section "ASan + UBSan (full suite)"
  # -DRAPIDPROTO_BUILD_TESTS=ON explicitly: a cached OFF from an earlier configure left the target
  # absent, and under the Makefiles generator `cmake --build --target X` then exits 0 doing nothing
  # WHEN a file of that name already exists (without one it fails loudly) -- so a leftover binary
  # from an earlier run was silently re-tested. Output is kept so a failure says which step broke.
  if san_build=$(cmake -S . -B build/san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER="$CC" \
       -DCMAKE_CXX_COMPILER="$CXX" -DRAPIDPROTO_SANITIZE=ON -DRAPIDPROTO_BUILD_TESTS=ON 2>&1) \
     && san_build=$(ensure_targets build/san rapidproto_tests 2>&1); then
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
     && cov_build=$(ensure_targets build/cov rapidproto_tests 2>&1); then
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
    # Two conditions, two messages. A stale directory that cannot be removed is the one that used
    # to pass: mkdir -p succeeds on it, the copies below fail under `2>/dev/null || true`, and the
    # `ls -A` emptiness warning stays quiet because the PREVIOUS run's seeds are still sitting
    # there -- so the target fuzzed green off stale input. An absent directory that cannot be
    # created was already red (libFuzzer refuses to start), just with a confusing CRASH label.
    if ! rm -rf "$dir"; then
      echo ">> cannot clear $dir -- fuzz_$target would rerun the previous run's seeds"
      deep_fail=1; return 1
    fi
    if ! mkdir -p "$dir"; then
      echo ">> cannot create $dir -- fuzz_$target has nowhere to stage its seeds"
      deep_fail=1; return 1
    fi
    if [[ "$target" == parser ]]; then
      # Every corpus schema, path-flattened. Flattening is not injective (a future
      # tests/corpus/arena/modes.proto would land on arena_modes.proto), so say so rather than
      # silently staging one file fewer than intended.
      local staged=0
      while IFS= read -r seed; do
        dst="$dir/${seed//\//_}"
        [[ -e "$dst" ]] && echo ">> seed name collision: $seed flattens onto an already-staged name"
        cp "$seed" "$dst" && staged=$((staged + 1))
      done < <(find tests/corpus -name '*.proto')
      # Counted, not inferred from `ls -A`: the two synthetic seeds below are written afterwards,
      # so the directory is never empty for this target and the emptiness check could not fire.
      # A moved tests/corpus dropped this target from 30 seeds to 2 with nothing said.
      if [[ $staged -eq 0 ]]; then
        echo ">> no corpus schemas staged for fuzz_parser (tests/corpus moved?) -- it would run"
        echo "   on the two synthetic depth seeds alone"
        deep_fail=1; return 1
      fi
      # The parser's recursion DepthGuard is its one explicit anti-crash mechanism, and no corpus
      # schema nests more than a few levels -- so mutation never assembles a run deep enough to
      # reach it, and deleting the guard goes unnoticed for as long as anyone cares to fuzz. These
      # two start just under the cap, at the two recursion points, for the mutator to extend.
      printf 'option x = %s' "$(printf '[%.0s' $(seq 1 2000))" >"$dir/zz_deep_list.proto"
      printf '%s' "$(printf 'message M{%.0s' $(seq 1 300))" >"$dir/zz_deep_message.proto"
    else
      # Counted like the parser's, and for the same reason: `ls -A` cannot tell "the fixtures are
      # gone" from "an old payload-seeds directory is still here". Measured with tests/wire_fixtures
      # moved and one stale payload present, all three targets fuzzed on 1 seed instead of 4 and
      # the tier reported clean.
      local staged=0 fixture
      for fixture in tests/wire_fixtures/*.bin; do
        [[ -f "$fixture" ]] && cp "$fixture" "$dir/" && staged=$((staged + 1))
      done
      if [[ $staged -eq 0 ]]; then
        echo ">> no wire fixtures staged for fuzz_$target (tests/wire_fixtures moved?)"
        deep_fail=1; return 1
      fi
      # Payloads the differential kept, if it was run with --write-seeds: thousands of valid
      # messages, reaching decoder arms a handful of fixtures never will. Additive only -- their
      # absence is normal, so they are not counted.
      if [[ -d build/fuzz/payload-seeds ]]; then
        cp build/fuzz/payload-seeds/* "$dir/" 2>/dev/null || true
      fi
    fi
    # Staging nothing is a failure, not a note: the target runs cold, finds little, and the tier
    # reported green anyway -- coverage silently lost, which is the one outcome this file exists to
    # prevent. A moved fixture directory is how it happens.
    if [[ -z "$(ls -A "$dir" 2>/dev/null)" ]]; then
      echo ">> no seeds staged for fuzz_$target (fixtures moved?) -- it would run from an empty corpus"
      deep_fail=1; return 1
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
      stage_fuzz_seeds "$f" || continue   # already reported; launching it only adds noise
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
  cmake --preset gcc -DRAPIDPROTO_BUILD_TESTS=ON >/dev/null 2>&1
  # Same construction as job_corpus: build, do not guess. The deep tier had no freshness handling at
  # all, so it swept 8018 schemas with whatever binary happened to be on disk.
  ensure_gcc_binaries rapidprotoc rapidproto_tests || deep_fail=1
  if [[ -x ./build/gcc/rapidprotoc ]]; then
    corpus_rc=0
    python3 tests/corpus_gate.py --rapidprotoc ./build/gcc/rapidprotoc --jobs "$JOBS" || corpus_rc=$?
    # 77 = not fetched: a skip, not a failure. CI's deep job never fetches the corpus, so treating
    # it as a failure would have turned that job red on every PR.
    [[ $corpus_rc -ne 0 && $corpus_rc -ne 77 ]] && deep_fail=1
    [[ $corpus_rc -eq 77 ]] && deep_skipped+=("corpus sweep")
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
    # The sweep above GENERATES 8018 schemas and compiles none of them (see corpus_gate.py). This
    # leg compiles a bounded sample instead, both models in ONE TU -- the only shape that can catch a
    # collision between them. Deep-tier only: ~13 CPU-minutes (~2 min at -j8), against the
    # sweep's ~160s.
    compile_rc=0
    python3 tests/corpus_compile.py --rapidprotoc ./build/gcc/rapidprotoc --jobs "$JOBS" \
      || compile_rc=$?
    [[ $compile_rc -ne 0 && $compile_rc -ne 77 ]] && deep_fail=1
    [[ $compile_rc -eq 77 ]] && deep_skipped+=("corpus compile")
  else
    echo ">> could not build build/gcc/rapidprotoc for the corpus sweep"; deep_fail=1
  fi

  # The regen scripts are the only thing that produces a golden, and no stage ran them: one that
  # stopped reproducing the tree stayed invisible until somebody regenerated by hand. Comparing the
  # tree afterwards is the whole check -- if the scripts are faithful, nothing changes.
  #
  # Skipped on a tree that already has uncommitted changes under tests/, where a diff afterwards
  # says nothing about the scripts.
  section "goldens reproduce from the regen scripts"
  # `git status --porcelain`, not `git diff`: a regen script that CREATES a file (a generator that
  # starts emitting an additional header) leaves it untracked, which `git diff` does not see.
  # Its EXIT STATUS is checked both times it decides anything: a git that cannot run prints
  # nothing, which reads as "clean" -- so a broken git turned this into a leg that compares
  # nothing and reports the goldens reproduced.
  tests_dirty() {
    local out
    out=$(git status --porcelain -- tests/) || { echo ">> git status failed"; return 2; }
    [[ -n "$out" ]]
  }
  tests_dirty; dirty_rc=$?
  if [[ $dirty_rc -eq 2 ]]; then
    deep_fail=1
  elif [[ $dirty_rc -eq 0 ]]; then
    echo "uncommitted changes under tests/ -- skipped"
    deep_skipped+=("golden regen")
  elif ! tests/regen_goldens.sh >/dev/null 2>&1; then
    echo ">> tests/regen_goldens.sh failed"; deep_fail=1
    # Restore BOTH directions here too: a regen that died halfway may already have created
    # untracked files, and checkout alone leaves them behind -- where they flip every later run
    # of this leg into the "skipped" branch above, permanently.
    git checkout -- tests/ 2>/dev/null || true
    git clean -fdq -- tests/ 2>/dev/null || true
  else
    tests_dirty; dirty_rc=$?
    if [[ $dirty_rc -eq 1 ]]; then
      echo "the checked-in goldens are exactly what the regen scripts produce"
    else
      [[ $dirty_rc -eq 2 ]] || {
        echo ">> the regen scripts no longer reproduce the checked-in goldens:"
        git status --porcelain -- tests/ | head -5
      }
      # Restore BOTH directions -- checkout alone leaves behind any file the regen created.
      git checkout -- tests/ 2>/dev/null || true
      git clean -fdq -- tests/ 2>/dev/null || true
      deep_fail=1
    fi
  fi

  section "deep summary"
  if [[ ${#deep_skipped[@]} -gt 0 ]]; then
    # Named, because "DEEP ALL GREEN" over a silently absent leg is how the corpus half of this
    # tier went unrun in CI: the job says corpus in its name and never fetched one.
    echo "ran but self-skipped (missing tool or data): ${deep_skipped[*]}"
  fi
  if [[ "$deep_fail" == 0 ]]; then echo "DEEP ALL GREEN"; else echo "DEEP FAILURES above"; fi
  exit "$deep_fail"
fi

# Kept, not deleted: after a long run the useful next step is almost always "read the stage that
# failed", and a deleted log means re-running the whole gate to see it again. One fixed path so the
# summary can name it (and so a second run does not accumulate directories).
# Per-run directory, symlinked to a stable name. A single fixed path let a second, overlapping run
# `rm -rf` the first's results: the first then printed the second's logs and reported ALL GREEN with
# its own failure erased. The symlink keeps "read build/gate-logs" true for the last run started.
# Checked: every stage of the default gate writes here (`quick` and `deep` exit above), so an
# unwritable build/ used to surface as a cascade of unrelated failures -- an empty $LOG turning
# every stage's log path into /<name> -- instead of one line naming the cause.
if ! mkdir -p build; then
  echo ">> cannot create build/ -- the gate writes all of its output there" >&2; exit 2
fi
if ! LOG="$(mktemp -d build/gate-logs.XXXXXX)"; then   # mktemp, not $$: PIDs repeat in containers
  echo ">> cannot create a log directory under build/" >&2; exit 2
fi
# Checked for the reason stated above. Only a leftover real DIRECTORY can fail here (a symlink is
# removed without trouble, and build/ is known writable by now) -- and when it does, the name keeps
# pointing at an EARLIER run, so `cat build/gate-logs/<stage>` shows that run's output while this
# one reports ALL GREEN: the exact confusion the per-run directory exists to end.
# $LOG is removed on these paths: they exit above the reaper that caps old log directories, so a
# persistent fault used to leave one orphan build/gate-logs.XXXXXX behind per run.
if ! rm -rf build/gate-logs; then   # a plain `ln -sfn` into an existing directory links INSIDE it
  echo ">> cannot replace build/gate-logs -- it would still point at an earlier run" >&2
  rm -rf "$LOG"; exit 2
fi
if ! ln -sfn "$(basename "$LOG")" build/gate-logs; then
  echo ">> cannot link build/gate-logs -> $LOG" >&2; rm -rf "$LOG"; exit 2
fi
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
# -DRAPIDPROTO_BUILD_TESTS=ON on both, for the reason build/san and build/cov already pin it: a
# cached OFF drops rapidproto_tests and the examples from the build system entirely, so the build
# succeeds without them and the stage runs whatever binary an earlier run left on disk. Measured:
# an injected failing assertion never compiled and the stage reported the same 9347 assertions,
# ALL GREEN.
cmake --preset gcc   -DRAPIDPROTO_BUILD_TESTS=ON >"$LOG/cfg-gcc"   2>&1 & cfg_gcc=$!
cmake --preset clang -DRAPIDPROTO_BUILD_TESTS=ON >"$LOG/cfg-clang" 2>&1 & cfg_clang=$!
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
  python3 tests/check_doc_links.py || return 1
  # Nothing compiles the code in the docs, so a namespace change rots it silently.
  # Grep for FAMILIES of spelling this generator cannot emit, not for the individual strings a past
  # cleanup happened to delete: a blocklist of literals only guards against re-introducing those
  # exact seven, which is not the failure being prevented. CHANGELOG.md is exempt -- its older
  # entries describe the layout as it was, which is the point of a changelog.
  #
  # The file list comes from git, not from walking the filesystem. A recursive scan reads whatever
  # else happens to sit in the tree -- a git worktree under .claude/, a second checkout under a
  # build dir -- so the stage's verdict depended on files that are not part of this commit, and an
  # agent's scratch worktree turned it red.
  local stale=0 hit rc
  local -a doc_files code_files
  mapfile -d '' -t doc_files < <(git ls-files -z -- '*.md' '*.cmake' 'CMakeLists.txt' \
    ':!:CHANGELOG.md' ':!:*golden*')
  # `.proto` carries prose too: a fixture's comment explains WHY the fixture exists, and those
  # justifications rot exactly like docs do.
  mapfile -d '' -t code_files < <(git ls-files -z -- '*.md' '*.cmake' '*.cpp' '*.hpp' '*.proto' \
    ':!:CHANGELOG.md' ':!:*golden*')
  if [[ ${#doc_files[@]} -eq 0 || ${#code_files[@]} -eq 0 ]]; then
    echo ">> no files to scan: git ls-files returned nothing (not a repository?)"
    return 1
  fi

  # grep exits 1 for "no match" and >=2 for an ERROR. Collapsing those makes a scan that cannot run
  # look like a scan that found nothing, which is the failure this gate exists to prevent.
  # One stderr spool for every rot_scan, CHECKED: an unchecked per-call mktemp meant a full or
  # unwritable TMPDIR made the redirect fail with grep's own rc=1 -- which reads as "no match", so
  # a broken /tmp silently disarmed all three scan families. That is the exact silent-pass class
  # this helper exists to close, so a failed mktemp is a red stage, not a quiet green.
  local rot_err
  if ! rot_err=$(mktemp); then
    echo ">> cannot create a temp file for the doc scans -- they would misreport failure as 'no match'"
    return 1
  fi
  rot_scan() {  # <description> <grep args...> -- sets `hit`, returns 0 when something matched
    local what="$1"; shift
    # stderr kept apart from the matches: merged, a grep WARNING on an otherwise-successful scan
    # would be reported as a stale-spelling hit.
    hit=$("$@" 2>"$rot_err"); rc=$?
    if [[ $rc -ge 2 ]]; then
      echo ">> the $what scan could not run (grep exit $rc):"
      sed 's/^/   /' "$rot_err" | head -3
      stale=1
      return 1
    fi
    [[ $rc -eq 0 ]]
  }

  # <pkg>::stream:: -- the pre-roots streaming spelling, for ANY package. `rp::stream::` is the
  # live one; `rapidproto::` is the runtime's own namespace and never a schema package.
  # Prose only. In C++ the same shape is ambiguous -- `pfx::stream::xr` is the LIVE spelling under
  # `--namespace-prefix=pfx`, and `sib::stream::logging` is a real package with a `stream` component
  # -- and stale generated spellings there cannot compile anyway; only comments can rot, and those
  # need a human. Prose has no such excuse.
  # `(ident::)+stream::`, not one component: `com::example::deep::stream::Msg` is the same defect
  # and was invisible. The exclusion is ANCHORED -- an unanchored `rp::stream::` matched any package
  # ending in `rp` (`corp`, `erp`).
  # The match starts at an identifier and grep prints `file:line:match`, so the live spellings are
  # excluded by anchoring on the WHOLE `file:line:` head and the end of the fragment. Testing the
  # character BEFORE the match cannot work here: at column 0 the preceding character is grep's own
  # `:`, so a doc line that STARTS with a live `rp::stream::` was reported as stale. Nor can a bare
  # `:` before the allowed root: that also matches the second colon of any interior `::`, so the
  # STALE `com::example::rp::stream::` was excluded as if it were live. The head is spelled out --
  # path (colon-free), line number, separator -- so only a fragment that IS a live spelling passes.
  if rot_scan "pre-roots streaming" grep -noE '([A-Za-z_][A-Za-z0-9_]*::)+stream::' \
       "${doc_files[@]}"; then
    if hit=$(grep -vE '^([^:]*:)?[0-9]+:(rp|rapidproto)::stream::$' <<<"$hit"); then
      echo ">> a pre-roots streaming spelling (<pkg>::stream::) -- the model root goes BEFORE the package:"
      sed 's/^/   /' <<<"$hit" | head -5
      stale=1
    fi
  fi
  # The PRE-roots prefixed layout, `rp::<pkg>::Msg`. The CHANGELOG explicitly migrates users off it
  # (they had passed --namespace-prefix=rp for protoc coexistence), so it is a live rot target: it
  # looks current, and every root spelling shares its first component. `rp::enums::` is caught here
  # too: it is the pre-rename spelling of the shared root, which is now `common`.
  if rot_scan "pre-roots prefixed" grep -noE '(^|[^:[:alnum:]_])rp::[A-Za-z_][A-Za-z0-9_]*::' \
       "${doc_files[@]}"; then
    if hit=$(grep -vE 'rp::(arena|stream|common)::' <<<"$hit"); then
      echo ">> the pre-roots prefixed layout (rp::<pkg>::) -- a model root goes between them:"
      sed 's/^/   /' <<<"$hit" | head -5
      stale=1
    fi
  fi

  # `rp_dump_write` is deliberately NOT here: it is still the name of the per-message core, and
  # dumpgen emits the call through a `$ns$` placeholder, so its own source contains the string
  # legitimately. `rp_dump_string` was removed outright, and the two only ever appeared as a pair,
  # so prose naming the old API is caught by the half that is unambiguous.
  for pat in 'rp_dump_string' 'rapidproto::dump::'; do
    if rot_scan "removed-spelling ($pat)" grep -nF "$pat" "${code_files[@]}"; then
      echo ">> a spelling this generator no longer emits: '$pat'"
      sed 's/^/   /' <<<"$hit" | head -5
      stale=1
    fi
  done

  # A blocklist cannot see the ARENA half of the rename: the pre-roots arena spelling was a bare
  # `pkg::Msg`, which is also how every unrelated C++ snippet in the docs looks. So assert the live
  # spelling is PRESENT instead -- reverting one of these pages to its pre-roots wording deletes the
  # root, and a positive check notices what no pattern of "what must not appear" can.
  # Each page is listed with the root spellings it must contain: the single-model pages teach one,
  # and README teaches the arena example and links out for streaming rather than spelling it.
  local required doc roots root
  for required in \
      'docs/arena.md:rp::arena::' \
      'docs/streaming.md:rp::stream::' \
      'docs/using-both-models.md:rp::arena:: rp::stream::' \
      'README.md:rp::arena::'; do
    doc="${required%%:*}"; roots="${required#*:}"
    for root in $roots; do
      if ! grep -qF "$root" "$doc"; then
        echo ">> $doc never mentions '$root' -- it teaches a layout this generator does not emit"
        stale=1
      fi
    done
  done

  rm -f "$rot_err"
  [[ $stale -eq 0 ]] || {
    echo ">> update the text, or add an exemption here if the spelling came back"
    return 1
  }
}

job_fixtures() {
  # Syntax-check every tracked shell script first: some are EXECUTED by no gate stage on this
  # platform (tests/system_build_test.sh runs only on the macOS legs), so a parse error would
  # otherwise merge green and fail where it is hardest to debug. Via git ls-files so the
  # obligation follows the rule, not a directory list. bash -n under the gate's bash proves
  # PARSEABILITY only -- the macOS jobs are what prove system_build_test.sh's bash-3.2 claim.
  local _sh _sh_count=0
  while IFS= read -r _sh; do
    if ! bash -n "$_sh" 2>&1; then
      echo ">> $_sh does not parse"
      return 1
    fi
    _sh_count=$((_sh_count + 1))
  done < <(git ls-files '*.sh')
  # Anti-vacuity: process substitution swallows git's own exit status, so an empty or failed
  # listing would pass green having parsed nothing. The tree has 16 tracked scripts today.
  if [[ $_sh_count -lt 10 ]]; then
    echo ">> script sweep found only $_sh_count scripts -- git ls-files failed or the tree moved"
    return 1
  fi

  # Every tests/test_*.cpp must be IN the test binary. A file added to tests/ but never added to
  # CMakeLists.txt is compiled by nothing and run by nothing: the format stage checks it, tidy
  # exits 0 on a TU absent from the compile database, and the suite prints its usual assertion
  # count -- so an entire test file's worth of failures reads as a clean run, with no number
  # moving to give it away. TEST_SRC is the same glob the format and tidy stages use.
  #
  # Matched against the add_executable(rapidproto_tests ...) block with # comments stripped, not
  # against the whole file: a plain substring search over CMakeLists.txt accepts a source that is
  # commented out, named in a comment, or listed in a DIFFERENT target -- and commenting a line
  # out "while debugging" is the most likely way this bug actually happens. Measured: one
  # commented-out entry removed 135 assertions and 18 test cases while the check said all present.
  local block missing=() tu
  block=$(sed -n '/add_executable(rapidproto_tests/,/^[[:space:]]*)[[:space:]]*$/p' CMakeLists.txt \
            | sed 's/#.*//')
  # The range must have TERMINATED on a closing paren. Anchored to `^  )` it silently ran to EOF
  # if CMakeLists were reindented, and the check then degraded back into the whole-file substring
  # search it replaced -- passing a source that had been moved to a different target.
  if [[ -z "$(grep -E '[^[:space:]]' <<<"$block")" ]] \
     || ! grep -qE '^[[:space:]]*\)[[:space:]]*$' <<<"$block"; then
    echo ">> could not read the add_executable(rapidproto_tests ...) source list from CMakeLists.txt"
    echo "   (the block must end in a closing paren on its own line)"
    return 1
  fi
  for tu in "${TEST_SRC[@]}"; do
    grep -qE "(^|[[:space:]])$tu([[:space:]]|$)" <<<"$block" || missing+=("$tu")
  done
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo ">> test sources not in the rapidproto_tests source list -- never compiled, never run:"
    printf '   %s\n' "${missing[@]}"
    return 1
  fi
  echo "test sources: ${#TEST_SRC[@]} files, each listed in add_executable(rapidproto_tests)"

  # Every public header must be in src/header_self_contained.cpp's include list: that TU is what
  # puts the headers under clang-tidy AND under the standalone-compile proof, while the format
  # stage's glob covers formatting only -- so a new header used to be format-checked and then
  # silently escape both. The comparison is glob-vs-includes, so the obligation is a gate, not a
  # memory. (HEADERS is the same globstar-derived list the format stage uses.)
  local hdr listed_hdrs missing_hdrs=()
  listed_hdrs=$(sed -n 's|^#include "\(rapidproto/.*\.hpp\)".*|include/\1|p' \
                  src/header_self_contained.cpp)
  for hdr in "${HEADERS[@]}"; do
    grep -qxF "$hdr" <<<"$listed_hdrs" || missing_hdrs+=("$hdr")
  done
  if [[ -z "$listed_hdrs" ]]; then
    echo ">> src/header_self_contained.cpp lists no rapidproto headers -- the include scan broke"
    return 1
  fi
  if [[ ${#missing_hdrs[@]} -gt 0 ]]; then
    echo ">> public headers missing from src/header_self_contained.cpp -- not tidied, and their"
    echo "   self-containment is unproven:"
    printf '   %s\n' "${missing_hdrs[@]}"
    return 1
  fi
  echo "header self-containment list: covers all ${#HEADERS[@]} public headers"

  # The checked-in wellknown embed must reproduce from its generator: editing a vendored .proto
  # without re-running embed_wellknown.py used to ship a silently stale embed (the name-list test
  # catches a removal, not an edit).
  python3 wellknown/embed_wellknown.py --check || return 1

  tests/check_fixture_coverage.sh
}

job_build_test() {  # $1 = preset; parallel build, then run the test binary
  local preset=$1 build_out rc
  build_out=$(cmake --build --preset "$preset" -j"$JOBS" 2>&1); rc=$?
  if [[ $rc -ne 0 ]] || grep -qE 'error:|warning:' <<<"$build_out"; then
    echo ">> build problems ($preset):"
    grep -E 'error:|warning:' <<<"$build_out" | head -30
    return 1
  fi
  echo "build clean ($preset)"
  # The binaries this stage grades, checked and rebuilt by name. The plain build above cannot see
  # a target that was renamed or dropped from `all`: it succeeds, leaves the previous binary on
  # disk, and the stage runs THAT. Measured on rapidproto_tests: an injected failing assertion
  # lived in a freshly built rapidproto_tests_v2 while the stage reported the stale binary's
  # 9212 assertions as ALL GREEN.
  ensure_targets "build/$preset" rapidproto_tests rapidproto_example_consumer \
    rapidproto_example_lean || return 1
  run_test_binary "./build/$preset/rapidproto_tests" "$preset" || return 1
  # The consumer example (examples/consumer) is built alongside via rapidproto_generate(); run it to
  # confirm the helper-generated decoders (arena + streaming, in one TU) decode at runtime here.
  local example out
  for example in rapidproto_example_consumer rapidproto_example_lean; do
    local path="./build/$preset/examples/consumer/$example"
    # -x on top of ensure_targets: that call proves the target exists and was built, this proves
    # the file it was supposed to produce is actually there and runnable.
    if [[ ! -x "$path" ]]; then
      echo ">> $example was not built ($preset) -- expected $path"; return 1
    fi
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
  ensure_gcc_binaries rapidprotoc || return 1
  command -v "$cf_cxx" >/dev/null 2>&1 || cf_cxx=c++
  if out=$(tests/streamgen_compile_fail.sh "$cf_cxx" 2>&1); then tail -1 <<<"$out"; else echo "$out"; rc=1; fi
  if out=$(tests/arenagen_compile_fail.sh "$cf_cxx" 2>&1); then tail -1 <<<"$out"; else echo "$out"; rc=1; fi
  # RAPIDPROTOC pinned: the script honors it as an override, and an inherited value from the
  # developer's environment would silently grade a DIFFERENT generator than the one
  # ensure_gcc_binaries just proved fresh (the same ambient-env hazard as RAPIDPROTO_REGEN_GOLDEN).
  if out=$(RAPIDPROTOC="$PWD/build/gcc/rapidprotoc" tests/dumpgen_compile_fail.sh "$cf_cxx" 2>&1); then tail -1 <<<"$out"; else echo "$out"; rc=1; fi
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
  # Both writes checked (this file runs without `set -e`): $tu persists across runs, so a failed
  # write left the PREVIOUS run's translation unit in place and the stage compiled that -- passing
  # without ever including the generated headers it exists to check.
  # One printf, not a { ... } group: a failed redirect on a COMPOUND command ({}, (), for, while)
  # aborts it with status 1 and DISCARDS a leading `!`, so `if ! { ... } >"$tu"` sees 1, takes the
  # else branch, and the guard never fires. On a simple command the `!` is applied as usual.
  # (Measured on bash 5.2.21. `{ ... } >"$tu" || return 1` is unaffected -- no `!` involved.)
  if ! printf '%s\n' \
       '#include "arenagen_golden/arena_naming.rp.hpp"   // the C++ keyword fixture lives here' \
       '#include "streamgen_golden/naming.rp.stream.hpp"' \
       'int main() {}' >"$tu"; then
    echo ">> cannot write $tu"; return 1
  fi
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

# The CMake helper declares the CLI's output paths as a custom command's OUTPUT -- asked from the
# CLI when a generator binary and every entry exist at configure, predicted by the fallback
# otherwise --
# and prediction and CLI must agree exactly or the declared file is never produced and the target
# regenerates forever. They are written in different languages and have drifted once already.
job_generate_names() {
  ensure_gcc_binaries rapidprotoc || return 1
  tests/check_generate_names.sh build/gcc/rapidprotoc
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
  # clang-tidy-20 exits 0 with them, so a non-zero status alone does not mean failure.
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
  # Range-checked: the modulo below silently aliases 0/3 onto 3/3 and 5/4 onto 1/4, so a shard
  # outside 1..N lints the wrong slice while every job still reports success.
  # 10# because arithmetic contexts read a leading zero as octal: 08/3 aborts the comparison as an
  # invalid digit, and 010/12 would be accepted as slice 8, leaving slice 10 unlinted.
  if [[ $shard_index =~ ^[0-9]+$ ]]; then shard_index=$((10#$shard_index)); fi
  if [[ $shard_count =~ ^[0-9]+$ ]]; then shard_count=$((10#$shard_count)); fi
  if [[ ! $shard_index =~ ^[0-9]+$ || ! $shard_count =~ ^[0-9]+$ ]] \
     || [[ $shard_count -lt 1 || $shard_index -lt 1 || $shard_index -gt $shard_count ]]; then
    echo ">> RAPIDPROTO_TIDY_SHARD='$shard' is not a valid i/N with 1 <= i <= N"; return 1
  fi
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
  # what a fan-out that never ran looks like. Measured, all as xargs' own status: a bad -P argument
  # exits 1, a child killed by a signal makes it exit 125, and a child that returns non-zero (one
  # that could not write its log) makes it exit 123 -- all of which used to be read as clean.
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

# THE stage table. Validation, aggregation and the summary read from it. Two list PAIRS still
# spell stages out -- DEFAULT_STAGES/NON_DEFAULT_STAGES and PARALLEL_STAGES/SEQUENTIAL_STAGES (the
# run loops and the print loop derive from the latter pair) -- so a new stage needs a key, a
# title, a job, and one entry in each pair. Both pairs are validated as true partitions of a
# duplicate-free STAGE_KEYS before any stage runs, so an omission, a duplicate, or a typo'd key
# in any list is an exit-2, never silence.
readonly STAGE_KEYS=(format docs fixtures gcc clang cf fuzz tidy corpus cxx20 names differential)

# What a bare ./check.sh runs. `corpus` is deliberately absent: sweeping ~8000 third-party schemas is
# a COMPATIBILITY check, not a fast-feedback one -- the library's own behaviour is covered by the
# explicit tests -- and at ~163s it was 30% of the gate. It moved to the deep tier (which gates every
# PR) and keeps its own CI runner, so nothing stopped watching it; it just left the inner loop.
readonly DEFAULT_STAGES=(format docs fixtures gcc clang cf fuzz tidy cxx20 names differential)
# Stages deliberately outside the default gate. Every STAGE_KEYS entry must be in DEFAULT_STAGES or
# here, checked below: a stage left out of BOTH is disabled by omission -- exactly the silent
# forgot-a-list failure the single table was introduced to end.
readonly NON_DEFAULT_STAGES=(corpus)
# HOW the stages run: the first group builds independently (parallelizable), the second consumes
# the gcc stage's binaries and runs serially after it, in this order. Validated below as a true
# partition of STAGE_KEYS (counts + both containments), so a stage cannot fall out of a run loop,
# run twice, or -- the quiet one -- drop out of the PRINT loop, which used to be a fourth hand
# list whose omission silently dropped a stage's captured log.
readonly PARALLEL_STAGES=(format docs fixtures gcc clang fuzz tidy)
readonly SEQUENTIAL_STAGES=(cf corpus cxx20 names differential)
# ONE partition check, applied to both pairs (all three directions: counts, list entries exist in
# STAGE_KEYS, every key in some list). What each direction prevents: a key out of both run lists
# never runs and its log never prints; a run-list entry outside STAGE_KEYS runs, fails in
# stage_job's default arm, and is then invisible to the summary loop (error printed, ALL GREEN
# anyway); a key in both DEFAULT_STAGES and NON_DEFAULT_STAGES makes run_stage's skip message lie.
# The count direction is sound only over a duplicate-free STAGE_KEYS, so that is checked first.
if [[ $(printf '%s\n' "${STAGE_KEYS[@]}" | sort -u | wc -l) -ne ${#STAGE_KEYS[@]} ]]; then
  echo ">> STAGE_KEYS contains a duplicate key" >&2
  exit 2
fi
_check_partition() {
  local -n _half_a=$1 _half_b=$2
  if [[ $(( ${#_half_a[@]} + ${#_half_b[@]} )) -ne ${#STAGE_KEYS[@]} ]]; then
    echo ">> $1 + $2 (${#_half_a[@]}+${#_half_b[@]}) do not partition STAGE_KEYS" >&2
    echo "   (${#STAGE_KEYS[@]}): a stage is missing, duplicated, or listed in both" >&2
    exit 2
  fi
  local _key
  for _key in "${_half_a[@]}" "${_half_b[@]}"; do
    if [[ " ${STAGE_KEYS[*]} " != *" $_key "* ]]; then
      echo ">> stage '$_key' is in $1/$2 but not in STAGE_KEYS" >&2
      exit 2
    fi
  done
  for _key in "${STAGE_KEYS[@]}"; do
    if [[ " ${_half_a[*]} ${_half_b[*]} " != *" $_key "* ]]; then
      echo ">> stage '$_key' is in STAGE_KEYS but in neither $1 nor $2" >&2
      exit 2
    fi
  done
}
_check_partition PARALLEL_STAGES SEQUENTIAL_STAGES
_check_partition DEFAULT_STAGES NON_DEFAULT_STAGES

stage_title() {
  case $1 in
    format)       echo "clang-format (check)" ;;
    docs)         echo "doc links + stale spellings" ;;
    fixtures)     echo "script syntax + corpus fixture coverage" ;;
    gcc)          echo "build + test (gcc)" ;;
    clang)        echo "build + test (clang)" ;;
    cf)           echo "compile-fail (generated decoder rejects misuse)" ;;
    fuzz)         echo "fuzz harness compile-check" ;;
    tidy)         echo "clang-tidy (library = strict, tests = relaxed)" ;;
    corpus)       echo "real-world schema corpus" ;;
    cxx20)        echo "generated headers at c++20/c++23" ;;
    names)        echo "cmake helper predicts the CLI's header paths" ;;
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
    names)        job_generate_names ;;
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
# `+set`, not `:-`: an EMPTY value is set-but-blank, and testing for non-empty skipped the whole
# validation and fell through to the default stage list -- so a CI job computing an empty list ran
# the entire gate instead of the nothing it asked for, or the something it meant to ask for.
_validate_stage_list() {  # $1 = variable name, $2 = its value (already normalized)
  local name=$1 value=$2 want key known
  if [[ -z "$value" ]]; then
    echo ">> $name is set but empty: that would run nothing and report success" >&2
    echo ">> valid stages (space-separated): ${STAGE_KEYS[*]}" >&2
    exit 2
  fi
  for want in $value; do
    known=0
    for key in "${STAGE_KEYS[@]}"; do [[ "$want" == "$key" ]] && known=1; done
    if [[ $known -eq 0 ]]; then
      echo ">> unknown gate stage '$want' in $name" >&2
      echo ">> valid stages (space-separated): ${STAGE_KEYS[*]}" >&2
      exit 2
    fi
  done
}
if [[ -n "${RAPIDPROTO_GATE_STAGES+set}" ]]; then
  RAPIDPROTO_GATE_STAGES="$(tr -s '[:space:]' ' ' <<<"$RAPIDPROTO_GATE_STAGES" | sed 's/^ *//; s/ *$//')"
  _validate_stage_list RAPIDPROTO_GATE_STAGES "$RAPIDPROTO_GATE_STAGES"
fi
# RAPIDPROTO_GATE_SKIP: run the DEFAULT stages minus these. This is how ci.yml's gate job DERIVES
# its list ("default minus tidy, which has its own sharded runners") instead of hand-copying
# DEFAULT_STAGES -- a stage added there used to run locally and never in PR CI, silently. Same
# validation as GATE_STAGES; combining the two is a contradiction, not a merge.
if [[ -n "${RAPIDPROTO_GATE_SKIP+set}" ]]; then
  if [[ -n "${RAPIDPROTO_GATE_STAGES+set}" ]]; then
    echo ">> RAPIDPROTO_GATE_STAGES and RAPIDPROTO_GATE_SKIP are both set: pick one (an explicit" >&2
    echo "   list already says what to run; subtracting from it too is ambiguous)" >&2
    exit 2
  fi
  RAPIDPROTO_GATE_SKIP="$(tr -s '[:space:]' ' ' <<<"$RAPIDPROTO_GATE_SKIP" | sed 's/^ *//; s/ *$//')"
  _validate_stage_list RAPIDPROTO_GATE_SKIP "$RAPIDPROTO_GATE_SKIP"
fi

stage_enabled() {
  [[ " ${RAPIDPROTO_GATE_SKIP:-} " == *" $1 "* ]] && return 1
  [[ " ${RAPIDPROTO_GATE_STAGES:-${DEFAULT_STAGES[*]}} " == *" $1 "* ]]
}

# Records the outcome and duration BESIDE the log, because a concurrent stage runs in a subshell and
# cannot assign to a parent variable -- which is exactly how a hand-written rc_<name> came to be
# forgotten. Reading it back from disk means an unrecorded stage is visibly absent, not silently 0.
run_stage() {  # $1 = stage key
  local key=$1 start rc
  if ! stage_enabled "$key"; then
    if [[ " ${RAPIDPROTO_GATE_SKIP:-} " == *" $key "* ]]; then
      echo "stage skipped (listed in RAPIDPROTO_GATE_SKIP)" >"$LOG/$key"
    elif [[ -n "${RAPIDPROTO_GATE_STAGES:-}" ]]; then
      echo "stage skipped (not in $GATE_SELECTION_DESC)" >"$LOG/$key"
    else
      echo "stage skipped (not in DEFAULT_STAGES -- see NON_DEFAULT_STAGES)" >"$LOG/$key"
    fi
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
  for key in "${PARALLEL_STAGES[@]}"; do
    stage_enabled "$key" && echo "serial gate: $key"   # no progress line for a stage we skip
    run_stage "$key"
  done
else
  stage_pids=()
  for key in "${PARALLEL_STAGES[@]}"; do
    run_stage "$key" & stage_pids+=("$!")
  done
  # Outcomes come from $LOG/<key>.rc, not from wait: each stage records its result where every
  # consumer reads it, so there is no second place to keep in sync.
  for pid in "${stage_pids[@]}"; do wait "$pid" || true; done
fi

# SEQUENTIAL_STAGES run after the build stages, never alongside them: they consume build/gcc's
# binaries (`cf` generates fresh headers with rapidprotoc -- alongside the gcc stage it only
# checked that SOME binary was executable, and it cannot build the binary itself because a second
# `cmake --build` in the same directory races the one already running; corpus/names/differential
# likewise drive build/gcc's tools; cxx20 needs only the on-disk goldens but is instant).
for key in "${SEQUENTIAL_STAGES[@]}"; do
  [[ "$serial_gate" == 1 ]] && stage_enabled "$key" && echo "serial gate: $key"
  run_stage "$key"
done

# --- print each stage's output in a fixed order (already captured, so never interleaved) ----------
# The partition IS the print order (sequential first: their logs are short and often the point),
# so a stage cannot run while its captured log silently never prints.

for key in "${SEQUENTIAL_STAGES[@]}" "${PARALLEL_STAGES[@]}"; do
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
