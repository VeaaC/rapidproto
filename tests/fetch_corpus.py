#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Fetch the real-world schema corpus RapidProto's integration tests run against.

Why this exists: RapidProto parses `.proto` itself rather than consuming a protoc
FileDescriptorSet, so nothing in the build checks the front-end against schemas protoc
actually accepts. The synthetic `tests/corpus/` pins semantics we chose; this corpus is
the compatibility check -- real schemas, written by other people, with option shapes and
import graphs we did not think of.

Nothing here is vendored. Each source is fetched at a PINNED ref into `build/corpus/`
(gitignored) and is a development-only dependency, like Catch2 and protozero: RapidProto
redistributes none of it. Every fetch is a blob-filtered, depth-1, sparse checkout of just
the paths we need, so the whole corpus is ~100 MB rather than several GB.

Usage:
    python3 tests/fetch_corpus.py               # fetch everything missing or stale
    python3 tests/fetch_corpus.py --list        # show the sources, no network
    python3 tests/fetch_corpus.py --only protobuf --only googleapis
    python3 tests/fetch_corpus.py --force       # refetch even if the stamp matches
    python3 tests/fetch_corpus.py --dest DIR    # default: build/corpus

A source is re-fetched only when its stamp file disagrees with the pinned ref, so re-running
this is cheap and offline-safe once populated.

Pin policy
----------
Every source is pinned to an exact ref AND the SHA that ref resolved to when it was pinned.
If a fetch resolves to a different SHA -- upstream moved a tag, or a branch advanced -- the
fetch FAILS loudly rather than quietly changing what the tests run against.

Pins are bumped **deliberately, in their own commit**, never as a side effect of other work:
a corpus change must never be a hidden variable when an unrelated gate starts failing. When
bumping, run the gate before and after so any new parse failure is attributable to the bump.
`googleapis` is pinned to a SHA rather than a tag because upstream publishes no releases.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO / "build" / "corpus"


@dataclass(frozen=True)
class Source:
    """One pinned upstream checkout.

    `ref` is what we ask the server for (a tag, or a SHA when upstream has no tag worth
    pinning). `sha` is that ref resolved at the time it was pinned -- recorded so a moved
    tag is visible as a mismatch rather than silently changing the corpus.

    `patterns` are git sparse-checkout patterns. NON-cone mode is required, not preferred:
    cone mode matches whole DIRECTORIES only, and we need file globs (`*.proto`, and single
    named files) to avoid dragging in protobuf's entire C++ source tree. Git documents
    non-cone as deprecated, so if a future git drops it these patterns must be reworked --
    the fallback is a cone checkout of the parent directories plus a prune pass.

    `include_root` is the path (relative to the checkout) to put on rapidprotoc's `-I`
    line, i.e. the directory an `import "..."` in these schemas resolves against.
    """

    name: str
    repo: str
    ref: str
    sha: str
    patterns: list[str]
    include_root: str
    license: str
    why: str
    probe: str  # must exist after a good fetch; its absence means the patterns went stale


