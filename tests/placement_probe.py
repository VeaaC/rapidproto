#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Measure how much a bench arm's throughput depends on CODE PLACEMENT alone.

    python3 tests/placement_probe.py --build-dir build/gcc-pb25            # 20 layouts
    python3 tests/placement_probe.py --build-dir build/gcc --seeds 4 --allow-unprepared

The instrument: relink the ALREADY-BUILT bench objects N times, identical bytes every time,
with lld shuffling the input-section order (--shuffle-sections) — a different but equally-valid
layout per seed, exactly the degree of freedom an ordinary rebuild exercises implicitly. Each
layout runs the probed scenarios pinned, and the per-(scenario, arm) spread across layouts IS
that arm's placement sensitivity: same instructions, same data, different addresses.

Two uses:
  - calibration: the measured per-arm spread replaces the folklore "~10% floor" with a number,
    and flags which arms need per-arm floors in bench.py's gating;
  - judging mitigations: alignment flags or per-arm section isolation are only real if they
    shrink this spread. Extra link flags for such an A/B go through --ld-extra.

The probe LINKS with lld while the normal bench links with GNU ld; layout sensitivity is a
property of the code, not of which linker arranged it, and only shuffled-vs-shuffled spreads
are compared. Expect ins/B spread ~0 on every row — that is the placement signature; a row
where ins/B moves too is NOT placement and needs a different explanation.
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


def quiesced() -> bool:
    try:
        turbo = pathlib.Path("/sys/devices/system/cpu/intel_pstate/no_turbo").read_text().strip()
        gov = pathlib.Path(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read_text().strip()
        return turbo == "1" and gov == "performance"
    except OSError:
        return False


def link_once(build_dir: pathlib.Path, target: str, out: pathlib.Path, extra: list[str]) -> None:
    link_txt = build_dir / "CMakeFiles" / f"{target}.dir" / "link.txt"
    cmd = shlex.split(link_txt.read_text().strip())
    # A later -o wins, so appending ours redirects the output without editing the command.
    cmd += extra + ["-o", str(out)]
    subprocess.run(cmd, cwd=build_dir, check=True, capture_output=True, text=True)


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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", type=pathlib.Path, required=True)
    ap.add_argument("--target", default="rapidproto_arena_bench")
    ap.add_argument("--seeds", type=int, default=20)
    ap.add_argument("--patterns", default=None,
                    help="comma-separated RAPIDPROTO_BENCH_ONLY patterns, one run each "
                         "(default: the flagged-sensitive set for --target)")
    ap.add_argument("--core", type=int, default=2)
    ap.add_argument("--ld-extra", default="",
                    help="extra link flags for a mitigation A/B (applied to every layout)")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    ap.add_argument("--allow-unprepared", action="store_true",
                    help="run on an unquiesced box (plumbing smoke only -- spreads then "
                         "mix frequency noise into the layout signal)")
    args = ap.parse_args()

    if not quiesced() and not args.allow_unprepared:
        raise SystemExit("box is not quiesced (tests/bench_box.sh setup) -- a layout spread "
                         "measured under turbo/governor noise is not a layout spread. "
                         "--allow-unprepared overrides for plumbing smokes.")

    build_dir = args.build_dir.resolve()
    subprocess.run(["cmake", "--build", str(build_dir), "--target", args.target],
                   check=True, capture_output=True, text=True)
    probe_bin = build_dir / "placement_probe_bin"
    patterns = ([p for p in args.patterns.split(",") if p] if args.patterns
                else DEFAULT_PATTERNS.get(args.target, ["Dataset"]))
    extra_base = ["-fuse-ld=lld"] + (shlex.split(args.ld_extra) if args.ld_extra else [])

    records = []
    for seed in range(1, args.seeds + 1):
        link_once(build_dir, args.target,
                  probe_bin, extra_base + [f"-Wl,--shuffle-sections=.text*={seed}"])
        for pattern in patterns:
            for rec in run_probe(probe_bin, pattern, args.core):
                rec["seed"] = seed
                records.append(rec)
        done = sum(1 for r in records if r["seed"] == seed)
        print(f"layout {seed:>3}/{args.seeds}: {done} arm rows", flush=True)

    out_path = args.out or (REPO / "bench_snapshots" / "placement-probe.ndjson")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        for rec in records:
            f.write(json.dumps(rec) + "\n")

    # Per-(scenario, arm) spread across layouts. GB/s spans the layouts; ins/B must not.
    by_key: dict[tuple[str, str], list[dict]] = {}
    for rec in records:
        by_key.setdefault((rec["scenario"], rec["arm"]), []).append(rec)
    print(f"\nper-arm layout sensitivity over {args.seeds} layouts "
          f"({'quiesced' if quiesced() else 'UNPREPARED BOX'}):")
    print(f"  {'scenario':<24}{'arm':<14}{'med GB/s':>9}{'min':>8}{'max':>8}"
          f"{'spread':>8}{'ins/B spread':>14}")
    rows = []
    for (scen, arm), rs in sorted(by_key.items()):
        gbs = [r["gb_s"] for r in rs]
        med = statistics.median(gbs)
        spread = (max(gbs) - min(gbs)) / med * 100 if med else 0.0
        ins = [r["ins_b"] for r in rs if r.get("ins_b") is not None]
        ins_spread = ((max(ins) - min(ins)) / statistics.median(ins) * 100) if ins else None
        ins_txt = f"{ins_spread:13.1f}%" if ins_spread is not None else f"{'n/a':>14}"
        print(f"  {scen:<24}{arm:<14}{med:9.3f}{min(gbs):8.3f}{max(gbs):8.3f}"
              f"{spread:7.1f}%{ins_txt}")
        rows.append(spread)
    print(f"\nworst spread {max(rows):.1f}%; records -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
