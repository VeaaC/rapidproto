#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Distill docs/ladder/ladder-data.json from the optimization-ladder campaign snapshots
(docs/optimizations.md). Run from the repo root; needs bench_snapshots/ladder-R*.ndjson, the
local (gitignored) timing runs of the article's nine builds -- so this reruns only with a new
campaign. The committed json is the source docs/ladder charts render from (ladder_charts.py).

Output: docs/ladder/ladder-data.json
  { "rungs": [names...], "panels": { panel: [ {id, family, ratios[9], thresh} ... ] } }
ratio = generated arm / reference arm from the SAME snapshot (arena-warm/protoc for arena,
generated/protozero for streaming); thresh = the scenario's own reference-arm drift across all
nine builds (max/min - 1), floored at 3%.
"""
import json
import glob

ARENA = [  # (scenario, family) — fixed article order
    ("rv fx1 1M", "packed varint sweeps"), ("rv fx3 1M", "packed varint sweeps"),
    ("rv fx6 1M", "packed varint sweeps"), ("rv fx10 1M", "packed varint sweeps"),
    ("rv mix12 1M", "packed varint sweeps"), ("rv mix13 1M", "packed varint sweeps"),
    ("rv skew 1M", "packed varint sweeps"), ("rv unif 1M", "packed varint sweeps"),
    ("rv-zz fx1 1M", "zigzag + enum"), ("rv-zz fx5 1M", "zigzag + enum"),
    ("rv-zz mix13 1M", "zigzag + enum"), ("rv-enum fx1 1M", "zigzag + enum"),
    ("rv-enum mix13 1M", "zigzag + enum"),
    ("packed int64(varint)", "packed arrays"), ("packed double(fixed)", "packed arrays"),
    ("scalar records (dispatch-bound)", "records"), ("few msgs, big arrays", "records"),
    ("many msgs, tiny arrays", "records"),
    ("Dataset", "real-world"), ("osm_blocks", "real-world"),
    ("osm: sint64 deltas + enum", "real-world"),
    ("google_message1", "protobuf corpus"), ("google_message2", "protobuf corpus"),
]
STREAM = [
    ("varint-1byte", "varint micro"), ("varint-multibyte", "varint micro"),
    ("zigzag-sint64", "varint micro"), ("packed-int32", "varint micro"),
    ("fixed32-float", "fixed + string"), ("fixed64-double", "fixed + string"),
    ("len-string", "fixed + string"), ("mixed", "fixed + string"),
    ("multibyte-tag", "dispatch + skip"), ("nested-msg", "dispatch + skip"),
    ("skip-heavy", "dispatch + skip"), ("sparse-skip", "dispatch + skip"),
    ("rv fx1 1M", "sweeps"), ("rv fx3 1M", "sweeps"), ("rv fx6 1M", "sweeps"),
    ("rv fx10 1M", "sweeps"), ("rv mix12 1M", "sweeps"), ("rv mix13 1M", "sweeps"),
    ("rv skew 1M", "sweeps"), ("rv unif 1M", "sweeps"),
]
RUNGS = [f"R{i}" for i in range(9)]
RUNG_TITLES = [
    "the baseline", "borrowed strings", "memory layout", "packed pre-sizing",
    "forced inlining", "fused tag reads", "SWAR varint kernels", "the one-byte peek hub",
    "field-order threading"]

snaps = {}
for f in glob.glob("bench_snapshots/ladder-R*.ndjson"):
    name = f.split("ladder-")[1].split(".")[0]
    rows = {}
    for l in open(f):
        r = json.loads(l)
        if r.get("rec") == "arm":
            rows[(r["decoder"], r["scenario"], r["arm"])] = r["gb_s"]
    snaps[name] = rows

def series(dec, scen, gen_arm, ref_arm):
    ratios, refs = [], []
    for rg in RUNGS:
        g = snaps[rg].get((dec, scen, gen_arm))
        p = snaps[rg].get((dec, scen, ref_arm))
        if g is None or p is None:
            return None
        ratios.append(g / p)
        refs.append(p)
    drift = max(refs) / min(refs) - 1
    return {"ratios": [round(x, 4) for x in ratios],
            "thresh": round(max(drift, 0.03), 4)}

out = {"rungs": RUNGS, "titles": RUNG_TITLES, "panels": {}}
for panel, picks, dec, gen_arm, ref in (
        ("arena", ARENA, "arena", "arena-warm", "protoc"),
        ("streaming", STREAM, "stream", "generated", "protozero")):
    rows = []
    for scen, fam in picks:
        s = series(dec, scen, gen_arm, ref)
        if s is None:
            print(f"MISSING: {panel} {scen}")
            continue
        rows.append({"id": scen, "family": fam, **s})
    out["panels"][panel] = rows
json.dump(out, open("docs/ladder/ladder-data.json", "w"), indent=1)
n = sum(len(v) for v in out["panels"].values())
print(f"wrote ladder-data.json: {n} bars")
# quick sanity: R8 medians + biggest per-rung movers
for panel, rows in out["panels"].items():
    r8 = sorted(r["ratios"][8] for r in rows)
    print(f"{panel}: R8 median {r8[len(r8)//2]:.2f}x  min {r8[0]:.2f}x  max {r8[-1]:.2f}x")
for i in range(1, 9):
    best = max((r["ratios"][i] / r["ratios"][i - 1], p, r["id"])
               for p, rows in out["panels"].items() for r in rows)
    print(f"{RUNGS[i]} {RUNG_TITLES[i]:24} biggest: {best[0]:.2f}x  {best[1]} {best[2]}")