SOURCES: list[Source] = [
    Source(
        name="protobuf",
        repo="https://github.com/protocolbuffers/protobuf",
        ref="v35.1",
        sha="35cd01f9fe9afbeea38cc7b979a3b6bfcde82c03",
        patterns=[
            # The conformance suite's message schemas: the canonical "every feature the
            # format has" set, one per syntax level. test_messages_edition2023.proto is the
            # only real-world EDITIONS schema published anywhere -- RapidProto's editions
            # support is otherwise tested solely against its own synthetic corpus. (There is
            # no editions-2024 equivalent upstream at any tag; that stays synthetic-only.)
            "/conformance/conformance.proto",
            "/conformance/test_protos/*.proto",
            "/src/google/protobuf/test_messages_proto2.proto",
            "/src/google/protobuf/test_messages_proto3.proto",
            # The two schemas real tooling consumes.
            "/src/google/protobuf/descriptor.proto",
            "/src/google/protobuf/compiler/plugin.proto",
        ],
        # Import paths resolve against src/ (`google/protobuf/descriptor.proto`). The
        # conformance schemas are entries with no imports of their own (checked), and
        # test_messages_proto3.proto's seven WKT imports resolve from the EMBEDDED
        # well-known types rather than disk -- the resolver searches include paths first
        # and falls back to them -- so upstream's WKT copies are deliberately not fetched.
        include_root="src",
        license="BSD-3-Clause",
        why=(
            "The conformance message schemas, plus descriptor.proto and plugin.proto. These "
            "are deliberate, curated sets maintained by the people who define the format -- "
            "not an arbitrary sample of the repository. The unittest*.proto family is "
            "excluded on purpose: it carries a large transitive import closure and mixes in "
            "intentionally-invalid fixtures used for compiler error-path tests, which would "
            "produce false failures under a 'must parse' assertion."
        ),
        probe="src/google/protobuf/descriptor.proto",
    ),
    Source(
        name="protobuf-benchmarks",
        repo="https://github.com/protocolbuffers/protobuf",
        ref="v21.12",
        sha="f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c",
        patterns=["/benchmarks/*.proto", "/benchmarks/datasets/**"],
        include_root="benchmarks",
        license="BSD-3-Clause",
        why=(
            "The official cross-language benchmark suite: benchmarks.proto plus the "
            "google_message1/google_message2 datasets -- anonymized real Google production "
            "message shapes, and the basis of protobuf's own published performance figures, "
            "so numbers measured on them are checkable by third parties. Pinned to v21.12 "
            "because the benchmarks directory was DELETED from later releases; the tag is "
            "the only way to get it. The google_message3/4 SCHEMAS come along as extra parse "
            "coverage; their datasets are git-LFS and are not in-tree at this tag, so no "
            "pointer files land."
        ),
        probe="benchmarks/benchmarks.proto",
    ),
    Source(
        name="googleapis",
        repo="https://github.com/googleapis/googleapis",
        ref="09bc253ea6b336c1a214487684f4eb303bd7fc62",
        sha="09bc253ea6b336c1a214487684f4eb303bd7fc62",
        patterns=["/google/**/*.proto"],
        include_root=".",
        license="Apache-2.0",
        why=(
            "Breadth: thousands of production schemas with heavy custom options, deep "
            "import graphs, and every naming shape in use. This is where parser gaps show up "
            "-- pinned to a SHA because upstream has no release tags."
        ),
        probe="google/rpc/status.proto",
    ),
]


def run(args: list[str], cwd: Path | None = None) -> None:
    """Run a git command, turning a failure into a message rather than a traceback.

    The overwhelmingly likely failure here is "no network" or "host unreachable", which a
    stack trace only obscures -- git has already printed the real reason to stderr.
    """
    try:
        subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError as exc:
        raise SystemExit(
            f"git failed (exit {exc.returncode}): {' '.join(args)}\n"
            f"See git's own error above. Any previously fetched corpus is left untouched: "
            f"this stages into <name>.partial and only swaps it in on success."
        ) from exc


def capture(args: list[str], cwd: Path | None = None) -> str:
    """`run`, but for a command whose stdout is the answer. Same failure contract."""
    try:
        return subprocess.run(
            args, cwd=cwd, check=True, capture_output=True, text=True
        ).stdout.strip()
    except subprocess.CalledProcessError as exc:
        raise SystemExit(
            f"git failed (exit {exc.returncode}): {' '.join(args)}\n{(exc.stderr or '').strip()}"
        ) from exc


def stamp_path(dest: Path, source: Source) -> Path:
    return dest / ".stamps" / f"{source.name}.json"


