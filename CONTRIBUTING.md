# Contributing to RapidProto

Thanks for your interest. RapidProto is a **decode-only** Protobuf decoder + code generator for
C++17. Before changing anything, skim [architecture.md](architecture.md); Part I is a ~10-minute
tour of the pipeline, the two decode models (arena + streaming), and the invariants a change must
not break.

## Toolchain

The quality gate pins specific versions, so install:

- **gcc-13** and the **clang-20** suite (`clang-20`, `clang-format-20`, `clang-tidy-20`,
  `llvm-cov-20`). Both compilers are required; the gate builds and tests on each.
- **CMake ≥ 3.21** (for the presets) and a build tool (Ninja or Make).
- For the benchmarks: `libprotobuf-dev` + `protozero`. Drive them with `tests/bench.py` (`run` to
  snapshot both decoders, `table` to render, `diff`/`experiment` to compare); read the
  [Decoder performance](architecture.md#decoder-performance) section first for how to read the numbers
  (placement noise, cyc/B vs the placement-invariant ins/B, pinning to one core). The current headline
  numbers live in [docs/benchmarks.md](docs/benchmarks.md) — update it first when they change, and
  keep the README's headline claims in step.
- `tests/compile_bench.py` measures the other half of what a code generator costs its user: how long
  the generated decoders take to **compile**, how big the resulting `.text` is, and the compiler's
  peak RSS. Same `run` / `table` / `diff` shape. It is a tool, not a gate — a single arena TU can take
  a minute on gcc, which is the point rather than a flaw.

## Building

```sh
cmake --preset gcc        # or: clang
cmake --build --preset gcc
./build/gcc/rapidproto_tests
```

## The quality gate

`./check.sh` is the one-stop bar and **must be green before you commit** (on macOS, where the
full gate cannot run — it needs bash >= 4.4 and the pinned gcc-13/clang-20 —
`tests/system_build_test.sh` is the local bar, and CI's Linux jobs cover the rest on your PR):

- `./check.sh`: clang-format, dual-compiler build + test, clang-tidy (strict on the library), the
  compile-fail harnesses, a docs link check, a dispatch-gate stress compile, and the randomized
  differential against protobuf.
- `./check.sh fix`: apply clang-format first, then run the full gate.
- `./check.sh quick`: gcc-only build + test for the inner loop (not the commit bar).
- `./check.sh compilers`: only the architecture-sensitive stages (build + test on both compilers,
  compile-fail, fuzz-compile) — what CI's arm64 job and the release workflow's arm64 leg run; the
  stage list lives in `check.sh`.
- `./check.sh deep` is the heavy tier: ASan + UBSan, a library coverage floor, the real-world corpus
  sweep plus a bounded corpus-compile sample, a check that the regen scripts reproduce the
  checked-in goldens, and a fuzz smoke over the four targets (see [Fuzzing](#fuzzing)).

CI runs the gate's stages spread across several jobs, `./check.sh deep`, and a Release `-O3 -Werror`
build on **every pull request and every push to the default branch** (feature-branch pushes are
gated by their PR run). Running `./check.sh` locally covers the same stages in one command; what
CI adds beyond it: the corpus-compile sample (locally a deep-tier leg), an arm64 build/test job,
a macOS build/test job (AppleClang + libc++, via `tests/system_build_test.sh` — the system-compiler
sequence shared with the release workflow's macOS leg), and the consumer job (install ->
`find_package` -> C++20/23 header compiles).

## The real-world schema corpus

RapidProto parses `.proto` itself instead of consuming a protoc `FileDescriptorSet`, so nothing in
the build otherwise checks the front-end against schemas *protoc* accepts. `tests/fetch_corpus.py`
closes that gap by fetching third-party schemas at pinned refs:

```sh
python3 tests/fetch_corpus.py            # ~100 MB into build/corpus (gitignored)
python3 tests/fetch_corpus.py --list     # what it fetches, and why, without touching the network
```

Nothing is vendored and nothing is redistributed, and everything corpus-related **skips** when
*nothing* has been fetched, so you never have to download it to run the gate. A **partially** fetched
corpus is a hard failure instead: sweeping a fraction of the schemas while reporting the same green
result is worse than not sweeping at all. Once fetched, `./check.sh deep` sweeps every schema
through `rapidprotoc` (tens of CPU-minutes; wall time scales with cores). It is out of the default gate
deliberately -- it is a compatibility check, not fast feedback -- and CI runs it in its own job on
every PR.

Failures are diffed against `tests/corpus_expected_failures.txt`, and that list is strict in three
directions: an unlisted schema that fails is a regression, a listed one that starts **passing** fails
too (so fixing a limitation deletes its lines in the same change), and a listed one that fails for a
**different reason** than recorded fails as well. Keep it small — every line is a documented gap
between RapidProto and protoc.

The deep sweep (`rapidproto_tests [sweep]`, under a minute) lex+parses every schema on its own. It is excluded
from the gate because `rapidprotoc` already parses all of them; run it directly when you want
per-file front-end diagnostics rather than one error per pipeline run.

Pins carry both the ref and the commit it resolved to, and a mismatch fails loudly — bump a pin **in
its own commit**, never as a side effect of other work, so a corpus change can't be a hidden variable
behind an unrelated failure.

## The differential against protobuf

`tests/differential.py` is the correctness oracle the goldens cannot be: it builds random messages
with protobuf's own reflection, serializes them, decodes the same bytes with the generated arena
decoder, and compares **every field**. Goldens pin what the generator emits; this pins what the
generated code *means*, against an independent implementation.

```bash
python3 tests/differential.py                      # every tests/corpus schema (a check.sh stage)
python3 tests/differential.py --messages 250 --seed 3 --verbose
python3 tests/differential.py --schema tests/corpus/proto3.proto
```

No comparator is written per schema: `rapidproto_diffgen` emits the C++ harness, taking the exact
generated type names from the shared `CppNameTable` rather than re-deriving them (keyword escapes
and collision suffixes make that unpredictable). The comparison surface is the debug dumper, since
RapidProto has no reflection and nothing else turns a decoded tree into data a script can walk.

It needs `protoc` and the protobuf Python bindings, and skips cleanly without them — they are
dev-only dependencies, like protozero and Catch2. Two things it cannot cover by construction:
extensions, which are never materialized, and unknown fields, since payloads come from the same
schema that decodes them. A schema it cannot drive is skipped rather than failed, with the reason
printed under `--verbose`: protoc rejecting it (an intentional-collision fixture, a custom option it
cannot resolve, editions on a protoc too old for them), or `package main`, whose `namespace main`
cannot coexist with the harness's `int main()`. A harness that fails to *compile* is a failure, not
a skip.

## Fuzzing

Four libFuzzer targets in `tests/fuzz/`, run by `./check.sh deep` (`FUZZ_TIME` seconds each, default
30) under ASan + UBSan. Three drive decoding — `fuzz_wire` (the wire reader), `fuzz_arena` and
`fuzz_stream` (generated decoders) — over arbitrary bytes. `fuzz_parser` drives the schema front-end:
lexer, parser, and the semantic passes. That one is a **robustness** bar rather than a trust
boundary — a schema is trusted input, per [SECURITY.md](SECURITY.md) — but a malformed one must be a
clean diagnostic, never a crash. It is filesystem-free, so import resolution (I/O, not parsing)
stays out.

Each target keeps a **persistent corpus** under `build/fuzz/corpus/<target>` (gitignored), so a run
on that machine starts from what earlier runs found rather than rediscovering the same coverage —
locally it compounds; CI checks out fresh each time and always starts from the seeds. Seeds are
staged into `build/fuzz/seeds/<target>`: the corpus schemas for `fuzz_parser`, and
`tests/wire_fixtures/*.bin` for the decode targets. To seed those far more heavily, keep the
differential's payloads first — thousands of valid messages over real schemas:

```sh
python3 tests/differential.py --write-seeds build/fuzz/payload-seeds
FUZZ_TIME=300 ./check.sh deep
```

Two seeds are synthesized rather than taken from the corpus: deeply-nested `option`/`message` runs
that put the mutator within reach of the parser's recursion cap. Nothing in `tests/corpus` nests more
than a few levels, so without them the cap — the parser's one explicit anti-crash mechanism — has no
regression cover at all: deleting it goes unnoticed through minutes of fuzzing, and is caught in
seconds with them.

A crash writes its input to `build/fuzz/` and fails the tier; reproduce with
`./build/fuzz/fuzz_<target> <that-file>`. A newly found crash never enters the corpus (libFuzzer
stores a unit only after it executes cleanly), but a change that makes an *already stored* unit
crash makes every later run fail on it — which is the point, and `rm -rf build/fuzz/corpus/<target>`
is the way out once the bug is fixed. The corpus is never pruned, so if replaying it starts eating
the time budget, delete it and let it rebuild.

## Goldens

Much of the suite is golden tests (the analyzed AST, the wire structure, each emitter's output, the
arena layout plan — all dumped to text and compared byte-for-byte). After an **intentional** change
to a generator or a dumper, regenerate with `tests/regen_goldens.sh`, then run `./check.sh` and
review the diff by hand. Never hand-edit a file under `tests/*_golden/`.

Adding a schema to `tests/corpus/nsedge/` needs one extra step: the regen scripts only *overwrite*
goldens that already exist, so seed each one once by running `rapidprotoc` directly, and add the
fixture to `tests/regen_goldens.sh`, `tests/regen_arenagen_goldens.sh`, and the case list plus
`#include` in the matching test file. `tests/check_fixture_coverage.sh` (a gate stage) fails until
the fixture is referenced by both regen scripts and both goldens exist — without it a new fixture
is silently unpinned, since a package/namespace shape can fail in ways no compiler reports.

## Style & scope

- All hand-written code is `clang-format`ed; the gate enforces it, and nothing is exempt. Comments
  explain **why**, not what.
- RapidProto **never crashes on any input**: serialized bytes are untrusted (see
  [SECURITY.md](SECURITY.md)). Preserve that invariant; the fuzzers and sanitizers guard it.
- The scope is deliberately narrow: decode-only, no serialization, no JSON. Read "Known limitations
  and non-goals" in architecture.md before proposing a feature.
- Keep commits small and focused, with a short, descriptive message.
- A change that breaks the generated API or the CLI contract bumps `project(VERSION)` (the minor,
  pre-1.0) **in the same PR**, with a CHANGELOG.md entry — `find_package` consumers pin against
  that version, so it must never lag the surface it describes.

## Pull requests

Open a PR against the default branch with `./check.sh` green. Describe what changed and why; if you
touched a generator, include the regenerated goldens in the same PR.
