#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Run every schema in the fetched corpus through rapidprotoc, and diff the result against
the checked-in expected-failure list.

Why this is the load-bearing test: RapidProto parses `.proto` itself instead of consuming a
protoc `FileDescriptorSet`, so nothing else checks the front-end against schemas protoc
actually accepts. `tests/corpus/` pins semantics we chose; this checks thousands of schemas
other people wrote, with option shapes and import graphs we never thought of.

One `rapidprotoc` invocation per schema covers parse -> resolve -> analyze -> generate for all
three emitters (arena, streaming, dump), so a failure anywhere in the pipeline surfaces here.
**Compilation of the generated code is NOT
covered** -- one googleapis schema generates 2223 message decoders and costs minutes to compile,
so a compile leg would need a schema subset small enough to stay affordable. That is not this
gate's job.

A failure must match its recorded reason, not merely its path: a listed schema that starts
failing for a DIFFERENT reason is a regression, and matching on the path alone would wave it
through. The list is also bidirectional -- a listed schema that starts passing fails the gate
too, so fixing a limitation forces its line out in the same change.

Usage:
    python3 tests/corpus_gate.py                    # sweep; skips only if nothing is fetched
    python3 tests/corpus_gate.py --jobs 8
    python3 tests/corpus_gate.py --list-failures    # print every failure, do not diff

Exits 0 when NO source has been fetched: downloading ~100 MB must never be a precondition for
running the gate locally. A partially fetched corpus is an ERROR, not a skip -- sweeping a
fraction of the schemas while reporting the same green result is the failure mode worth being
loudest about.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_corpus  # noqa: E402  (needs the path insert above)

REPO = Path(__file__).resolve().parent.parent
# See differential.py: 77 means "expected not-run", which check.sh reports as a self-skip.
SKIP_RC = 77

DEFAULT_EXPECTED = Path(__file__).resolve().parent / "corpus_expected_failures.txt"

# A single schema should take milliseconds. This is a deadlock/runaway guard, not a budget:
# an unbounded wait on a cyclic import graph or a runaway recursion -- exactly the bug class
# this corpus exists to find -- would burn the CI job's whole time limit instead of failing
# in seconds with the guilty path named.
PER_SCHEMA_TIMEOUT_S = 120


