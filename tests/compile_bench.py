#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Measure what RapidProto's generated decoders cost to COMPILE, and how big they are.

Decode throughput is measured by tests/bench.py. This measures the other half of the bargain a
code generator strikes with its user: build time, code size, and the compiler's peak memory.
All three were invisible, and all three scale badly -- `RP_FLATTEN` on every message's
`rp_decode_into` transitively inlines the whole sub-message closure, so on gcc a 10-message
nesting chain takes ~60s and 599 KB of `.text` where clang takes ~1.1s and 48 KB.

Methodology
-----------
Each translation unit defines **one external-linkage function per message**, taking the input
as a parameter. That is the load-bearing part, and the reason is narrow: external linkage
obliges the compiler to emit each body, and an opaque parameter stops it reasoning about the
input. Measured, an earlier design that merely `#include`d the header emitted ~0 bytes.

The streaming rows additionally forward each value's address to an undefined `extern` sink. Its
effect on `.text` is NOT a simple increase: A/B'd, it ranges from -34% (gcc/proto3) to +38%
(clang/chain10), varying by both compiler and schema. It is kept because it makes the streaming
rows represent a consumer that actually reads the values it is handed -- not because it
reliably enlarges the measurement.

The message list comes from the ARENA header for BOTH models, so the two models always measure
the same set. Deriving each model's list from its own header silently diverged: the streaming
header nests sub-messages inside their parent, so a top-level scan found 23 of descriptor's 34
messages and the alphabetical cap then sampled a different set per model.

Peak RSS is reported because it is the failure that actually stops a build: one arena TU peaks
near 1 GB on gcc, against 7 GB CI runners running parallel jobs.

Caveats that the numbers do NOT capture:
  * The streaming rows use a catch-all callback, which does not recurse into sub-messages. A
    recursing consumer over one chain10 message costs ~4.0s/47 KB against the 0.8s/21.6 KB
    reported here for all ten, so the streaming column understates a nesting-heavy consumer and
    is flat in exactly the dimension flatten stresses. Every record carries `recurses: false`.
  * `.text` includes shared runtime and libstdc++ COMDAT code that is not attributable to the
    schema, and its size is COMPILER-dependent: a trivial one-message baseline measures ~10.0 KB
    on gcc against ~4.0 KB on clang. Since the table puts the two side by side, subtract the
    baseline before reading a gcc/clang `.text` gap as schema code.

Message counts are CAPPED per case only because the flatten cost makes uncapped numbers
impractical. Every record carries the cap and a digest of the exact decoders instantiated;
`diff` refuses to compare records whose set differs, because those are not the same
measurement. Lifting the caps belongs to the flatten fix, not here.

Usage:
    python3 tests/compile_bench.py run                    # measure, write a snapshot
    python3 tests/compile_bench.py run --compiler g++-13 --case chain10
    python3 tests/compile_bench.py table SNAP [SNAP...]
    python3 tests/compile_bench.py diff OLD NEW           # exit 1 past the threshold

Snapshots follow tests/bench.py's convention (NDJSON, a `{"rec": "snapshot", ...}` header,
under bench_snapshots/) but carry their own record type: compile seconds and GB/s are not
comparable quantities. Records are written as they are produced, so an interrupted or OOM-killed
run still leaves the measurements it completed -- which matters most on the runs peak RSS exists
to catch.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SNAPDIR = REPO / "bench_snapshots"
CORPUS = REPO / "build" / "corpus"

# Arena decode entry points, e.g. `inline const Outer::Inner* Outer::Inner::decode(...)`. The
# qualified name is what a call site needs, and it is the same inside the streaming namespace.
DECODE_DEF = re.compile(r"^inline const ([A-Za-z0-9_:]+)\* \1::decode\(", re.MULTILINE)
NAMESPACE = re.compile(r"^namespace ([A-Za-z0-9_:]+) \{", re.MULTILINE)

# Compile with the flags the project's own performance targets use (CMakeLists' bench targets),
# so these numbers describe the build a user actually ships rather than a debug-ish -O2.
CXXFLAGS = ["-std=c++17", "-O3", "-DNDEBUG"]

# A crude backstop ONLY: it catches a TU that measured nothing at all (a decoder-name regex gone
# stale against the generator, or headers included but never referenced). It deliberately does
# not pretend to catch subtler under-measurement -- with external linkage the code is emitted
# even when a callback is empty, so those modes differ by ~1% and no threshold separates them.
MIN_TEXT_PER_DECODER = 256

# One compile should be seconds to a minute. Past this something is wrong (or a cap was lifted
# without re-checking), and a hung compile must not silently consume the whole run.
COMPILE_TIMEOUT_S = 900