def is_current(dest: Path, source: Source) -> bool:
    """True when this source is already checked out at the pinned commit.

    The stamp alone is NOT trusted: it records what we meant to fetch, and a checkout that
    was interrupted, truncated, or left behind by a failed pin bump can disagree with it
    while still looking plausible. `git rev-parse HEAD` in the checkout is the ground truth,
    so ask it -- a corpus silently disagreeing with its pin is exactly the hidden variable
    the pin policy exists to prevent.
    """
    work = dest / source.name
    stamp = stamp_path(dest, source)
    if not stamp.is_file() or not (work / source.probe).is_file():
        return False
    try:
        recorded = json.loads(stamp.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    if recorded.get("ref") != source.ref or recorded.get("sha") != source.sha:
        return False
    try:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=work, check=True, capture_output=True, text=True
        ).stdout.strip()
    except (subprocess.CalledProcessError, OSError):
        return False  # not a usable checkout, whatever the stamp says
    return head == source.sha

def fetched_sources(dest: Path) -> "tuple[list[Source], list[str], list[str]]":
    """Split SOURCES into (present-and-at-their-pin, stale, absent) under `dest`.

    THE answer to "which corpus sources are usable" -- corpus_gate, corpus_compile and
    compile_bench all ask here rather than re-deriving it (one of them used to trust a bare
    directory listing, which sampled a stale or half-fetched corpus silently). Stale and absent
    stay separate because they mean opposite things: nothing fetched is a legitimate skip, a
    checkout at the wrong commit is the loudest failure the corpus consumers have.
    """
    present, stale, absent = [], [], []
    for source in SOURCES:
        if is_current(dest, source):
            present.append(source)
        elif (dest / source.name).is_dir():
            stale.append(source.name)
        else:
            absent.append(source.name)
    return present, stale, absent


def include_root_for(dest: Path, proto: Path) -> Path:
    """The -I root a corpus file is generated with, in ONE place.

    The source's DECLARED include_root (protobuf's schemas import relative to `src`, the
    benchmarks' to `benchmarks`), falling back to the source's checkout root for files that live
    OUTSIDE it (protobuf keeps its editions/conformance schemas beside `src`, and those import
    relative to the checkout). corpus_gate and corpus_compile used to answer this independently
    and differed on exactly the outside-root files.
    """
    rel = proto.relative_to(dest)
    source_dir = dest / rel.parts[0]
    declared_rel = next((s.include_root for s in SOURCES if s.name == rel.parts[0]), ".")
    declared = (source_dir / declared_rel).resolve()
    return declared if declared in proto.resolve().parents else source_dir.resolve()




def is_ours(path: Path) -> bool:
    """Whether `path` looks like a checkout this script created (and may therefore delete)."""
    return (path / ".git").exists()


def checkout(work: Path, source: Source) -> None:
    """Populate the (empty) staging directory `work` with `source` at its pinned commit.

    Fetch-then-checkout (rather than `git clone --branch`) because a pinned SHA is not a
    branch name; sparse patterns are set BEFORE checkout so the unwanted blobs are never
    materialized in the first place. Returns only once the commit and the probe both check
    out, so a caller that swaps this in cannot publish a wrong or truncated tree.
    """
    run(["git", "init", "-q"], cwd=work)
    run(["git", "remote", "add", "origin", source.repo], cwd=work)
    run(["git", "sparse-checkout", "set", "--no-cone", *source.patterns], cwd=work)
    run(
        ["git", "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", source.ref],
        cwd=work,
    )
    run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=work)

    resolved = capture(["git", "rev-parse", "HEAD"], cwd=work)
    if resolved != source.sha:
        raise SystemExit(
            f"{source.name}: ref '{source.ref}' resolved to {resolved}, but the corpus pins "
            f"{source.sha}.\n"
            f"Either upstream moved the ref, or the pin records an ANNOTATED TAG OBJECT rather "
            f"than the commit it points at -- `git ls-remote --tags URL PATTERN` does not print "
            f"the peeled `refs/tags/X^{{}}` line, so it is easy to record the wrong one. The "
            f"commit is what `git rev-parse` yields after checkout:\n"
            f"    git ls-remote --tags {source.repo} | grep '{source.ref}'\n"
            f"Verify the change is benign, then update the pin in {Path(__file__).name}."
        )
    if not (work / source.probe).is_file():
        raise SystemExit(
            f"{source.name}: fetch succeeded but {source.probe} is missing -- the "
            f"sparse-checkout patterns {source.patterns} no longer match upstream's layout."
        )


