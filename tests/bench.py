#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Driver for the two decode benchmarks (rapidproto_bench = streaming, rapidproto_arena_bench = arena).

The benches emit NDJSON when RAPIDPROTO_BENCH_JSON=1 (see tests/bench_harness.hpp); this collects both
into one snapshot and renders a unified table. Four subcommands:

  bench.py run   [--build-dir D] [--core N] [--repeat K] [--out F]  build both, run pinned, snapshot
  bench.py table SNAPSHOT [SNAPSHOT ...]                            render one, or compare several
  bench.py diff  OLD NEW [--threshold PCT]                          regression check (exit 1 on fail)
  bench.py experiment BASELINE_REF [VARIANT_REF]                    snapshot two git refs, then diff

A snapshot is NDJSON: one `{"rec":"snapshot",...}` header (compiler / protobuf / git rev) then every
bench record, each tagged with `"decoder":"stream"|"arena"`. GB/s (measured decode throughput) is the
PRIMARY signal -- it is what a reader actually cares about and, unlike ins/B, it reflects everything the
CPU pays for (branch mispredictions, cache/memory stalls), so it is what the compare and the regression
gate key on. Caveat: across independent builds GB/s carries code-PLACEMENT noise -- byte-identical
functions measure ~10% apart from address/alignment alone (architecture.md), plus frequency drift -- so
the gate keys on the larger of that ~10% floor and the arm's own measured run-to-run spread; sub-floor
cross-build deltas are not reliable (quiesce the box -- tests/bench_box.sh -- and pin a core).
cyc/B and ins/B are kept as diagnostics: cyc/B is frequency-invariant timing (why is throughput low --
mispredicts show here, not in ins/B), and ins/B is deterministic retired-work (identical across machines
for one binary+input, so it resolves a real sub-floor codegen change GB/s cannot -- but it is a rough
proxy for work, blind to the stalls above, not a substitute for measured time).