@dataclass(frozen=True)
class Case:
    """One schema to measure.

    `max_messages` caps how many decoders the TU instantiates. None means "all"; a number is
    always a concession to the current flatten cost, never a statement about the schema.
    """

    name: str
    # schema/include are unused when chain_depth is set: that case is synthesized into a temp
    # directory instead of read from the tree.
    schema: str = ""  # relative to REPO, or to CORPUS when needs_corpus
    include: str = ""  # -I root, same convention
    max_messages: int | None = None
    needs_corpus: bool = False
    chain_depth: int | None = None


# Ordered cheapest-first: records are streamed to the snapshot, so an interrupted run still
# yields the cases that completed.
CASES: list[Case] = [
    # In-repo, always available: a feature-complete proto3 schema, and the >64-required-fields
    # message whose multi-word transient mask is its own codegen shape.
    Case("proto3", "tests/corpus/proto3.proto", "tests/corpus"),
    Case("manyreq", "tests/corpus/arena_manyreq.proto", "tests/corpus"),
    # Synthesized nesting chains: the shape that exposed the scaling problem, and the cleanest
    # control for it, since only the depth varies.
    Case("chain5", chain_depth=5),
    Case("chain10", chain_depth=10),
    # Real schemas. descriptor.proto is densely mutually recursive -- measured N=1/3/5 at
    # 50/47/54s, i.e. FLAT, because the first decoder already drags in the whole closure and
    # the rest reuse it, so a small cap loses little. compute.proto is wide and shallow
    # (103k lines, 2223 decoders), where the cap is doing real work.
    Case("descriptor", "protobuf/src/google/protobuf/descriptor.proto",
         "protobuf/src", max_messages=5, needs_corpus=True),
    Case("compute", "googleapis/google/cloud/compute/v1beta/compute.proto",
         "googleapis", max_messages=10, needs_corpus=True),
]

DEFAULT_COMPILERS = ["g++-13", "clang++-20"]
METRICS = ("seconds", "text_bytes", "peak_rss_kb")


def git_rev() -> str:
    rev = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True, check=False).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain"],
                           capture_output=True, text=True, check=False).stdout.strip()
    return (rev or "unknown") + ("-dirty" if dirty else "")


def write_chain(directory: Path, depth: int) -> Path:
    """A chain of `depth` messages, each referencing the next.

    The worst realistic shape for flatten: message N's decoder inlines N+1's, which inlines
    N+2's, so emitted code grows with the whole tail rather than with one message. Each message
    also carries a string, a scalar and a repeated field so its body is not degenerate.
    """
    lines = ['syntax = "proto3";', "package chain;"]
    for i in range(1, depth + 1):
        lines += [f"message M{i} {{", "  string a = 1;", "  int32 b = 2;",
                  "  repeated int64 c = 3;"]
        if i < depth:
            lines.append(f"  M{i + 1} next = 4;")
        lines.append("}")
    path = directory / f"chain{depth}.proto"
    path.write_text("\n".join(lines) + "\n")
    return path


def namespace_of(header: Path) -> str:
    """The header's namespace, or "" for a package-less schema (types land at global scope).

    The generator emits exactly one top-level namespace per header (imports land in their own
    included headers), and every call site here depends on that. Assert it rather than take the
    first match: a future emitter that opened a helper namespace first would otherwise qualify
    every name against the wrong one, silently.
    """
    if not header.is_file():
        raise SystemExit(f"expected generated header {header} is missing")
    found = NAMESPACE.findall(header.read_text())
    if len(found) > 1:
        raise SystemExit(
            f"{header.name} declares {len(found)} top-level namespaces ({', '.join(found)}); "
            f"this harness assumes one and would qualify names against the wrong one."
        )
    return found[0] if found else ""


def qualified(namespace: str, name: str) -> str:
    return f"::{namespace}::{name}" if namespace else f"::{name}"


def text_bytes(obj: Path) -> int:
    """Sum the object's executable sections.

    Match `.text` exactly or as a `.text.` prefix -- a substring test also counts
    `.rodata._ZN...SourceContext...` and `.gcc_except_table._ZN...` for any message whose
    MANGLED NAME happens to contain "text", which is neither code nor bounded in principle.
    """
    dump = subprocess.run(["objdump", "-h", str(obj)], capture_output=True, text=True)
    if dump.returncode != 0:
        raise SystemExit(f"objdump failed on {obj}:\n{dump.stderr[-500:]}")
    total = 0
    for line in dump.stdout.splitlines():
        parts = line.split()
        if len(parts) > 2 and (parts[1] == ".text" or parts[1].startswith(".text.")):
            total += int(parts[2], 16)
    return total


