#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Measure how much a bench arm's throughput depends on CODE PLACEMENT alone.

    python3 tests/placement_probe.py --build-dir build/gcc-pb25            # 20 layouts
    python3 tests/placement_probe.py --build-dir build/gcc --seeds 4 --allow-unprepared

The instrument: relink the ALREADY-BUILT bench objects N times, identical bytes every time,
with lld shuffling the input-section order (--shuffle-sections) — a different but equally-valid
layout per seed, the degree of freedom an ordinary rebuild exercises implicitly. Each layout
runs the probed scenarios pinned, and the per-(scenario, arm) spread across layouts bounds that
arm's placement sensitivity from above: same instructions, same data, different addresses. An
upper bound, not a pure reading — each layout is measured once, so the spread also contains
run-to-run noise; the probe ends by re-running the first layout and printing that same-layout
delta as the noise reference. The environment is held fixed across layouts (a real cross-build
comparison also varies environment size, another placement input this probe deliberately
freezes).

Two uses:
  - calibration: the measured per-arm spread replaces folklore with a number, re-derivable
    after any codegen change;
  - judging LINK-side mitigations via --ld-extra. COMPILE-side A/Bs (alignment flags) need a
    separately configured build dir per variant — the probe measures whatever the build dir
    was compiled with.

Requires the Makefile generator (the link command is read from CMake's link.txt) and lld. The
probe links with lld while the normal bench links with GNU ld; layout sensitivity is a property
of the code, and only shuffled-vs-shuffled spreads are compared. Expect ins/B spread ~0 on
every row — the placement signature; a row where ins/B moves too (upb's ~1%: its work adapts
slightly) is an upper bound with a non-placement component.
"""
import argparse
import json
import pathlib
import shlex
import statistics
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# Per-target defaults: the flagged-sensitive scenarios plus a stable control each.
DEFAULT_PATTERNS = {
    "rapidproto_arena_bench": ["rv fx1 1M", "osm_blocks", "Dataset"],
    "rapidproto_bench": ["fixed32-float", "rv fx1 1M"],
}


def quiesce_problems(core: int) -> list[str]:
    """Like bench.py: complain only about a node that exists AND reads wrong, so non-Intel
    boxes without intel_pstate are not refused on a path they cannot have."""
    problems = []
    turbo = pathlib.Path("/sys/devices/system/cpu/intel_pstate/no_turbo")
    if turbo.is_file() and turbo.read_text().strip() == "0":
        problems.append("turbo is on")
    gov = pathlib.Path(f"/sys/devices/system/cpu/cpu{core}/cpufreq/scaling_governor")
    if gov.is_file() and gov.read_text().strip() != "performance":
        problems.append(f"cpu{core} governor is not 'performance'")
    smt = pathlib.Path("/sys/devices/system/cpu/smt/control")
    if smt.is_file() and smt.read_text().strip() == "on":
        problems.append("SMT is on")
    return problems


def link_once(build_dir: pathlib.Path, target: str, out: pathlib.Path, extra: list[str]) -> None:
    link_txt = build_dir / "CMakeFiles" / f"{target}.dir" / "link.txt"
    if not link_txt.is_file():
        raise SystemExit(f"{link_txt} not found -- the probe reads the Makefile generator's "
                         "link command; configure the build dir with -G 'Unix Makefiles'")
    cmd = shlex.split(link_txt.read_text().strip())
    # A later -o wins, so appending ours redirects the output without editing the command.
    cmd += extra + ["-o", str(out)]
    r = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"link failed:\n{r.stderr}")


def run_probe(binary: pathlib.Path, pattern: str, core: int) -> list[dict]:
    r = subprocess.run(
        ["taskset", "-c", str(core), str(binary)],
        env={"PATH": "/usr/bin:/bin", "RAPIDPROTO_BENCH_JSON": "1",
             "RAPIDPROTO_BENCH_ONLY": pattern},
        capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"probe run failed for pattern {pattern!r}:\n{r.stderr}")
    out = []
    for line in r.stdout.splitlines():
        if line.startswith("{"):
            rec = json.loads(line)
            if rec.get("rec") == "arm":
                out.append(rec)
    return out


def one_layout(build_dir, target, probe_bin, extra_base, seed, patterns, core, warned):
    link_once(build_dir, target, probe_bin,
              extra_base + [f"-Wl,--shuffle-sections=.text*={seed}"])
    rows = []
    for pattern in patterns:
        got = run_probe(probe_bin, pattern, core)
        if not got and pattern not in warned:
            print(f"note: pattern {pattern!r} matched no scenario in {target} "
                  "(missing corpus/dataset?)", flush=True)
            warned.add(pattern)
        for rec in got:
            rec["seed"] = seed
            rows.append(rec)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=pathlib.Path, required=True)
    ap.add_argument("--target", default="rapidproto_arena_bench")
    ap.add_argument("--seeds", type=int, default=20)
    ap.add_argument("--patterns", default=None,
                    help="comma-separated RAPIDPROTO_BENCH_ONLY patterns, one run each "
                         "(default: the flagged-sensitive set for --target)")
    ap.add_argument("--core", type=int, default=2)
    ap.add_argument("--ld-extra", default="",
                    help="extra LINK flags for a link-side A/B (compile-side variants need "
                         "their own build dir)")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    ap.add_argument("--allow-unprepared", action="store_true",
                    help="run on an unquiesced box (plumbing smoke only -- spreads then "
                         "mix frequency noise into the layout signal)")
    args = ap.parse_args()
    if args.seeds < 2:
        ap.error("--seeds must be >= 2 (a single layout has no spread)")

    problems = quiesce_problems(args.core)
    if problems and not args.allow_unprepared:
        raise SystemExit("box is not quiesced: " + "; ".join(problems) +
                         " (tests/bench_box.sh setup). A layout spread measured under "
                         "frequency noise is not a layout spread; --allow-unprepared "
                         "overrides for plumbing smokes.")

    build_dir = args.build_dir.resolve()
    r = subprocess.run(["cmake", "--build", str(build_dir), "--target", args.target],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"build failed:\n{r.stderr}")
    probe_bin = build_dir / "placement_probe_bin"
    patterns = ([p for p in args.patterns.split(",") if p] if args.patterns
                else DEFAULT_PATTERNS.get(args.target, ["Dataset"]))
    extra_base = ["-fuse-ld=lld"] + (shlex.split(args.ld_extra) if args.ld_extra else [])

    header = {"rec": "probe", "target": args.target, "build_dir": str(args.build_dir),
              "seeds": args.seeds, "patterns": patterns, "core": args.core,
              "quiesced": not problems, "ld_extra": args.ld_extra}
    records, warned = [], set()
    for seed in range(1, args.seeds + 1):
        rows = one_layout(build_dir, args.target, probe_bin, extra_base, seed, patterns,
                          args.core, warned)
        records.extend(rows)
        print(f"layout {seed:>3}/{args.seeds}: {len(rows)} arm rows", flush=True)
    # Same-layout control: seed 1 again. Its delta against the first pass is run noise at ONE
    # fixed layout -- the part of every spread below that is not attributable to placement.
    control = one_layout(build_dir, args.target, probe_bin, extra_base, 1, patterns,
                         args.core, warned)
    if not records:
        raise SystemExit("no arm rows at all -- every pattern matched nothing")

    out_path = args.out or (REPO / "bench_snapshots" / "placement-probe.ndjson")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        f.write(json.dumps(header) + "\n")
        for rec in records:
            f.write(json.dumps(rec) + "\n")
        for rec in control:
            rec["control"] = True
            f.write(json.dumps(rec) + "\n")

    by_key: dict[tuple[str, str], list[dict]] = {}
    for rec in records:
        by_key.setdefault((rec["scenario"], rec["arm"]), []).append(rec)
    first = {(r["scenario"], r["arm"]): r["gb_s"] for r in records if r["seed"] == 1}
    ctrl = {(r["scenario"], r["arm"]): r["gb_s"] for r in control}

    print(f"\nper-arm layout sensitivity over {args.seeds} layouts "
          f"({'quiesced' if not problems else 'UNPREPARED BOX'}):")
    print(f"  {'scenario':<24}{'arm':<14}{'med GB/s':>9}{'min':>8}{'max':>8}"
          f"{'spread':>8}{'ins/B spread':>14}{'noise ref':>11}")
    spreads = []
    for (scen, arm), rs in sorted(by_key.items()):
        gbs = [r["gb_s"] for r in rs]
        med = statistics.median(gbs)
        spread = (max(gbs) - min(gbs)) / med * 100 if med else 0.0
        ins = [r["ins_b"] for r in rs if r.get("ins_b") is not None]
        ins_spread = ((max(ins) - min(ins)) / statistics.median(ins) * 100) if ins else None
        ins_txt = f"{ins_spread:13.1f}%" if ins_spread is not None else f"{'n/a':>14}"
        noise = ""
        if (scen, arm) in first and (scen, arm) in ctrl and first[(scen, arm)]:
            noise = f"{abs(ctrl[(scen, arm)] - first[(scen, arm)]) / first[(scen, arm)] * 100:9.1f}%"
        print(f"  {scen:<24}{arm:<14}{med:9.3f}{min(gbs):8.3f}{max(gbs):8.3f}"
              f"{spread:7.1f}%{ins_txt}{noise:>11}")
        spreads.append(spread)
    print(f"\nworst spread {max(spreads):.1f}% (noise ref = same-layout rerun delta); "
          f"records -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
