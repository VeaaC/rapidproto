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
  (placement noise, cyc/B vs the placement-invariant ins/B, pinning to one core).

## Building

```sh
cmake --preset gcc        # or: clang
cmake --build --preset gcc
./build/gcc/rapidproto_tests
```

## The quality gate

`./check.sh` is the one-stop bar and **must be green before you commit**:

- `./check.sh`: clang-format, dual-compiler build + test, clang-tidy (strict on the library), the
  compile-fail harnesses, and a dispatch-gate stress compile.
- `./check.sh fix`: apply clang-format first, then run the full gate.
- `./check.sh quick`: gcc-only build + test for the inner loop (not the commit bar).
- `./check.sh deep` is the heavy tier: ASan + UBSan, a library coverage floor, and a fuzz smoke.

CI runs `./check.sh`, `./check.sh deep`, and a Release `-O3 -Werror` build on **every push and pull
request**.

## Goldens

Much of the suite is golden tests (the analyzed AST, the wire structure, each emitter's output, the
arena layout plan — all dumped to text and compared byte-for-byte). After an **intentional** change
to a generator or a dumper, regenerate with `tests/regen_goldens.sh`, then run `./check.sh` and
review the diff by hand. Never hand-edit a file under `tests/*_golden/`.

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