def measure(compiler: str, tu: Path, include: Path) -> tuple[float, int, int]:
    """Compile one TU; return (wall seconds, .text bytes, peak RSS KB)."""
    obj = tu.with_suffix(".o")
    # /usr/bin/time for peak RSS: the compiler's own memory is what OOMs a runner, and it is
    # not observable from the parent process.
    argv = ["/usr/bin/time", "-f", "%M", compiler, *CXXFLAGS,
            f"-I{include}", "-c", str(tu), "-o", str(obj)]
    start = time.monotonic()
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=COMPILE_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise SystemExit(f"{compiler} exceeded {COMPILE_TIMEOUT_S}s on {tu.name}") from None
    elapsed = time.monotonic() - start
    if proc.returncode != 0:
        raise SystemExit(f"compile failed ({compiler}, {tu.name}):\n{proc.stderr[-2000:]}")
    return elapsed, text_bytes(obj), int(proc.stderr.strip().splitlines()[-1])


def emit_tu(path: Path, header: str, namespace: str, names: list[str], model: str) -> None:
    """One external-linkage function per message (see the module docstring on why)."""
    body = [f'#include "{header}"', "#include <cstddef>", ""]
    if model == "stream":
        # Declared, never defined: with -c there is nothing to link, so the call is opaque and
        # &val makes each decoded value's address escape, forcing materialization.
        body += ["extern void rp_bench_sink(const void* value, std::size_t size);", ""]
    for index, name in enumerate(names):
        symbol = f"rp_bench_{index}_{name.replace('::', '_')}"
        target = qualified(namespace, name)
        if model == "arena":
            body += [f"bool {symbol}(::rapidproto::ByteView v, ::rapidproto::Arena& a) {{",
                     f"    return {target}::decode(v, a) != nullptr;", "}"]
        else:
            body += [f"std::size_t {symbol}(::rapidproto::ByteView v) {{",
                     "    std::size_t n = 0;",
                     f"    (void){target}{{v}}.decode([&n](auto, auto&& val) {{",
                     "        rp_bench_sink(&val, sizeof(val));", "        ++n;", "    });",
                     "    return n;", "}"]
    path.write_text("\n".join(body) + "\n")


def run_case(tool: Path, case: Case, compiler: str, work: Path) -> list[dict]:
    """Generate once, then measure both models over the SAME decoder set."""
    root = CORPUS if case.needs_corpus else REPO
    if case.chain_depth is not None:
        schema, include = write_chain(work, case.chain_depth), work
    else:
        schema, include = root / case.schema, root / case.include
        if not schema.is_file():
            raise SystemExit(f"{case.name}: {schema} not found (corpus pin drifted?)")

    out = work / f"gen-{case.name}"
    out.mkdir(parents=True, exist_ok=True)
    gen = subprocess.run([str(tool), "--arena", "--stream", f"-I{include}",
                          "--out-dir", str(out), str(schema)], capture_output=True, text=True)
    if gen.returncode != 0:
        raise SystemExit(f"generation failed ({case.name}):\n{gen.stderr[-1000:]}")

    stem = schema.relative_to(include).with_suffix("")
    arena_header, stream_header = out / f"{stem}.rp.hpp", out / f"{stem}.rp.stream.hpp"
    names = sorted(set(DECODE_DEF.findall(arena_header.read_text())))
    if not names:
        raise SystemExit(
            f"{case.name}: found no decoders in {arena_header.name}. The generated shape no "
            f"longer matches DECODE_DEF, so this would have measured an empty translation unit."
        )
    capped = names[: case.max_messages] if case.max_messages else names
    digest = hashlib.sha256("\n".join(capped).encode()).hexdigest()[:12]

    records = []
    for model, header in (("arena", arena_header), ("stream", stream_header)):
        tu = work / f"tu-{case.name}-{model}.cpp"
        emit_tu(tu, f"{stem}.rp.{'stream.' if model == 'stream' else ''}hpp",
                namespace_of(header), capped, model)
        seconds, text, rss = measure(compiler, tu, out)
        if text < MIN_TEXT_PER_DECODER * len(capped):
            raise SystemExit(
                f"{case.name}/{model}/{compiler}: {text} bytes of .text for {len(capped)} "
                f"decoders is implausibly small -- the TU measured nothing. Check that "
                f"DECODE_DEF still matches the generator's output."
            )
        records.append({
            "rec": "compile", "case": case.name, "model": model, "compiler": compiler,
            "schema_messages": len(names), "instantiated": len(capped),
            "max_messages": case.max_messages, "decoder_digest": digest,
            "recurses": model == "arena",  # streaming catch-all does not descend
            "seconds": round(seconds, 2), "text_bytes": text, "peak_rss_kb": rss,
        })
    return records


