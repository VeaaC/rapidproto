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
    """A sample spread evenly WITHIN EACH corpus source, so a run covers every source's package and
    feature shapes. Sampling the flat sorted list does not: googleapis is 7993 of 8018 files, so an
    evenly-spaced global sample never reaches protobuf's own test schemas -- descriptor.proto, the
    editions files, the groups/extensions shapes -- which sort last. Deterministic (no RNG), so a
    failure reproduces from the same --sample."""
    by_source: dict[str, list[pathlib.Path]] = {}
    for proto in sorted(CORPUS.rglob("*.proto")):
        by_source.setdefault(proto.relative_to(CORPUS).parts[0], []).append(proto)
    if sample <= 0:
        return [p for group in by_source.values() for p in group]
    # Proportional to each source's size, with a floor so a small source is never sampled away:
    # protobuf's own test schemas are 25 files against googleapis' 7993, and they carry the shapes
    # googleapis does not (groups, extensions, editions, deep recursion).
    total = sum(len(g) for g in by_source.values())
    floor = min(20, sample // max(1, len(by_source)))
    picked: list[pathlib.Path] = []
    for group in by_source.values():
        want = max(floor, round(sample * len(group) / total))
        if want >= len(group):
            picked.extend(group)
            continue
        step = len(group) / want
        picked.extend(group[int(i * step)] for i in range(want))
    return picked


def compile_one(binary: pathlib.Path, cxx: str,
                proto: pathlib.Path) -> tuple[pathlib.Path, str, bool]:
    """Generate every model for `proto` and compile them together.

    Returns (proto, error, compiled). `compiled` is False when the generator rejected the schema, so
    the caller can tell "nothing to compile" from "compiled clean" -- reporting a count of SAMPLED
    schemas would let this leg print green having invoked the compiler zero times."""
    rel = proto.relative_to(CORPUS)
    root = CORPUS / rel.parts[0]  # each corpus source is its own import root
    with tempfile.TemporaryDirectory(prefix="rp_cc_") as tmp:
        out = pathlib.Path(tmp)
        gen = subprocess.run(
            [str(binary), "--arena", "--stream", "--dump", "-I", str(root), "--out-dir", str(out),
             str(proto)],
            capture_output=True, text=True, check=False)
        if gen.returncode != 0:
            # A schema the generator rejects is corpus_gate.py's business, not ours.
            return proto, "", False
        # The header path is relative to the IMPORT ROOT the CLI was given, not the corpus root.
        stem = proto.relative_to(root).with_suffix("")
        tu = out / "rp_cc_tu.cpp"
        # All three headers, because the dump emitter puts every schema's hooks in ONE shared
        # namespace (rapidproto::dump_detail) -- a cross-schema clash there is invisible to a TU
        # that only ever holds one schema's arena and stream headers.
        tu.write_text('#include "{0}.rp.hpp"\n#include "{0}.rp.stream.hpp"\n'
                      '#include "{0}.rp.dump.hpp"\nint main() {{}}\n'.format(stem.as_posix()))
        cc = subprocess.run([cxx, "-std=c++17", "-Wall", "-fsyntax-only", "-I", str(out), str(tu)],
                            capture_output=True, text=True, check=False)
        if cc.returncode == 0:
            return proto, "", True
        # The temp dir dies with this scope, so rewrite its paths to something a reader can open.
        return proto, cc.stderr[:1500].replace(str(out) + "/", "<generated>/"), True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rapidprotoc", default="build/gcc/rapidprotoc")
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--sample", type=int, default=150, help="schemas to compile (0 = all)")
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
    compiled = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for proto, err, ok in pool.map(lambda p: compile_one(binary, args.cxx, p), picked):
            compiled += int(ok)
            if err:
                failures.append((proto, err))

    for proto, err in failures:
        print(f">> {proto.relative_to(CORPUS)} generates but does not compile:", file=sys.stderr)
        print("\n".join(err.splitlines()[:6]), file=sys.stderr)
    sources = len({p.relative_to(CORPUS).parts[0] for p in picked})
    print(f"corpus compile: {compiled}/{len(picked)} schemas compiled across {sources} sources, "
          f"all models in one TU, {len(failures)} failed")
    # Absence of failures is not a pass. A generator that rejects everything, or a sampler that
    # picks nothing, previously reported green here having never invoked the compiler. The floor is
    # "did we compile at all", not a rejection rate: the corpus legitimately holds schemas this
    # generator declines (corpus_gate.py owns those), and a ratio would go flaky as it is refetched.
    if compiled == 0:
        print(f">> 0 of {len(picked)} sampled schemas reached the compiler: this leg checked "
              f"nothing", file=sys.stderr)
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