def fetch(dest: Path, source: Source) -> None:
    """Fetch one source at its pinned ref and publish it as `dest/<name>`.

    Staged into a sibling `<name>.partial` and swapped in only once the commit and the probe
    both check out. `git checkout` is NOT atomic over the working tree -- interrupt it, or
    run out of disk, and it leaves a partial tree behind -- so building in place would let a
    truncated corpus inherit a valid-looking stamp. It also means a failed refetch (bad pin,
    no network) leaves the previous good corpus untouched instead of destroying it first.
    """
    final = dest / source.name
    work = dest / f"{source.name}.partial"
    if final.exists() and not is_ours(final):
        raise SystemExit(
            f"{final} exists but is not a git checkout this script created; refusing to "
            f"delete it. Move it aside, or pick a different --dest."
        )
    for stale in (work, dest / f"{source.name}.old"):
        if stale.exists():
            shutil.rmtree(stale)
    work.mkdir(parents=True)

    try:
        checkout(work, source)
    except BaseException:  # including SystemExit / KeyboardInterrupt
        # Don't strand the staging tree: for googleapis that is ~100 MB of garbage whose only
        # other cleanup is the next run of this script.
        shutil.rmtree(work, ignore_errors=True)
        raise

    # Drop the old stamp BEFORE swapping: if the process dies mid-swap, the next run must see
    # "not current" and refetch, never inherit a stamp that outlived its tree.
    stamp = stamp_path(dest, source)
    stamp.unlink(missing_ok=True)
    if final.exists():
        final.rename(dest / f"{source.name}.old")
    work.rename(final)
    shutil.rmtree(dest / f"{source.name}.old", ignore_errors=True)

    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps({"ref": source.ref, "sha": source.sha}, indent=2) + "\n")


def count_protos(root: Path) -> int:
    return sum(1 for _ in root.rglob("*.proto"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=DEFAULT_DEST,
        help="corpus directory; the C++ tests only probe the default, so a custom "
        "--dest is for inspection, not for feeding the gate",
    )
    parser.add_argument(
        "--only", action="append", default=[], metavar="NAME", help="fetch just this source (repeatable)"
    )
    parser.add_argument("--force", action="store_true", help="refetch even when the stamp matches")
    parser.add_argument("--list", action="store_true", help="describe the sources and exit (no network)")
    args = parser.parse_args()

    selected = SOURCES
    if args.only:
        known = {s.name for s in SOURCES}
        unknown = sorted(set(args.only) - known)
        if unknown:
            parser.error(f"unknown source(s): {', '.join(unknown)} (known: {', '.join(sorted(known))})")
        selected = [s for s in SOURCES if s.name in args.only]

    if args.list:
        for source in selected:
            print(f"{source.name}  {source.repo}@{source.ref}  [{source.license}]")
            print(f"    include root : <dest>/{source.name}/{source.include_root}")
            print(f"    paths        : {' '.join(source.patterns)}")
            print(f"    why          : {source.why}")
            print()
        return 0

    if not shutil.which("git"):
        raise SystemExit("error: git is required to fetch the corpus")

    args.dest.mkdir(parents=True, exist_ok=True)
    for source in selected:
        if not args.force and is_current(args.dest, source):
            print(f"{source.name}: up to date ({source.ref})", flush=True)
            continue
        print(f"{source.name}: fetching {source.repo}@{source.ref} ...", flush=True)
        fetch(args.dest, source)
        print(f"{source.name}: {count_protos(args.dest / source.name)} .proto files")

    print(f"corpus ready at {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