def cmd_run(args: argparse.Namespace) -> int:
    tool = REPO / "build" / "gcc" / "rapidprotoc"
    if not tool.is_file():
        raise SystemExit(f"{tool} not found -- build it first (cmake --build --preset gcc)")
    compilers = args.compiler or DEFAULT_COMPILERS
    for compiler in compilers:
        if not shutil.which(compiler):
            raise SystemExit(f"{compiler} not found")
    for binary in ("/usr/bin/time", "objdump"):
        if not shutil.which(binary):
            raise SystemExit(f"{binary} is required")

    cases = [c for c in CASES if not args.case or c.name in args.case]
    if args.case:
        unknown = sorted(set(args.case) - {c.name for c in CASES})
        if unknown:
            raise SystemExit(f"unknown case(s): {', '.join(unknown)}")
    # Record what was left out, in the snapshot itself: a case that vanishes silently is how a
    # shrinking benchmark comes to look like a passing one.
    skipped = [c.name for c in cases if c.needs_corpus and not CORPUS.is_dir()]
    cases = [c for c in cases if c.name not in skipped]
    if not cases:
        raise SystemExit("no cases to run (corpus not fetched? see tests/fetch_corpus.py)")
    if skipped:
        print(f"note: skipping {', '.join(skipped)} (corpus not fetched)", file=sys.stderr)

    out = Path(args.out) if args.out else \
        SNAPDIR / f"compile-{'+'.join(compilers)}-{git_rev()}.ndjson"
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists() and not args.force:
        raise SystemExit(
            f"{out} exists. Re-running would overwrite it -- and a partial snapshot from an "
            f"interrupted or OOM-killed run is exactly what you would lose. Pass --force to "
            f"replace it, or --out to write elsewhere."
        )
    written = 0
    with out.open("w") as handle:
        handle.write(json.dumps({
            "rec": "snapshot", "kind": "compile", "git_rev": git_rev(),
            "compilers": compilers, "cxxflags": CXXFLAGS, "skipped_cases": skipped,
            # What was ASKED for, so a truncated run is detectable: `skipped_cases` alone cannot
            # distinguish "ran everything" from "ran one case via --case".
            "requested_cases": [c.name for c in cases],
        }) + "\n")
        handle.flush()
        with tempfile.TemporaryDirectory(prefix="rpcompile-") as tmp:
            for case in cases:
                for compiler in compilers:
                    for rec in run_case(tool, case, compiler, Path(tmp)):
                        handle.write(json.dumps(rec) + "\n")
                        handle.flush()  # survive an interrupt or an OOM kill
                        written += 1
                        print(f"{rec['case']:<12}{rec['model']:<8}{rec['compiler']:<12}"
                              f"{rec['seconds']:>8.2f}s  text={rec['text_bytes']:>9}  "
                              f"rss={rec['peak_rss_kb']:>8}KB  msgs={rec['instantiated']}",
                              flush=True)
    print(f"wrote {written} records -> {out}", file=sys.stderr)
    return 0


def load(path: Path) -> tuple[dict, list[dict]]:
    if not Path(path).is_file():
        raise SystemExit(f"{path}: no such snapshot")
    header, records = {}, []
    for line in Path(path).read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("rec") == "snapshot":
            header = rec
        elif rec.get("rec") == "compile":
            records.append(rec)
    return header, records


def key(rec: dict) -> tuple:
    return (rec["case"], rec["model"], rec["compiler"])


def cmd_table(args: argparse.Namespace) -> int:
    for path in args.snapshots:
        header, records = load(path)
        print(f"\n=== {path}  (rev {header.get('git_rev', '?')}, "
              f"{' '.join(header.get('cxxflags', []))}) ===")
        if header.get("skipped_cases"):
            print(f"  skipped: {', '.join(header['skipped_cases'])}")
        print(f"{'case':<12}{'model':<8}{'compiler':<12}{'seconds':>9}{'.text':>11}"
              f"{'peakRSS':>10}{'msgs':>7}")
        for rec in sorted(records, key=key):
            print(f"{rec['case']:<12}{rec['model']:<8}{rec['compiler']:<12}"
                  f"{rec['seconds']:>9.2f}{rec['text_bytes']:>11}{rec['peak_rss_kb']:>10}"
                  f"{rec['instantiated']:>7}")
        print("  note: streaming rows use a non-recursing catch-all, so they understate a "
              "consumer that descends into sub-messages.")
    return 0


