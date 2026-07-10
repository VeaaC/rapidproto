#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Driver for the two decode benchmarks (rapidproto_bench = streaming, rapidproto_arena_bench = arena).

The benches emit NDJSON when RAPIDPROTO_BENCH_JSON=1 (see tests/bench_harness.hpp); this collects both
into one snapshot and renders a unified table. Four subcommands:

  bench.py run   [--build-dir D] [--core N] [--out FILE]   build both, run both pinned, write a snapshot
  bench.py table SNAPSHOT [SNAPSHOT ...]                    render one snapshot, or compare several
  bench.py diff  OLD NEW [--threshold PCT]                  ins/B regression check (exit 1 on regression)
  bench.py experiment BASELINE_REF [VARIANT_REF]           build+snapshot two git refs, then diff them

A snapshot is NDJSON: one `{"rec":"snapshot",...}` header (compiler / protobuf / git rev) then every
bench record, each tagged with `"decoder":"stream"|"arena"`. ins/B is the deterministic signal (retired
instructions per byte -- identical across machines for a given binary + input), so it is what the
multi-snapshot compare keys on; cyc/B and GB/s are same-machine-only and shown for context.
"""
import argparse
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCHES = [("stream", "rapidproto_bench"), ("arena", "rapidproto_arena_bench")]


# ── run: build + execute + collect ────────────────────────────────────────────────────────────────

def compiler_label(build_dir):
    """Read CMAKE_CXX_COMPILER out of the build dir's cache; basename is a good short label."""
    cache = os.path.join(build_dir, "CMakeCache.txt")
    try:
        with open(cache) as f:
            for line in f:
                if line.startswith("CMAKE_CXX_COMPILER:"):
                    return os.path.basename(line.split("=", 1)[1].strip())
    except OSError:
        pass
    return "unknown-cxx"