Snapshots also embed the COMPILE-COST sweep (rec:"compile": seconds / .text bytes / peak RSS per
case x model x compiler; ~2 min; --no-compile skips it) -- what the throughput above costs the
consumer's build, measured by tests/compile_bench.py's machinery so the two tools cannot drift.
`table` renders it, and `diff`/`experiment` gate it at a tight threshold: .text is deterministic
and peak RSS nearly so, with no placement floor to hide behind (compile SECONDS is wall clock --
load-sensitive like any timing, just without GB/s's cross-build placement problem).
"""
import argparse
import json
import statistics
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # same pattern as compile_bench itself
import compile_bench  # noqa: E402  -- the one home for the compile-cost machinery this tool embeds

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCHES = [("stream", "rapidproto_bench"), ("arena", "rapidproto_arena_bench")]
# Runs per snapshot. See build_and_run for the measurement that picked 5.
DEFAULT_REPEAT = 5


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


def check_machine(core):
    """Warn about box settings that make a measurement untrustworthy, with the exact fix.

    None of these persist across a reboot, and every one of them is silent: the bench still prints
    confident-looking numbers. Each was measured to matter on this bench (see docs/benchmarks.md) --
    SMT matters because whether the pinned core's sibling takes work is a per-run coin flip, which
    makes affected arms read as if they were bimodal. `tests/bench_box.sh setup` applies all of
    these and saves the previous values; `restore` puts them back."""
    def read(path):
        try:
            with open(path) as f:
                return f.read().strip()
        except OSError:
            return None

    problems = []
    paranoid = read("/proc/sys/kernel/perf_event_paranoid")
    # The harness opens a PER-TASK event with exclude_kernel=1, which levels 0-2 all permit; only
    # level 3+ (an extra level Debian/Ubuntu carry) denies perf_event_open outright.
    if paranoid is not None and paranoid.lstrip("-").isdigit() and int(paranoid) >= 3:
        problems.append(("hardware counters are blocked (cyc/B and ins/B will be n/a)",
                         "sudo sysctl -w kernel.perf_event_paranoid=2"))
    if read("/sys/devices/system/cpu/smt/control") == "on":
        problems.append(("SMT is on -- the pinned core's sibling may take unrelated work mid-run",
                         "sudo sh -c 'echo off > /sys/devices/system/cpu/smt/control'"))
    if read("/sys/devices/system/cpu/intel_pstate/no_turbo") == "0":
        problems.append(("turbo is enabled -- clock varies with thermal/power state",
                         "sudo sh -c 'echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo'"))
    gov = read(f"/sys/devices/system/cpu/cpu{core}/cpufreq/scaling_governor")
    if gov is not None and gov != "performance":
        problems.append((f"cpu{core} governor is '{gov}', not 'performance'",
                         "sudo sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/"
                         "scaling_governor; do echo performance > $g; done'"))
    if not problems:
        return
    print("\n*** benchmark box is not quiesced -- numbers will be noisier than the gate assumes.",
          file=sys.stderr)
    print("*** These do NOT survive a reboot; re-apply after every restart.", file=sys.stderr)
    for what, how in problems:
        print(f"***   - {what}\n***       {how}", file=sys.stderr)
    print("***   or run: tests/bench_box.sh setup   (and `restore` when you are done)",
          file=sys.stderr)
    print(file=sys.stderr)


def build_and_run(build_dir, core, repeat=DEFAULT_REPEAT):
    """Build both bench targets in build_dir and run each pinned (core='none'/'' skips pinning)
    `repeat` times, returning (records, protobuf_version) with every record tagged by its decoder.

    Why repeat at all: one run's converged number is not reproducible ACROSS PROCESS LAUNCHES. Each
    run already medians >=30 rotated rounds to a confident CI (bench_harness.hpp), but a few arms
    still land ~14% apart run to run on a quiesced box, so at repeat=1 two snapshots of
    IDENTICAL code differ by ~14% (p95) -- above any threshold worth gating on. Bootstrapped from 40
    repeated runs of the worst arm, that identical-code p95 falls to ~9% at 3 runs, ~8% at 5, ~6% at
    7: roughly 0.8pp per added run beyond 3, with no knee. 5 is a cost/benefit pick (7 would buy
    another ~2pp for 40% more wall time), measured on the WORST arm so it over-samples the rest.

    Each arm keeps its MEDIAN run, and every run's GB/s is kept in run order alongside it so a
    snapshot can be re-analysed without re-measuring. Not the FASTEST run, tempting as the
    "interference only subtracts throughput" argument is: across two snapshots of one binary the
    fastest-run estimator moved the worst arm -46.9% where the median moved +0.35%, because it
    records upward flukes the next snapshot has no reason to repeat.

    The median's sampling variance falls with K, so two snapshots are only comparable at equal
    `repeat`, which `diff` enforces."""
    check_machine(core)
    targets = [t for _, t in BENCHES]
    print(f"building {', '.join(targets)} in {build_dir} ...", file=sys.stderr)
    subprocess.check_call(["cmake", "--build", build_dir, "--target", *targets])

    env = dict(os.environ, RAPIDPROTO_BENCH_JSON="1")
    no_pin = str(core).lower() in ("", "none")
    pin = [] if no_pin else ["taskset", "-c", str(core)]
    protobuf_version = None
    meta, chosen_by_key = [], {}  # key -> the one run kept for this arm
    runs_by_key = {}  # key -> [record per run], so the kept one carries its own run's counters
    other = {}  # non-arm records (`mem` etc.): keep one, never aggregate
    for decoder, target in BENCHES:
        binary = os.path.join(build_dir, target)
        where = "unpinned" if no_pin else f"pinned to core {core}"
        for r in range(repeat):
            print(f"running {decoder} ({binary}) {where} [{r + 1}/{repeat}] ...", file=sys.stderr)
            try:
                out = subprocess.check_output([*pin, binary], env=env, text=True)
            except subprocess.CalledProcessError as e:
                # The binaries exit nonzero on a checksum mismatch, and WHICH scenario/arm diverged
                # is in their captured output ('"ok":false' per arm in json mode) -- without this
                # echo, a red CI bench run would say only "exit status 1".
                sys.stderr.write(e.output or "")
                raise
            for line in out.splitlines():
                if not line.startswith("{"):
                    continue
                rec = json.loads(line)
                rec["decoder"] = decoder
                if rec.get("rec") == "meta":
                    if "protobuf_version" in rec:
                        protobuf_version = rec["protobuf_version"]
                    if r == 0:
                        meta.append(rec)
                    continue
                if rec.get("rec") != "arm":  # `mem` and anything else: keep one, never aggregate
                    other.setdefault((decoder, rec.get("rec"), rec.get("shape")), rec)
                    continue
                key = (decoder, rec.get("scenario"), rec.get("arm"))
                gb = rec.get("gb_s")
                if gb is None:
                    chosen_by_key.setdefault(key, rec)
                    continue
                runs_by_key.setdefault(key, []).append(rec)
    for key, records in runs_by_key.items():
        v = [r["gb_s"] for r in records]  # run order
        ordered = sorted(range(len(v)), key=lambda i: v[i])
        # The median RUN, so the record carries that run's own cyc/ins/verdict rather than a value
        # interpolated across runs. At even K this is the upper of the two middle runs.
        chosen = records[ordered[len(ordered) // 2]]
        chosen_by_key[key] = chosen
        chosen["runs"] = len(v)
        chosen["gb_s_runs"] = v  # run order, not sorted: a whole-run effect is visible only in order
        # No mismatch handling here: a binary with ANY mismatched arm exits nonzero, so check_output
        # raises above and no snapshot is written. (`render` still reads "ok" -- older snapshots,
        # written before the binaries' verdicts reached their exit codes, can carry ok:false.)
        # Two-sided dispersion about the median: the interquartile range, relative to the median.
        #
        # The recorded value is the median, so what matters is how much the MIDDLE of the
        # distribution moves, not how far the extremes reach. An IQR ignores one outlier in either
        # direction.
        #
        # Requires >= 5 runs, and that is not a style preference: statistics.quantiles INTERPOLATES,
        # so at K=3 the "IQR" is exactly half the full range and at K=4 the extremes still carry
        # weight. Only from K=5 is it v[-2] - v[1]. Below 5 the arm reports 0.0 and `diff` falls
        # back to the flat threshold
        # and says so, rather than gating on a number that does not mean what it claims.
        chosen["spread_pct"] = 0.0
        if len(v) >= 5:
            q1, med, q3 = statistics.quantiles(sorted(v), n=4, method="inclusive")
            if med > 0:
                chosen["spread_pct"] = (q3 - q1) / med * 100.0
    return meta + list(other.values()) + list(chosen_by_key.values()), protobuf_version


def write_snapshot(records, protobuf_version, build_dir, core, out_path, extra_header=None):
    """Write a snapshot (header + records) to out_path (defaulted from compiler+rev), returning it.
    `extra_header` carries the embedded compile sweep's comparability fields (see collect_compile)."""
    header = {
        "rec": "snapshot",
        "compiler": compiler_label(build_dir),
        "protobuf_version": protobuf_version,
        "git_rev": git_rev(),  # after a checkout this reports the checked-out ref's rev
        "build_dir": os.path.relpath(build_dir, REPO),
        "core": core,
        # A filtered snapshot covers only part of the suite; `diff` refuses to gate on one.
        "scenario_filter": os.environ.get("RAPIDPROTO_BENCH_ONLY") or None,
    }
    # Merged AFTER so a collision would be visible in review; every extra key is compile_-prefixed
    # by contract (collect_compile), so none can shadow the fields above.
    header.update(extra_header or {})
    out_path = out_path or os.path.join(
        REPO, "bench_snapshots", f"{header['compiler']}-{header['git_rev']}.ndjson")
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write(json.dumps(header) + "\n")
        for rec in records:
            f.write(json.dumps(rec) + "\n")
    print(f"wrote {len(records)} records -> {out_path}", file=sys.stderr)
    return out_path


def compile_preflight(enabled):
    """The cheap availability checks, BEFORE any expensive measuring (the same rule experiment's
    own docstring states for RAPIDPROTO_BENCH_ONLY): a box without /usr/bin/time or a pinned
    compiler must refuse up front, not after a multi-minute bench run. Returns (cases, skipped)
    or (None, None) when the sweep is disabled."""
    if not enabled:
        return None, None
    compile_bench.check_tools(compile_bench.DEFAULT_COMPILERS)
    return compile_bench.prepare_cases(None)


def collect_compile(build_dir, cases, skipped):
    """Build rapidprotoc in build_dir (so an `experiment` measures each REF's own generator) and
    run the prepared compile-cost cases, returning (records, header_fields).
    tests/compile_bench.py is the one home for the machinery; this embeds its records --
    rec:"compile" -- into the decode snapshot so `table` and `diff` can show what the throughput
    COSTS. Full sweep measured at ~2 min, beside a multi-minute bench run.

    Provenance is deliberately MIXED and pinned at import: the harness, cases, flags and chain
    synthesis are the INVOKER's compile_bench (bound in sys.modules before any experiment
    checkout, so both refs are measured by one methodology); the generator and the tests/corpus
    schemas are the CHECKED-OUT ref's; descriptor/compute come from the shared build/corpus.
    An edit to a tests/corpus schema in the variant therefore shows up as a compile delta."""
    subprocess.check_call(["cmake", "--build", build_dir, "--target", "rapidprotoc"])
    tool = Path(build_dir) / "rapidprotoc"
    if not tool.is_file():
        raise SystemExit(f"{tool} not found after building target rapidprotoc")
    print(f"measuring compile cost ({len(cases)} cases x "
          f"{len(compile_bench.DEFAULT_COMPILERS)} compilers) ...", file=sys.stderr)
    records = compile_bench.collect(
        tool, compile_bench.DEFAULT_COMPILERS, cases,
        on_record=lambda rec: print("  " + compile_bench.progress_line(rec), file=sys.stderr))
    header_fields = {
        "compile_compilers": compile_bench.DEFAULT_COMPILERS,
        "compile_cxxflags": compile_bench.CXXFLAGS,
        "compile_skipped_cases": skipped,
        "compile_requested_cases": [c.name for c in cases],
    }
    return records, header_fields


def run(args):
    if args.repeat < 1:  # repeat=0 would write a header-only snapshot and exit 0
        sys.exit("run: --repeat must be >= 1")
    if os.environ.get("RAPIDPROTO_BENCH_ONLY"):
        print("WARNING: RAPIDPROTO_BENCH_ONLY is set -- this snapshot covers only part of the suite "
              "and `diff` will refuse it.", file=sys.stderr)
    cases, skipped = compile_preflight(not args.no_compile)
    records, pv = build_and_run(args.build_dir, args.core, args.repeat)
    extra_header = {}
    if cases is not None:
        compile_records, extra_header = collect_compile(args.build_dir, cases, skipped)
        records = records + compile_records
    write_snapshot(records, pv, args.build_dir, args.core, args.out, extra_header)


# ── table: render / compare ───────────────────────────────────────────────────────────────────────

def load(path):
    header, arms, mems, compiles = None, [], [], []
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
            elif kind == "compile":
                compiles.append(rec)
    return header, arms, mems, compiles


def fmt(v, spec):
    if v is None or v < 0:  # pad the sentinel to the field width so columns stay aligned
        m = re.search(r"\d+", spec)
        return "n/a".rjust(int(m.group()) if m else 0)
    return format(v, spec)


def render_one(path):
    header, arms, mems, compiles = load(path)
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
    if compiles:
        print("\ncompile cost -- what the generated decoders cost to BUILD (near-deterministic; "
              "no placement floor)")
        compile_bench.render(compiles, indent="  ")


def render_compare(paths):
    """Multiple snapshots side by side, keyed on (decoder, scenario, arm). GB/s is the primary metric --
    measured decode throughput -- so its cross-snapshot delta (and winner) is computed: this IS the
    cross-compiler / cross-backend comparison (higher is better). GB/s is same-machine and carries some
    placement/frequency noise, so read small deltas with care. ins/B is shown below as deterministic
    context (lower = less retired work; a rough proxy, not measured time)."""
    snaps = [load(p) for p in paths]
    labels = [(h or {}).get("compiler", os.path.basename(p)) for (h, *_), p in zip(snaps, paths)]
    for lab, p in zip(labels, paths):
        print(f"  {lab:<16} <- {os.path.basename(p)}")

    keys, index = [], {}
    for si, (_, arms, _, _) in enumerate(snaps):
        for a in arms:
            k = (a["decoder"], a["scenario"], a["arm"])
            if k not in index:
                index[k] = {}
                keys.append(k)
            index[k][si] = a

    def print_rows(metric, delta, higher_better):
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
                if len(present) >= 2:  # GB/s: highest wins; ins/B: lowest wins
                    best_si, _ = (max if higher_better else min)(present, key=lambda t: t[1])
                    win = labels[best_si][:10]
                    if len(snaps) == 2 and all(v is not None and v >= 0 for v in vals) and vals[0]:
                        dpct = (vals[1] - vals[0]) / vals[0] * 100
                        tail = f"{dpct:>+8.1f}%  {win}"
                    else:
                        tail = f"{'':>9}  {win}"  # >2 snapshots: name the winner, skip the ambiguous Δ
            print(f"  {d:<8}{s:<24}{arm:<14}{cells}{tail}")

    print("\nGB/s  (measured throughput; PRIMARY. delta = 2nd vs 1st, higher is better)")
    print_rows("gb_s", delta=True, higher_better=True)
    print("\nins/B  (deterministic context; lower = less retired work, a rough proxy -- not time)")
    print_rows("ins_b", delta=True, higher_better=False)

    # Compile cost side by side, keyed like compile_bench does (case, model, compiler): what the
    # throughput above COSTS. Only when every snapshot carries embedded compile records --
    # a mixed set would render a half-empty table that reads as data.
    if all(comp for (_, _, _, comp) in snaps):
        by_key = [{compile_bench.key(r): r for r in comp} for (_, _, _, comp) in snaps]
        ckeys = sorted({k for m in by_key for k in m})
        # Columns labeled by git rev (falling back to the file name), NOT by the decode-build
        # compiler: the embedded sweep always measures the same pinned compiler pair, so in a
        # gcc-vs-clang decode comparison two `g++-13` headings over identical rows would read as
        # a comparison while showing one measurement twice. Rows carry their own compiler column.
        clabels = [(h or {}).get("git_rev") or os.path.basename(p)
                   for (h, *_), p in zip(snaps, paths)]
        if len(set(clabels)) < len(clabels):
            clabels = [os.path.basename(p) for p in paths]
        for metric, title, spec in (("seconds", "compile seconds  (lower is better)", ">10.2f"),
                                    ("text_bytes", ".text bytes  (lower is better)", ">10")):
            print(f"\n{title}")
            head = "".join(f"{lab[:14]:>15}" for lab in clabels)
            print(f"  {'case':<12}{'model':<8}{'compiler':<12}{head}")
            for k in ckeys:
                cells = "".join(
                    f"{m[k][metric]:{spec}}".rjust(15) if k in m else f"{'-':>15}"
                    for m in by_key)
                print(f"  {k[0]:<12}{k[1]:<8}{k[2]:<12}{cells}")
    elif any(comp for (_, _, _, comp) in snaps):
        print("\nnote: some snapshots carry embedded compile records and some do not; "
              "compile cost not compared (re-snapshot, or use compile_bench.py directly)")


def table(args):
    if len(args.snapshots) == 1:
        render_one(args.snapshots[0])
    else:
        render_compare(args.snapshots)


def overhead_dominated(scenario):
    """The repeated-varint sweep's tiniest rows (n<=100 -> tens to hundreds of bytes) are dominated by
    per-op call/alloc/timer overhead, not decode, so their GB/s swings far more than the placement floor
    and is meaningless to gate. Excluded from pass/fail (still shown in the report); the deterministic
    ins/B column still compares them fine. Only the sweep uses this `rv <dist> <count>` naming."""
    return scenario.startswith("rv ") and scenario.rsplit(" ", 1)[-1] in ("10", "100")


def diff_compile(old_header, new_header, old_records, new_records):
    """Gate the embedded compile records (if both snapshots carry them). Returns a status pair
    (fail_line or None, ungated_reason or None) -- exactly one is non-None unless the gate ran
    clean (both None). The comparison itself is compile_bench.compare, the one home for what a
    compile regression means. Asymmetric or non-comparable snapshots report and skip rather than
    gate: an archived baseline predating the embedding must not fail every diff against it."""
    if not old_records and not new_records:
        return None, "neither snapshot embeds compile records"
    side = "old" if not old_records else "new"
    if not old_records or not new_records:
        print(f"\nnote: the {side} snapshot has no embedded compile records -- compile cost not "
              f"compared (re-snapshot both sides with `bench.py run`)")
        return None, f"the {side} snapshot has no compile records"
    for field in ("compile_cxxflags", "compile_requested_cases", "compile_compilers"):
        if old_header.get(field) != new_header.get(field):
            print(f"\nnote: {field} differs between the snapshots -- compile cost not compared "
                  f"(the delta would be real but meaningless)")
            return None, f"{field} differs"
    regressions, skipped, compared, vanished = compile_bench.compare(
        old_records, new_records, compile_bench.DEFAULT_THRESHOLD)
    print(f"\ncompile cost (threshold {compile_bench.DEFAULT_THRESHOLD:.1f}%; .text is "
          f"deterministic, RSS nearly so; seconds is wall clock)")
    for line in skipped:
        print(f"  skipped (not the same measurement): {line}")
    for line in regressions:
        print(f"  >> REGRESSION {line}")
    for missing in vanished:
        print(f"  >> MISSING FROM NEW: {'/'.join(missing)}")
    if compared == 0 and not vanished:
        print("  note: 0 comparable pairs -- compile cost not gated")
        return None, "0 comparable compile pairs"
    if regressions:
        return (f"FAIL: {len(regressions)} compile-cost regression(s) exceed "
                f"{compile_bench.DEFAULT_THRESHOLD:.1f}% (listed above); GB/s itself is clean"), None
    if vanished:
        return (f"FAIL: {len(vanished)} compile measurement(s) present in the old snapshot are "
                f"missing from the new one; a shrunken sweep must not read as a pass"), None
    print(f"  OK: {compared} pairs compared, no compile regression beyond "
          f"{compile_bench.DEFAULT_THRESHOLD:.1f}%")
    return None, None


def diff(args):
    """Regression check between two snapshots (old -> new), keyed on (decoder, scenario, arm). Gates on
    GB/s -- measured decode throughput, the real-performance signal (it catches what ins/B cannot: branch
    mispredictions, cache/memory stalls). Exits 1 if any arm's GB/s DROPPED past the gate.

    An arm fails only past BOTH the flat threshold (--threshold, default 10%: the cross-build
    code-PLACEMENT floor, since byte-identical functions measure ~10% apart from address/alignment
    alone) and its own measured spread_pct. Arms that moved past the flat threshold but stayed inside
    their own noise are listed separately and never gated -- an arm that noisy cannot resolve a change
    that size, which is information rather than a pass.

    For a sub-floor change, read the deterministic ins/B column, or the within-run `vs <baseline>`
    verdict in the PRETTY bench output, which is placement-robust because it ratios arms back-to-back
    in one binary. Run both snapshots pinned to a quiesced box (tests/bench_box.sh). The sweep's
    n<=100 scenarios are excluded from pass/fail (too small to time meaningfully)."""
    if args.threshold < 0:  # a negative threshold would make the regression/improvement sets overlap
        sys.exit("diff: --threshold must be >= 0")
    (ho, ao, _, co), (hn, an, _, cn) = load(args.old), load(args.new)
    ho, hn = ho or {}, hn or {}
    tag = lambda h: f"{h.get('compiler', '?')} rev {h.get('git_rev', '?')}"
    print(f"diff: {tag(ho)}  ->  {tag(hn)}   (threshold {args.threshold:.1f}% GB/s)")
    if ho.get("compiler") != hn.get("compiler"):
        print("  note: compilers differ -- this is a throughput comparison, not a same-compiler regression check")

    by_key = lambda arms: {(a["decoder"], a["scenario"], a["arm"]): a for a in arms}
    old_i, new_i = by_key(ao), by_key(an)

    # Both snapshots must use the same `repeat`. The median's sampling variance falls with K, and at
    # even K the harness keeps the UPPER of the two middle runs, so a mixed-K pair compares two
    # differently-behaved estimators -- one noisier, one slightly biased against the other. Refuse
    # rather than print a number nobody should act on.
    runs = lambda arms: {a.get("runs") for a in arms if a.get("gb_s")} or {None}
    ro, rn = runs(ao), runs(an)
    # Only refuse when BOTH sides state a K and they disagree. A snapshot written before `runs` was
    # recorded states nothing; refusing there would make every archived snapshot permanently
    # un-diffable, so those fall through to the flat threshold and the NOTE below.
    if None not in ro and None not in rn and ro != rn:
        sys.exit(f"diff: snapshots use different --repeat ({sorted(ro, key=str)} vs "
                 f"{sorted(rn, key=str)}); the median-of-K estimator is K-dependent, so "
                 f"re-measure at a common K")

    # (gb_s delta%, decoder, scenario, arm, old_gb, new_gb, old_ins, new_ins). Delta is signed as a
    # PERFORMANCE change: positive = faster (GB/s up), negative = slower (a regression).
    rows = []
    excluded = 0
    for k, a in new_i.items():
        if overhead_dominated(k[1]):
            excluded += 1
            continue
        og = (old_i.get(k) or {}).get("gb_s")
        ng = a.get("gb_s")
        if og is None or ng is None or og <= 0 or ng <= 0:
            continue
        oi = (old_i.get(k) or {}).get("ins_b")
        ni = a.get("ins_b")
        # This arm's own demonstrated noise: the wider of the two snapshots' run-to-run spreads.
        # An arm that moved less than that has not shown anything the repeated runs did not already
        # show on unchanged code, so it is reported but never failed on.
        noise = max((old_i.get(k) or {}).get("spread_pct") or 0.0, a.get("spread_pct") or 0.0)
        rows.append(((ng - og) / og * 100, k[0], k[1], k[2], og, ng, oi, ni, noise))
    added = [k for k in new_i if k not in old_i]
    removed = [k for k in old_i if k not in new_i]

    t = args.threshold
    # Gate an arm only when it clears BOTH the flat threshold and its own measured spread. A single
    # global threshold is wrong in both directions here: measured spreads across arms range from
    # under 1% to ~14%, so one number is too tight for the noisy arms and too loose for the
    # quiet ones. Snapshots without spread data (repeat=1, or written before it was recorded) score
    # 0 noise and fall back to the flat threshold alone.
    def gate(r):
        return max(t, r[8])

    regr = sorted((r for r in rows if r[0] < -gate(r)), key=lambda r: r[0])  # slower: GB/s dropped
    impr = sorted((r for r in rows if r[0] > gate(r)), key=lambda r: -r[0])  # faster: GB/s rose
    muted = [r for r in rows if abs(r[0]) > t and abs(r[0]) <= gate(r)]

    def ins_delta(oi, ni):
        if oi is None or ni is None or oi < 0 or ni < 0 or not oi:
            return "     n/a"
        return f"{(ni - oi) / oi * 100:>+7.1f}%"

    def show(title, group):
        if not group:
            return
        print(f"\n{title}")
        print(f"  {'decoder':<8}{'scenario':<24}{'arm':<14}{'old GB/s':>9}{'new GB/s':>9}"
              f"{'delta':>9}{'noise':>8}{'ins/B':>9}")
        for dpct, dec, scen, arm, og, ng, oi, ni, noise in group:
            print(f"  {dec:<8}{scen:<24}{arm:<14}{og:>9.2f}{ng:>9.2f}{dpct:>+8.1f}%"
                  f"{noise:>7.1f}%{ins_delta(oi, ni):>9}")

    show(f"regressions (GB/s down, beyond {t:.1f}% and the arm's own noise)", regr)
    show(f"improvements (GB/s up, beyond {t:.1f}% and the arm's own noise)", impr)
    show(f"moved > {t:.1f}% but within their own measured noise (NOT gated)", muted)

    # BEFORE the refusal exits below: those abort the GB/s verdict, and the measured compile data
    # should still have been reported by then rather than silently discarded.
    compile_fail_line, compile_ungated = diff_compile(ho, hn, co, cn)

    for h, side in ((ho, "old"), (hn, "new")):
        if h.get("scenario_filter"):
            sys.exit(f"diff: the {side} snapshot was written with RAPIDPROTO_BENCH_ONLY="
                     f"{h['scenario_filter']!r}, so it covers only part of the suite -- "
                     f"re-measure it unfiltered")
    if not rows:
        sys.exit("diff: no comparable arms (empty or truncated snapshot?) -- nothing was checked")
    if removed:
        sys.exit(f"diff: {len(removed)} arm(s) present in the old snapshot are missing from the new "
                 f"one, e.g. {removed[0]}; a partial snapshot cannot be gated")
    ex = f"; {excluded} tiny-buffer sweep scenarios excluded from the gate" if excluded else ""
    extra = f"; {len(added)} added, {len(removed)} removed" if (added or removed) else ""
    quiet = len(rows) - len(regr) - len(impr) - len(muted)
    print(f"\n{quiet} arms unchanged (|delta| <= {t:.1f}%){ex}{extra}")
    # Fires when EITHER side cannot supply usable per-arm noise -- no `spread_pct` at all (an
    # archived snapshot), or fewer than 5 runs, where the statistic is not defined. Such a pair gates
    # on the flat threshold alone, and asymmetrically if only one side is short, so say so.
    gated_keys = {(r[1], r[2], r[3]) for r in rows}
    thin = [a for a in (*ao, *an)
            if (a.get("decoder"), a.get("scenario"), a.get("arm")) in gated_keys
            and (a.get("spread_pct") is None or (a.get("runs") or 1) < 5)]
    if rows and thin:
        print(f"NOTE: a snapshot has no usable per-arm noise (archived, or --repeat < 5) -- gating "
              f"on the flat {t:.1f}% threshold alone, which is not reliable. Re-snapshot both sides "
              f"with `bench.py run --repeat {DEFAULT_REPEAT}`.")
    if regr:
        print(f"\nFAIL: {len(regr)} GB/s regression(s) exceed {t:.1f}%")
        sys.exit(1)
    if compile_fail_line:
        print(f"\n{compile_fail_line}")
        sys.exit(1)
    # The gate's one-line verdict must say when half of it did not run: an "OK" over a snapshot
    # pair that could not compare compile cost is not the promise the CHANGELOG makes.
    tail_note = f" (compile cost NOT gated: {compile_ungated})" if compile_ungated else ""
    if muted:
        print(f"\nOK: no gated regression -- but {len(muted)} arm(s) moved beyond {t:.1f}% and were "
              f"muted by their own noise (listed above). Those arms cannot resolve a change that "
              f"size; a real regression there would look the same.{tail_note}")
    else:
        print(f"\nOK: no GB/s regression beyond {t:.1f}%{tail_note}")


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
    """Build+snapshot two git refs (baseline, then variant) in the same build dir and diff them on GB/s
    via diff() -- measured throughput, the real-performance signal. The two are independent builds with
    different code placement, so a sub-~10% GB/s delta can be layout/frequency noise (the diff threshold
    defaults to that floor); keep the box quiesced and pinned, and for a change in the 2-9% band read the
    deterministic ins/B column or re-run the PRETTY bench and read its within-run `vs <baseline>` verdict.
    Refuses to run on a dirty working tree (it checks out refs) and always restores the original ref."""
    if args.repeat < 1:
        sys.exit("experiment: --repeat must be >= 1")
    # Check before measuring: a filtered snapshot cannot be gated, and finding that out after ten
    # runs across two checkouts wastes tens of minutes.
    if os.environ.get("RAPIDPROTO_BENCH_ONLY"):
        sys.exit("experiment: RAPIDPROTO_BENCH_ONLY is set, so the snapshots would cover only part "
                 "of the suite and could not be diffed; unset it")
    if args.threshold < 0:
        sys.exit("experiment: --threshold must be >= 0")
    if subprocess.check_output(["git", "-C", REPO, "status", "--porcelain"], text=True).strip():
        sys.exit("experiment: working tree is dirty -- commit or stash first (this checks out refs and "
                 "always restores, but refuses to risk uncommitted work)")

    compile_cases, compile_skipped = compile_preflight(not args.no_compile)

    original = current_ref()
    snapdir = os.path.join(REPO, "bench_snapshots")

    def resolve(ref):  # pin to an immutable sha BEFORE any checkout -- HEAD-relative refs (HEAD, HEAD^)
        try:  # would otherwise shift as we move HEAD, silently comparing a ref against itself
            return subprocess.check_output(
                ["git", "-C", REPO, "rev-parse", "--verify", "-q", f"{ref}^{{commit}}"], text=True).strip()
        except subprocess.CalledProcessError:
            sys.exit(f"experiment: '{ref}' is not a valid git ref")

    baseline_sha = resolve(args.baseline)
    variant_sha = resolve(args.variant or "HEAD")

    def snapshot_ref(sha, ref, name):
        print(f"\n=== {name}: {ref} ({sha[:9]}) ===", file=sys.stderr)
        subprocess.check_call(["git", "-C", REPO, "checkout", "-q", sha])
        records, pv = build_and_run(args.build_dir, args.core, args.repeat)
        if not any(r.get("rec") == "arm" for r in records):
            sys.exit(f"experiment: ref '{ref}' emitted no NDJSON arm records -- it likely predates the "
                     "machine-readable bench harness; both refs must be able to emit NDJSON")
        extra_header = {}
        if compile_cases is not None:
            # The checked-out ref's OWN generator and headers: rapidprotoc is rebuilt in this
            # build dir per ref, so the compile diff below measures the change's codegen cost.
            # Degrade, never abort: a ref predating the current generated-code shape (the per-
            # model roots, say) makes the invoker's sweep refuse -- that must not throw away the
            # bench run that just finished. diff_compile reports the resulting one-sided
            # snapshot as "compile cost NOT gated" rather than failing.
            try:
                compile_records, extra_header = collect_compile(
                    args.build_dir, compile_cases, compile_skipped)
                records = records + compile_records
            except (SystemExit, subprocess.CalledProcessError) as e:
                print(f"WARNING: compile sweep unavailable at ref '{ref}' ({e}) -- compile cost "
                      f"not embedded for this snapshot", file=sys.stderr)
                extra_header = {}
        return write_snapshot(records, pv, args.build_dir, args.core,
                              os.path.join(snapdir, f"exp-{name}.ndjson"), extra_header)

    try:
        base_snap = snapshot_ref(baseline_sha, args.baseline, "baseline")
        var_snap = snapshot_ref(variant_sha, args.variant or "HEAD", "variant")
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
    r.add_argument("--build-dir", default=os.path.join(REPO, "build", "gcc-pb25"),
                   help="cmake build dir (default build/gcc-pb25; no preset creates it -- see "
                        "docs/benchmarks.md)")
    r.add_argument("--core", default="2", help="taskset core to pin to, or 'none' to skip pinning (default 2)")
    r.add_argument("--out", default=None, help="snapshot path (default bench_snapshots/<cc>-<rev>.ndjson)")
    r.add_argument("--repeat", type=int, default=DEFAULT_REPEAT,
                   help=f"runs per snapshot, median run kept per arm (default {DEFAULT_REPEAT}; "
                        f"below {DEFAULT_REPEAT} there is no usable per-arm noise and the gate falls "
                        f"back to the flat threshold)")
    r.add_argument("--no-compile", action="store_true",
                   help="skip the embedded compile-cost sweep (~2 min; see tests/compile_bench.py)")
    r.set_defaults(func=run)

    t = sub.add_parser("table", help="render one snapshot, or compare several")
    t.add_argument("snapshots", nargs="+")
    t.set_defaults(func=table)

    d = sub.add_parser("diff", help="GB/s regression check between two snapshots (exit 1 on regression)")
    d.add_argument("old")
    d.add_argument("new")
    d.add_argument("--threshold", type=float, default=10.0,
                   help="regression threshold in %% GB/s (default 10.0 = the cross-build placement-noise "
                        "floor; the gate uses the LARGER of this and the arm's own measured spread, "
                        "and sub-floor changes are not reliably gateable -- see ins/B)")
    d.set_defaults(func=diff)

    e = sub.add_parser("experiment", help="build+snapshot two git refs and diff them on GB/s")
    e.add_argument("baseline", help="git ref for the baseline (built and snapshotted first)")
    e.add_argument("variant", nargs="?", default=None, help="git ref for the variant (default: current HEAD)")
    e.add_argument("--build-dir", default=os.path.join(REPO, "build", "gcc-pb25"),
                   help="cmake build dir (default build/gcc-pb25; no preset creates it)")
    e.add_argument("--core", default="2", help="taskset core, or 'none' to skip pinning (default 2)")
    e.add_argument("--repeat", type=int, default=DEFAULT_REPEAT,
                   help=f"runs per snapshot, median run kept per arm (default {DEFAULT_REPEAT})")
    e.add_argument("--threshold", type=float, default=10.0,
                   help="regression threshold in %% GB/s (default 10.0 = the cross-build placement-noise floor)")
    e.add_argument("--no-compile", action="store_true",
                   help="skip the embedded compile-cost sweep on both refs")
    e.set_defaults(func=experiment)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