def cmd_diff(args: argparse.Namespace) -> int:
    """Regression check on compile seconds, .text and peak RSS between two snapshots.

    All three are near-deterministic -- the same TU compiled twice varies by a few percent, with
    no code-placement floor to hide behind -- so the threshold is far tighter than the decode
    benchmark's ~10%.
    """
    if args.threshold < 0:
        raise SystemExit("diff: --threshold must be >= 0")
    old_header, old_records = load(args.old)
    new_header, new_records = load(args.new)
    for name, header in (("old", old_header), ("new", new_header)):
        if header.get("kind") != "compile":
            raise SystemExit(
                f"{name} snapshot is not a compile-bench snapshot (kind="
                f"{header.get('kind')!r}); tests/bench.py snapshots measure a different thing."
            )
    if old_header.get("cxxflags") != new_header.get("cxxflags"):
        raise SystemExit(
            f"compiler flags differ ({old_header.get('cxxflags')} vs "
            f"{new_header.get('cxxflags')}); the delta would be real but meaningless."
        )
    old_index = {key(r): r for r in old_records}
    regressions, skipped, compared = [], [], 0
    for rec in new_records:
        old = old_index.get(key(rec))
        if old is None:
            continue
        # Comparing across a changed decoder SET is not a regression check. `instantiated`
        # alone is not enough: a corpus pin bump can add a message that sorts into the capped
        # prefix, keeping the count identical while sampling different code.
        if (old.get("instantiated") != rec.get("instantiated")
                or old.get("decoder_digest") != rec.get("decoder_digest")):
            skipped.append(f"{'/'.join(key(rec))}: decoder set changed "
                           f"({old.get('instantiated')} -> {rec.get('instantiated')} decoders)")
            continue
        checked = 0
        for metric in METRICS:
            before, after = old.get(metric), rec.get(metric)
            if not before or not after or before <= 0:
                continue
            checked += 1
            delta = (after - before) / before * 100.0
            if delta > args.threshold:
                regressions.append(
                    f"{'/'.join(key(rec))} {metric}: {before} -> {after}  (+{delta:.1f}%)")
        # Count pairs whose metrics were actually comparable. A renamed metric key would
        # otherwise degrade diff to checking nothing while still reporting a pair count.
        if checked:
            compared += 1
        else:
            skipped.append(f"{'/'.join(key(rec))}: no comparable metrics")
    vanished = sorted(set(old_index) - {key(r) for r in new_records})
    for missing in vanished:
        print(f">> MISSING FROM NEW: {'/'.join(missing)}")
    for line in skipped:
        print(f"skipped (not the same measurement): {line}")
    for line in regressions:
        print(f">> REGRESSION {line}")
    if vanished:
        print(f">> {len(vanished)} measurement(s) present in {args.old} are absent from "
              f"{args.new}. A run that measured FEWER cases must not read as an improvement; "
              f"re-run the missing cases (corpus not fetched? interrupted? --case filter?).")
        return 1
    if regressions:
        return 1
    # A comparison that matched nothing must never read as success -- snapshots from different
    # compilers share no keys at all, and skipped pairs are not compared pairs.
    if compared == 0:
        raise SystemExit(
            f"diff compared 0 pairs ({len(skipped)} skipped). The snapshots share no comparable "
            f"(case, model, compiler) records -- this is not a regression check."
        )
    print(f"OK: {compared} pairs compared ({len(skipped)} skipped), no regression beyond "
          f"{args.threshold:.1f}%")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    run_parser = sub.add_parser("run", help="measure and write a snapshot")
    run_parser.add_argument("--compiler", action="append",
                            help=f"repeatable (default: {' '.join(DEFAULT_COMPILERS)})")
    run_parser.add_argument("--case", action="append",
                            help=f"repeatable (default: all of {', '.join(c.name for c in CASES)})")
    run_parser.add_argument("--out", help="snapshot path (default: bench_snapshots/compile-...)")
    run_parser.add_argument("--force", action="store_true",
                            help="overwrite an existing snapshot instead of refusing")
    run_parser.set_defaults(func=cmd_run)

    table_parser = sub.add_parser("table", help="render one or more snapshots")
    table_parser.add_argument("snapshots", nargs="+", type=Path)
    table_parser.set_defaults(func=cmd_table)

    diff_parser = sub.add_parser("diff", help="regression check between two snapshots")
    diff_parser.add_argument("old", type=Path)
    diff_parser.add_argument("new", type=Path)
    diff_parser.add_argument("--threshold", type=float, default=20.0,
                             help="percent growth counting as a regression (default: 20)")
    diff_parser.set_defaults(func=cmd_diff)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