def git_rev():
    try:
        return subprocess.check_output(
            ["git", "-C", REPO, "rev-parse", "--short", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def build_and_run(build_dir, core):
    """Build both bench targets in build_dir and run each pinned (core='none'/'' skips pinning),
    returning (records, protobuf_version) with every record tagged by its decoder."""
    targets = [t for _, t in BENCHES]
    print(f"building {', '.join(targets)} in {build_dir} ...", file=sys.stderr)
    subprocess.check_call(["cmake", "--build", build_dir, "--target", *targets])

    env = dict(os.environ, RAPIDPROTO_BENCH_JSON="1")
    no_pin = str(core).lower() in ("", "none")
    pin = [] if no_pin else ["taskset", "-c", str(core)]
    records, protobuf_version = [], None
    for decoder, target in BENCHES:
        binary = os.path.join(build_dir, target)
        where = "unpinned" if no_pin else f"pinned to core {core}"
        print(f"running {decoder} ({binary}) {where} ...", file=sys.stderr)
        out = subprocess.check_output([*pin, binary], env=env, text=True)
        for line in out.splitlines():
            if not line.startswith("{"):
                continue
            rec = json.loads(line)
            rec["decoder"] = decoder
            if rec.get("rec") == "meta" and "protobuf_version" in rec:
                protobuf_version = rec["protobuf_version"]
            records.append(rec)
    return records, protobuf_version


def write_snapshot(records, protobuf_version, build_dir, core, out_path):
    """Write a snapshot (header + records) to out_path (defaulted from compiler+rev), returning it."""
    header = {
        "rec": "snapshot",
        "compiler": compiler_label(build_dir),
        "protobuf_version": protobuf_version,
        "git_rev": git_rev(),  # after a checkout this reports the checked-out ref's rev
        "build_dir": os.path.relpath(build_dir, REPO),
        "core": core,
    }
    out_path = out_path or os.path.join(
        REPO, "bench_snapshots", f"{header['compiler']}-{header['git_rev']}.ndjson")
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write(json.dumps(header) + "\n")
        for rec in records:
            f.write(json.dumps(rec) + "\n")
    print(f"wrote {len(records)} records -> {out_path}", file=sys.stderr)
    return out_path


def run(args):
    records, pv = build_and_run(args.build_dir, args.core)
    write_snapshot(records, pv, args.build_dir, args.core, args.out)


# ── table: render / compare ───────────────────────────────────────────────────────────────────────

def load(path):
    header, arms, mems = None, [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            kind = rec.get("rec")
            if kind == "snapshot":
                header = rec
            elif kind == "arm":
                arms.append(rec)
            elif kind == "mem":
                mems.append(rec)
    return header, arms, mems


def fmt(v, spec):
    if v is None or v < 0:  # pad the sentinel to the field width so columns stay aligned
        m = re.search(r"\d+", spec)
        return "n/a".rjust(int(m.group()) if m else 0)
    return format(v, spec)


def render_one(path):
    header, arms, mems = load(path)
    h = header or {}
    print(f"snapshot: {h.get('compiler','?')} / protobuf {h.get('protobuf_version','?')} "
          f"/ rev {h.get('git_rev','?')}   ({os.path.basename(path)})")
    for decoder, title in (("stream", "STREAMING decoder"), ("arena", "ARENA decoder")):
        rows = [a for a in arms if a["decoder"] == decoder]
        if not rows:
            continue
        print(f"\n{title}")
        print(f"  {'scenario':<24}{'arm':<14}{'GB/s':>7}{'cyc/B':>8}{'ins/B':>9}"
              f"{'vs base':>10}  verdict")
        last = None
        for a in rows:
            scen = "" if a["scenario"] == last else a["scenario"]
            last = a["scenario"]
            vs = "baseline" if a["baseline"] else f"{a['vs_base_pct']:+.1f}%"
            flag = "" if a["ok"] else "  MISMATCH"
            print(f"  {scen:<24}{a['arm']:<14}{a['gb_s']:>7.2f}"
                  f"{fmt(a.get('cyc_b'), '>8.2f')}{fmt(a.get('ins_b'), '>9.2f')}"
                  f"{vs:>10}  {a['verdict']}{flag}")
    if mems:
        print("\nmemory -- materializers, bytes (arena vs protoc, lower is better)")
        print(f"  {'shape':<14}{'arena_used':>12}{'protoc_used':>13}{'used x':>8}"
              f"{'arena_held':>13}{'protoc_held':>13}{'held x':>8}")
        for m in mems:
            ux = m["arena_used"] / m["protoc_used"] if m["protoc_used"] else 0
            hx = m["arena_held"] / m["protoc_held"] if m["protoc_held"] else 0
            print(f"  {m['shape']:<14}{m['arena_used']:>12}{m['protoc_used']:>13}{ux:>8.2f}"
                  f"{m['arena_held']:>13}{m['protoc_held']:>13}{hx:>8.2f}")


def render_compare(paths):
    """Multiple snapshots side by side, keyed on (decoder, scenario, arm). ins/B is deterministic, so
    its cross-snapshot delta is a genuine codegen difference (lower is better -> a winner is named, and
    for two snapshots a signed Δ% is computed). GB/s is same-machine and placement-sensitive, so it is
    shown as raw context columns with NO delta -- a cross-build GB/s delta would fold in layout noise."""
    snaps = [load(p) for p in paths]
    labels = [(h or {}).get("compiler", os.path.basename(p)) for (h, _, _), p in zip(snaps, paths)]
    for lab, p in zip(labels, paths):
        print(f"  {lab:<16} <- {os.path.basename(p)}")

    keys, index = [], {}
    for si, (_, arms, _) in enumerate(snaps):
        for a in arms:
            k = (a["decoder"], a["scenario"], a["arm"])
            if k not in index:
                index[k] = {}
                keys.append(k)
            index[k][si] = a

    def print_rows(metric, delta):
        head = f"  {'decoder':<8}{'scenario':<24}{'arm':<14}"
        head += "".join(f"{lab[:10]:>11}" for lab in labels)
        if delta:
            head += f"{'delta':>9}  win"
        print(head)
        last = None
        for k in keys:
            decoder, scen, arm = k
            tag = (decoder, scen)
            d = "" if last and decoder == last[0] else decoder
            s = "" if tag == last else scen
            last = tag
            vals = [(index[k].get(si) or {}).get(metric) for si in range(len(snaps))]
            cells = "".join(fmt(v, ">11.2f") if v is not None else f"{'--':>11}" for v in vals)
            tail = ""
            if delta:
                present = [(si, v) for si, v in enumerate(vals) if v is not None and v >= 0]
                if len(present) >= 2:  # lower ins/B is better -> the smallest wins
                    best_si, _ = min(present, key=lambda t: t[1])
                    win = labels[best_si][:10]
                    if len(snaps) == 2 and all(v is not None and v >= 0 for v in vals) and vals[0]:
                        dpct = (vals[1] - vals[0]) / vals[0] * 100
                        tail = f"{dpct:>+8.1f}%  {win}"
                    else:
                        tail = f"{'':>9}  {win}"  # >2 snapshots: name the winner, skip the ambiguous Δ
            print(f"  {d:<8}{s:<24}{arm:<14}{cells}{tail}")

    print("\nins/B  (deterministic, cross-machine; lower is better, delta = 2nd vs 1st)")
    print_rows("ins_b", delta=True)
    print("\nGB/s  (same-machine only, placement-sensitive -- no delta computed on purpose)")
    print_rows("gb_s", delta=False)


def table(args):
    if len(args.snapshots) == 1:
        render_one(args.snapshots[0])
    else:
        render_compare(args.snapshots)


def diff(args):
    """Regression check between two snapshots (old -> new), keyed on (decoder, scenario, arm). Gates on
    ins/B ONLY: it is deterministic (same binary + input -> same value on any machine), so a change is a
    real codegen difference, not noise -- unlike GB/s, which is same-machine and placement-sensitive and
    so is not comparable across two builds. Exits 1 if any arm's ins/B rose by more than --threshold."""
    if args.threshold < 0:  # a negative threshold would make the regression/improvement sets overlap
        sys.exit("diff: --threshold must be >= 0")
    (ho, ao, _), (hn, an, _) = load(args.old), load(args.new)
    ho, hn = ho or {}, hn or {}
    tag = lambda h: f"{h.get('compiler', '?')} rev {h.get('git_rev', '?')}"
    print(f"diff: {tag(ho)}  ->  {tag(hn)}   (threshold {args.threshold:.1f}% ins/B)")
    if ho.get("compiler") != hn.get("compiler"):
        print("  note: compilers differ -- this is a codegen comparison, not a same-compiler regression check")

    by_key = lambda arms: {(a["decoder"], a["scenario"], a["arm"]): a for a in arms}
    old_i, new_i = by_key(ao), by_key(an)

    rows = []  # (delta_pct, decoder, scenario, arm, old_ins, new_ins) for arms in BOTH with valid ins/B
    for k, a in new_i.items():
        oi = (old_i.get(k) or {}).get("ins_b")
        ni = a.get("ins_b")
        if oi is None or ni is None or oi < 0 or ni < 0 or not oi:
            continue
        rows.append(((ni - oi) / oi * 100, k[0], k[1], k[2], oi, ni))
    added = [k for k in new_i if k not in old_i]
    removed = [k for k in old_i if k not in new_i]

    t = args.threshold
    regr = sorted((r for r in rows if r[0] > t), key=lambda r: -r[0])
    impr = sorted((r for r in rows if r[0] < -t), key=lambda r: r[0])

    def show(title, group):
        if not group:
            return
        print(f"\n{title}")
        print(f"  {'decoder':<8}{'scenario':<24}{'arm':<14}{'old':>9}{'new':>9}{'delta':>9}")
        for dpct, dec, scen, arm, oi, ni in group:
            print(f"  {dec:<8}{scen:<24}{arm:<14}{oi:>9.2f}{ni:>9.2f}{dpct:>+8.1f}%")

    show(f"regressions (ins/B up > {t:.1f}%)", regr)
    show(f"improvements (ins/B down > {t:.1f}%)", impr)

    extra = f"; {len(added)} added, {len(removed)} removed" if (added or removed) else ""
    print(f"\n{len(rows) - len(regr) - len(impr)} arms unchanged (|delta| <= {t:.1f}%){extra}")
    if regr:
        print(f"\nFAIL: {len(regr)} ins/B regression(s) exceed {t:.1f}%")
        sys.exit(1)
    print(f"\nOK: no ins/B regression beyond {t:.1f}%")


def current_ref():
    """The branch name if on one, else the detached-HEAD commit sha -- what to restore to afterwards."""
    try:
        branch = subprocess.check_output(
            ["git", "-C", REPO, "symbolic-ref", "-q", "--short", "HEAD"], text=True).strip()
        if branch:
            return branch
    except subprocess.CalledProcessError:
        pass  # detached HEAD -> symbolic-ref exits non-zero; fall back to the sha
    return subprocess.check_output(["git", "-C", REPO, "rev-parse", "HEAD"], text=True).strip()


def experiment(args):
    """Build+snapshot two git refs (baseline, then variant) in the same build dir and diff them. The
    two are independent builds with different code placement, so it diffs on ins/B (deterministic) via
    diff() -- cyc/B and GB/s are not comparable across builds. Refuses to run on a dirty working tree
    (it checks out refs) and always restores the original ref, even if a build fails."""
    if args.threshold < 0:
        sys.exit("experiment: --threshold must be >= 0")
    if subprocess.check_output(["git", "-C", REPO, "status", "--porcelain"], text=True).strip():
        sys.exit("experiment: working tree is dirty -- commit or stash first (this checks out refs and "
                 "always restores, but refuses to risk uncommitted work)")

    variant_ref = args.variant or current_ref()
    original = current_ref()
    snapdir = os.path.join(REPO, "bench_snapshots")

    def snapshot_ref(ref, name):
        print(f"\n=== {name}: {ref} ===", file=sys.stderr)
        subprocess.check_call(["git", "-C", REPO, "checkout", "-q", ref])
        records, pv = build_and_run(args.build_dir, args.core)
        if not any(r.get("rec") == "arm" for r in records):
            sys.exit(f"experiment: ref '{ref}' emitted no NDJSON arm records -- it likely predates the "
                     "machine-readable bench harness; both refs must be able to emit NDJSON")
        return write_snapshot(records, pv, args.build_dir, args.core,
                              os.path.join(snapdir, f"exp-{name}.ndjson"))

    try:
        base_snap = snapshot_ref(args.baseline, "baseline")
        var_snap = snapshot_ref(variant_ref, "variant")
    finally:
        try:
            subprocess.check_call(["git", "-C", REPO, "checkout", "-q", original])
            print(f"restored {original}", file=sys.stderr)
        except subprocess.CalledProcessError:  # make a stranded checkout loud, not a raw traceback
            print(f"WARNING: failed to restore {original}; recover with: git checkout {original}",
                  file=sys.stderr)

    print()
    diff(argparse.Namespace(old=base_snap, new=var_snap, threshold=args.threshold))


# ── cli ─────────────────────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="build both benches, run pinned, write a snapshot")
    r.add_argument("--build-dir", default=os.path.join(REPO, "build", "gcc-pb25"))
    r.add_argument("--core", default="2", help="taskset core to pin to, or 'none' to skip pinning (default 2)")
    r.add_argument("--out", default=None, help="snapshot path (default bench_snapshots/<cc>-<rev>.ndjson)")
    r.set_defaults(func=run)

    t = sub.add_parser("table", help="render one snapshot, or compare several")
    t.add_argument("snapshots", nargs="+")
    t.set_defaults(func=table)

    d = sub.add_parser("diff", help="ins/B regression check between two snapshots (exit 1 on regression)")
    d.add_argument("old")
    d.add_argument("new")
    d.add_argument("--threshold", type=float, default=1.0, help="regression threshold in %% ins/B (default 1.0)")
    d.set_defaults(func=diff)

    e = sub.add_parser("experiment", help="build+snapshot two git refs and diff them on ins/B")
    e.add_argument("baseline", help="git ref for the baseline (built and snapshotted first)")
    e.add_argument("variant", nargs="?", default=None, help="git ref for the variant (default: current HEAD)")
    e.add_argument("--build-dir", default=os.path.join(REPO, "build", "gcc-pb25"))
    e.add_argument("--core", default="2", help="taskset core, or 'none' to skip pinning (default 2)")
    e.add_argument("--threshold", type=float, default=1.0, help="regression threshold in %% ins/B (default 1.0)")
    e.set_defaults(func=experiment)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