def load_expected(path: Path) -> dict[str, str]:
    """Map corpus-relative schema path -> the substring its error message must contain."""
    expected: dict[str, str] = {}
    for number, raw in enumerate(path.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "\t" not in line:
            raise SystemExit(
                f"{path.name}:{number}: expected `path<TAB>reason`, found no tab:\n  {line}"
            )
        schema, _, reason = line.partition("\t")
        schema, reason = schema.strip(), reason.strip()
        if not reason:
            raise SystemExit(f"{path.name}:{number}: empty reason for {schema}")
        if schema in expected:
            raise SystemExit(f"{path.name}:{number}: duplicate entry for {schema}")
        expected[schema] = reason
    return expected


def fetched_sources(corpus: Path) -> tuple[list[fetch_corpus.Source], list[str]]:
    """Split the declared sources into (present-and-at-their-pin, not-usable).

    Reuses fetch_corpus's own staleness check -- stamp AND `git rev-parse HEAD` against the
    pinned commit -- rather than trusting that a directory exists. Deriving the source list
    from fetch_corpus.SOURCES (instead of a local copy) means a source added to the fetcher
    cannot be silently left out of the sweep.
    """
    present, unusable = [], []
    for source in fetch_corpus.SOURCES:
        if fetch_corpus.is_current(corpus, source):
            present.append(source)
        else:
            unusable.append(source.name)
    return present, unusable


def schemas(corpus: Path, sources: list[fetch_corpus.Source]) -> list[tuple[str, Path, Path]]:
    """(corpus-relative key, -I root, file) for every .proto in every usable source."""
    found = []
    for source in sources:
        base = corpus / source.name
        root = (base / source.include_root).resolve()
        for path in sorted(base.rglob("*.proto")):
            found.append((str(path.relative_to(corpus)), root, path))
    return found


def check(tool: Path, root: Path, path: Path, timeout: float) -> str | None:
    """None when rapidprotoc accepts the schema, else its first error line.

    A fresh out-dir per schema: generated headers collide across schemas (a shared
    <stem>.rp.common.hpp), and a stale one could mask a generation failure.
    """
    with tempfile.TemporaryDirectory(prefix="rpgate-") as out:
        try:
            proc = subprocess.run(
                # All three emitters: --dump costs ~3% and is otherwise the only shipped
                # emitter no real-world schema ever reaches.
                [str(tool), "--arena", "--stream", "--dump",
                 f"-I{root}", "--out-dir", out, str(path)],
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return f"TIMEOUT after {timeout}s (hang or runaway recursion)"
    if proc.returncode == 0:
        return None
    lines = (proc.stderr or proc.stdout).strip().splitlines()
    # Skip non-fatal `warning:` lines: they precede the real diagnostic, and taking one as the
    # failure reason would diff against corpus_expected_failures.txt as a spurious reason change.
    message = [ln for ln in lines if not ln.lstrip().startswith("warning:")]
    return message[0] if message else f"exit {proc.returncode} with no diagnostic"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--corpus",
        type=Path,
        default=None,
        help="corpus directory (default: the one tests/fetch_corpus.py writes)",
    )
    parser.add_argument(
        "--rapidprotoc",
        type=Path,
        default=REPO / "build" / "gcc" / "rapidprotoc",
        help="the rapidprotoc to sweep with (default: build/gcc/rapidprotoc)",
    )
    parser.add_argument(
        "--expected",
        type=Path,
        default=DEFAULT_EXPECTED,
        help=f"expected-failure list (default: {DEFAULT_EXPECTED.name})",
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel sweeps")
    parser.add_argument(
        "--timeout",
        type=float,
        default=PER_SCHEMA_TIMEOUT_S,
        help=f"per-schema runaway guard in seconds (default: {PER_SCHEMA_TIMEOUT_S})",
    )
    parser.add_argument(
        "--list-failures", action="store_true", help="print every failure; skip the diff"
    )
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")

    # An explicit --corpus that does not exist is a typo, not a reason to report success.
    explicit = args.corpus is not None
    corpus = args.corpus or fetch_corpus.DEFAULT_DEST
    if explicit and not corpus.is_dir():
        raise SystemExit(f"--corpus {corpus} does not exist")

    present, unusable = (fetched_sources(corpus) if corpus.is_dir() else ([], []))
    if not present:
        print(f"corpus not fetched ({corpus}); skipping -- run tests/fetch_corpus.py")
        return SKIP_RC
    if unusable:
        raise SystemExit(
            f"corpus is INCOMPLETE: {', '.join(unusable)} missing or not at the pinned commit.\n"
            f"Sweeping a fraction of the schemas would report the same green result as a full "
            f"run. Re-run tests/fetch_corpus.py."
        )

    tool = args.rapidprotoc
    if not tool.is_file() or not os.access(tool, os.X_OK):
        raise SystemExit(f"{tool} is not an executable rapidprotoc; build it first")

    work = schemas(corpus, present)
    if not work:
        raise SystemExit(f"{corpus} passed its stamp checks but holds no .proto files")

    failures: dict[str, str] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(check, tool, root, path, args.timeout): key for key, root, path in work}
        for future in concurrent.futures.as_completed(futures):
            error = future.result()
            if error is not None:
                failures[futures[future]] = error

    print(f"corpus gate: {len(work)} schemas, {len(failures)} failed ({tool})")

    if args.list_failures:
        for key in sorted(failures):
            print(f"  {key}\n      {failures[key]}")
        return 0

    expected = load_expected(args.expected)
    unexpected = sorted(set(failures) - set(expected))
    fixed = sorted(set(expected) - set(failures))
    changed = sorted(k for k in set(failures) & set(expected) if expected[k] not in failures[k])

    for key in unexpected:
        print(f">> UNEXPECTED FAILURE: {key}\n      {failures[key]}")
    for key in fixed:
        print(
            f">> NO LONGER FAILING: {key}\n"
            f"      was: {expected[key]}\n"
            f"      remove it from {args.expected.name} -- the list must not outlive the limitation"
        )
    for key in changed:
        print(
            f">> FAILS FOR A DIFFERENT REASON: {key}\n"
            f"      expected: {expected[key]}\n"
            f"      actual  : {failures[key]}"
        )
    if unexpected or fixed or changed:
        return 1

    print(f"corpus gate: matches {args.expected.name} ({len(expected)} known-failing schemas)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
