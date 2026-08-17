#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Compile a bounded sample of corpus-generated headers.

Why this exists: `tests/corpus_gate.py` runs the generator over all 8018 schemas but never
compiles a line of what it emits -- its own docstring says so, because one googleapis schema
generates 2223 decoders and costs minutes. So the sweep cannot see a header that generates
happily and does not compile: a name that collides only in C++, a nesting mirror that lands an
alias somewhere unreachable, a cross-file reference to a scope that was never opened.

Two schemas ARE compiled elsewhere (`compute.proto` by the arena bench's large-schema arm, and
`descriptor.proto` + `compute.proto` by tests/compile_bench.py), but both arena-only and both one
schema. This leg buys BREADTH instead: a sample, with arena and streaming in ONE translation unit,
which is the only shape that can catch a collision BETWEEN the two models.

Deep-tier only -- see check.sh. Exit 77 when the corpus is not fetched, matching corpus_gate.py.

    python3 tests/corpus_compile.py --rapidprotoc build/gcc/rapidprotoc [--sample 200] [--jobs 8]
"""
import argparse
import concurrent.futures
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
CORPUS = REPO / "build" / "corpus"
NOT_FETCHED = 77


def schemas(sample: int) -> list[pathlib.Path]:
    """An evenly-spaced sample over the sorted corpus, so a run covers every source and package
    shape rather than clustering in whichever tree sorts first. Deterministic: no RNG, so a
    failure reproduces from the same --sample."""
    every = sorted(CORPUS.rglob("*.proto"))
    if sample <= 0 or sample >= len(every):
        return every
    step = len(every) / sample
    return [every[int(i * step)] for i in range(sample)]


def compile_one(binary: pathlib.Path, cxx: str, proto: pathlib.Path) -> tuple[pathlib.Path, str]:
    """Generate both models for `proto` and compile them together. Returns (proto, "") on success."""
    rel = proto.relative_to(CORPUS)
    root = CORPUS / rel.parts[0]  # each corpus source is its own import root
    with tempfile.TemporaryDirectory(prefix="rp_cc_") as tmp:
        out = pathlib.Path(tmp)
        gen = subprocess.run(
            [str(binary), "--arena", "--stream", "-I", str(root), "--out-dir", str(out), str(proto)],
            capture_output=True, text=True, check=False)
        if gen.returncode != 0:
            # A schema the generator rejects is corpus_gate.py's business, not ours.
            return proto, ""
        # The header path is relative to the IMPORT ROOT the CLI was given, not the corpus root.
        stem = proto.relative_to(root).with_suffix("")
        tu = out / "rp_cc_tu.cpp"
        tu.write_text('#include "%s.rp.hpp"\n#include "%s.rp.stream.hpp"\nint main() {}\n'
                      % (stem.as_posix(), stem.as_posix()))
        cc = subprocess.run([cxx, "-std=c++17", "-fsyntax-only", "-I", str(out), str(tu)],
                            capture_output=True, text=True, check=False)
        return proto, "" if cc.returncode == 0 else cc.stderr[:1500]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rapidprotoc", default="build/gcc/rapidprotoc")
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--sample", type=int, default=200, help="schemas to compile (0 = all)")
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()

    if not CORPUS.is_dir():
        print("corpus compile: not fetched (run tests/fetch_corpus.py) -- skipped")
        return NOT_FETCHED
    binary = pathlib.Path(args.rapidprotoc).resolve()
    if not binary.is_file():
        print(f">> {binary} not found", file=sys.stderr)
        return 1

    picked = schemas(args.sample)
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for proto, err in pool.map(lambda p: compile_one(binary, args.cxx, p), picked):
            if err:
                failures.append((proto, err))

    for proto, err in failures:
        print(f">> {proto.relative_to(CORPUS)} generates but does not compile:", file=sys.stderr)
        print("\n".join(err.splitlines()[:6]), file=sys.stderr)
    print(f"corpus compile: {len(picked)} schemas, arena+stream in one TU, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
