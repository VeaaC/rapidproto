#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Render ../optimizations.md's per-step charts (ladder-step*.svg, next to this script) from
the committed ladder-data.json (extract.py). One SVG
per article step: every benchmark as a bar (speed vs its reference), the part the step changed
colored -- gains blue, losses orange -- and deltas within the scenario's own measured
reference drift left uncolored. Arena bars are vs protoc, streaming bars vs protozero; one
dashed line marks the reference's 1x."""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

BASE, GAIN, LOSS = "#d9d8d5", "#2a78d6", "#eb6834"
INK, INK2, GRID, SURF = "#0b0b0b", "#52514e", "#e7e6e3", "#fcfcfb"

data = json.load(open(sys.argv[1] if len(sys.argv) > 1
                      else os.path.join(HERE, "ladder-data.json")))
RUNGS, TITLES = data["rungs"], data["titles"]
hi_all = max(max(r["ratios"]) for p in data["panels"].values() for r in p)

def render(idx, out, shown=None):
    W, H, ML, MR, MT, MB = 880, 360, 60, 14, 58, 66
    ph = H - MT - MB
    hi = hi_all * 1.05
    def Y(v):  # sqrt scale: keeps sub-1x visible while 6x doesn't crush everything
        return MT + ph - (math.sqrt(v) / math.sqrt(hi)) * ph
    bars = [(p, r) for p in ("arena", "streaming") for r in data["panels"][p]]
    n, pgap, fgap = len(bars), 26, 8
    fams = []
    for p, r in bars:
        if not fams or fams[-1][0] != (p, r["family"]):
            fams.append(((p, r["family"]), 0))
        fams[-1] = (fams[-1][0], fams[-1][1] + 1)
    gaps = pgap + fgap * (len(fams) - 2)
    bw = (W - ML - MR - gaps - (n - 1) * 2) / n
    step = TITLES[idx]
    n = idx if shown is None else shown
    title = ("The baseline: a straightforward, validating decoder" if idx == 0
             else f"Step {n}: {step}")
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'font-family="system-ui,sans-serif">',
         f'<rect width="{W}" height="{H}" fill="{SURF}"/>',
         f'<style>.t{{font-size:14.5px;font-weight:600;fill:{INK}}}'
         f'.a{{font-size:10.5px;fill:{INK2}}}.g{{font-size:10px;fill:{INK2}}}</style>',
         f'<text x="{ML}" y="24" class="t">{title}</text>',
         f'<text x="{ML}" y="40" class="a">decode speed &#8212; arena vs protoc, streaming vs '
         f'protozero. <tspan fill="{GAIN}" font-weight="600">&#9632; gained this step</tspan>'
         f'&#160;&#160;<tspan fill="{LOSS}" font-weight="600">&#9632; lost this step</tspan>'
         f'&#160;&#160;changes within a benchmark&#8217;s own measured noise stay grey</text>']
    for tick in (0.5, 1, 2, 3, 4, 5, 6):
        if tick <= hi:
            y = Y(tick)
            ref = tick == 1
            o.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{W - MR}" y2="{y:.1f}" '
                     f'stroke="{INK2 if ref else GRID}" stroke-width="1"'
                     + (' stroke-dasharray="5 4"' if ref else '') + '/>')
            o.append(f'<text x="{ML - 6}" y="{y + 3.5:.1f}" class="a" text-anchor="end">'
                     + ("1x ref" if ref else f"{tick:g}x") + '</text>')
    x, lastp, lastf = ML, None, None
    fam_x = []
    for p, r in bars:
        if lastp is not None and p != lastp:
            x += pgap
            o.append(f'<line x1="{x - pgap / 2:.1f}" y1="{MT - 6}" x2="{x - pgap / 2:.1f}" '
                     f'y2="{MT + ph}" stroke="{GRID}" stroke-width="1"/>')
        elif lastf is not None and r["family"] != lastf:
            x += fgap
        if r["family"] != lastf or p != lastp:
            fam_x.append([r["family"], x, x])
        c, pv = r["ratios"][idx], (r["ratios"][idx - 1] if idx else r["ratios"][idx])
        lo = min(c, pv)
        sig = abs(c / pv - 1) > r["thresh"] if idx else False
        yb, y0 = Y(lo), MT + ph
        o.append(f'<rect x="{x:.1f}" y="{yb:.1f}" width="{bw:.1f}" '
                 f'height="{y0 - yb:.1f}" fill="{BASE}"/>')
        if sig and c > pv:
            h = yb - Y(c)
            o.append(f'<rect x="{x:.1f}" y="{Y(c):.1f}" width="{bw:.1f}" height="{h:.1f}" '
                     f'fill="{GAIN}"' + (' rx="2"' if h >= 4 else '') + '/>')
        if sig and c < pv:
            h = yb - Y(pv)
            o.append(f'<rect x="{x:.1f}" y="{Y(pv):.1f}" width="{bw:.1f}" height="{h:.1f}" '
                     f'fill="{LOSS}"' + (' rx="2"' if h >= 4 else '') + '/>')
        fam_x[-1][2] = x + bw
        x += bw + 2
        lastp, lastf = p, r["family"]
    for fam, x0, x1 in fam_x:
        words, y = fam.split(), H - MB + 16
        for j, wd in enumerate(words):
            o.append(f'<text x="{(x0 + x1) / 2:.1f}" y="{y + j * 11}" class="g" '
                     f'text-anchor="middle">{wd}</text>')
    na = sum(1 for (p, f), c in fams if p == "arena")  # arena family count
    arena_end = max(x1 for (f, x0, x1) in fam_x[:na])
    stream_start = min(x0 for (f, x0, x1) in fam_x[na:])
    o.append(f'<text x="{(ML + arena_end) / 2:.0f}" y="{MT - 8}" class="g" '
             f'text-anchor="middle">arena model</text>')
    o.append(f'<text x="{(stream_start + W - MR) / 2:.0f}" y="{MT - 8}" class="g" '
             f'text-anchor="middle">streaming model</text>')
    o.append(f'<line x1="{ML}" y1="{MT + ph}" x2="{W - MR}" y2="{MT + ph}" '
             f'stroke="{INK2}" stroke-width="1"/>')
    o.append('</svg>')
    open(out, "w").write("\n".join(o))

# Article numbering: the memory-layout rung (R2) has no section; steps renumber past it.
ARTICLE = [0, 1, 3, 4, 5, 6, 7, 8]  # article step n -> campaign rung index
for n, ri in enumerate(ARTICLE):
    render(ri, os.path.join(HERE, f"ladder-step{n}.svg"), n)
print("wrote ladder-step0..7.svg")
