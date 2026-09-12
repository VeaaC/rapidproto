#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Render docs/optimizations.md's explanatory diagrams (docs/ladder/diagram-*.svg). Run from
the repo root. Same palette and type as ladder_charts.py so charts and diagrams read as one
family."""

SURF, INK, INK2 = "#fcfcfb", "#0b0b0b", "#52514e"
GRID, BASE, GAIN, LOSS = "#e7e6e3", "#d9d8d5", "#2a78d6", "#eb6834"

STYLE = (f'<style>.t{{font-size:14.5px;font-weight:600;fill:{INK}}}'
         f'.a{{font-size:11px;fill:{INK2}}}.g{{font-size:10px;fill:{INK2}}}'
         f'.m{{font-size:11px;font-family:ui-monospace,monospace;fill:{INK}}}'
         f'.mg{{font-size:10px;font-family:ui-monospace,monospace;fill:{INK2}}}</style>')
ARROW = (f'<defs><marker id="ah" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" '
         f'markerHeight="7" orient="auto-start-reverse">'
         f'<path d="M0 0 L8 4 L0 8 z" fill="{INK2}"/></marker>'
         f'<marker id="ahb" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" '
         f'markerHeight="7" orient="auto-start-reverse">'
         f'<path d="M0 0 L8 4 L0 8 z" fill="{GAIN}"/></marker></defs>')


def svg(w, h, body, out):
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
         f'font-family="system-ui,sans-serif">',
         f'<rect width="{w}" height="{h}" fill="{SURF}"/>', STYLE, ARROW]
    o += body
    o.append('</svg>')
    out = "docs/ladder/" + out
    open(out, "w").write("\n".join(o))
    print("wrote", out)


def box(x, y, w, h, fill="none", stroke=INK2, dash=""):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="1" rx="3"{d}/>')


def txt(x, y, s, cls="a", anchor="middle"):
    return f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}">{s}</text>'


def arrow(x1, y1, x2, y2, color=INK2, dash="", marker="ah"):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" '
            f'stroke-width="1.3"{d} marker-end="url(#{marker})"/>')


def path_arrow(d, color=INK2, dash="", marker="ah"):
    dd = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<path d="{d}" fill="none" stroke="{color}" stroke-width="1.3"{dd} '
            f'marker-end="url(#{marker})"/>')


# ---------------------------------------------------------------- borrowed strings
def strings():
    o = []
    for px, cap, borrowed in ((40, "baseline: copy", False),
                              (460, "step 1: borrow", True)):
        pw = 380
        o.append(txt(px + pw / 2, 30, cap, "t"))
        # input buffer with a string span inside
        o.append(box(px, 52, pw, 30))
        o.append(txt(px + 4, 46, "input buffer", "g", "start"))
        sx, sw = px + 150, 120
        o.append(box(sx, 52, sw, 30, BASE))
        o.append(txt(sx + sw / 2, 71, '"/index.html"', "m"))
        # arena holds the message struct itself (and, in the baseline, the copied bytes)
        o.append(box(px, 128, pw, 108))
        o.append(txt(px + 4, 122, "arena", "g", "start"))
        fx, fw = px + 110, 160
        o.append(box(fx, 188, fw, 32))
        o.append(txt(fx + fw / 2, 208, "msg.path", "m"))
        if borrowed:
            o.append(txt(px + pw / 2, 158, "no string copy, no allocation", "g"))
            # the field points straight into the input buffer
            o.append(path_arrow(f'M {fx + fw / 2} 188 C {fx + fw / 2} 140, '
                                f'{sx + sw / 2} 130, {sx + sw / 2} 84'))
            o.append(txt(fx + fw + 8, 208, "{pointer,", "mg", "start"))
            o.append(txt(fx + fw + 8, 221, " length}", "mg", "start"))
        else:
            cx, cw = px + 140, 120
            o.append(box(cx, 140, cw, 30, BASE))
            o.append(txt(cx + cw / 2, 159, '"/index.html"', "m"))
            o.append(arrow(sx + sw / 2, 84, cx + cw / 2, 138))
            o.append(txt(sx + sw / 2 + 10, 110, "alloc + memcpy", "g", "start"))
            o.append(arrow(fx + fw / 2, 188, cx + cw / 2, 172))
    o.append(f'<line x1="440" y1="20" x2="440" y2="240" stroke="{GRID}"/>')
    svg(880, 252, o, "diagram-strings.svg")


# ---------------------------------------------------------------- packed pre-sizing
def presize():
    o = []
    # the wire span
    o.append(txt(60, 34, "wire", "g", "start"))
    o.append(box(60, 42, 44, 28, BASE))
    o.append(txt(82, 60, "tag", "m"))
    o.append(box(106, 42, 80, 28, BASE))
    o.append(txt(146, 60, "len 190", "m"))
    o.append(box(188, 42, 560, 28))
    o.append(txt(468, 60, "87 varints in 190 bytes", "m"))
    # baseline: geometric growth
    o.append(txt(60, 118, "baseline: grow as elements arrive", "a", "start"))
    gx = 60
    for cap in (4, 8, 16, 32):
        w = cap * 4
        o.append(box(gx, 128, w, 26))
        o.append(txt(gx + w / 2, 145, str(cap), "mg"))
        o.append(arrow(gx + w + 4, 141, gx + w + 26, 141))
        gx += w + 30
    o.append(txt(gx + 4, 145, "&#8230; allocate &#215;2, copy everything, repeat", "a", "start"))
    # step: allocate the bound once
    o.append(txt(60, 208, "step 2: len bounds the count - allocate 190 slots once, "
                          "decode into place", "a", "start"))
    ax, aw = 188, 560
    used = aw * 87 / 190
    o.append(box(ax, 218, aw, 28))
    o.append(f'<rect x="{ax}" y="218" width="{used:.0f}" height="28" fill="{GAIN}" '
             f'fill-opacity="0.25" stroke="none"/>')
    o.append(f'<line x1="{ax + used:.0f}" y1="218" x2="{ax + used:.0f}" y2="246" '
             f'stroke="{INK2}" stroke-width="1"/>')
    o.append(txt(ax + used / 2, 236, "87 decoded", "m"))
    o.append(txt(ax + used + (aw - used) / 2, 236, "unused tail", "mg"))
    o.append(txt(ax + used + (aw - used) / 2, 268,
                 "returned to the arena: a pointer subtraction", "g"))
    svg(880, 282, o, "diagram-presize.svg")


# ---------------------------------------------------------------- SWAR kernel
def swar():
    o = []
    bx, bw, gap = 190, 70, 6
    bytes_ = [("96", 1), ("01", 0), ("ac", 1), ("02", 0),
              ("08", 0), ("d2", 1), ("85", 1), ("03", 0)]
    groups = [(0, 2, "150"), (2, 2, "300"), (4, 1, "8"), (5, 3, "49874")]
    X = lambda i: bx + i * (bw + gap)
    # row 1: the loaded word
    o.append(txt(60, 30, "one 64-bit load", "g", "start"))
    o.append(txt(60, 66, "wire bytes", "a", "start"))
    for i, (hx, cont) in enumerate(bytes_):
        x = X(i)
        o.append(box(x, 44, bw, 32))
        o.append(f'<rect x="{x + 1}" y="45" width="10" height="30" '
                 f'fill="{LOSS if cont else GAIN}" fill-opacity="0.85" stroke="none"/>')
        o.append(txt(x + 12 + (bw - 12) / 2, 64, "0x" + hx, "m"))
    o.append(f'<rect x="380" y="22" width="9" height="9" fill="{LOSS}"/>')
    o.append(txt(394, 30, "high bit set: value continues", "g", "start"))
    o.append(f'<rect x="610" y="22" width="9" height="9" fill="{GAIN}"/>')
    o.append(txt(624, 30, "high bit clear: value ends", "g", "start"))
    # row 2: continuation mask
    o.append(txt(60, 152, "continuation mask", "a", "start"))
    o.append(txt(60, 166, "one multiply, one shift", "g", "start"))
    for i, (hx, cont) in enumerate(bytes_):
        x = X(i) + bw / 2 - 11
        o.append(box(x, 138, 22, 22, BASE if cont else "none"))
        o.append(txt(x + 11, 153, str(cont), "m"))
        o.append(arrow(X(i) + bw / 2, 80, X(i) + bw / 2, 134, GRID.replace(GRID, INK2),
                       dash="2 3"))
    # row 3: compacted values
    o.append(txt(60, 222, "decoded values", "a", "start"))
    o.append(txt(60, 236, "3 shift-and-mask rounds", "g", "start"))
    for start, n, val in groups:
        x0, x1 = X(start), X(start + n - 1) + bw
        o.append(box(x0, 208, x1 - x0, 30, GAIN))
        o.append(f'<text x="{(x0 + x1) / 2}" y="227" text-anchor="middle" fill="#ffffff" '
                 f'font-size="11" font-family="ui-monospace,monospace">{val}</text>')
        o.append(arrow((x0 + x1) / 2, 164, (x0 + x1) / 2, 204, dash="2 3"))
    o.append(txt(bx, 266, "one branch per 64-bit word replaces one branch per byte",
                 "a", "start"))
    svg(880, 280, o, "diagram-swar.svg")


# ---------------------------------------------------------------- one-byte peek hub
def hub():
    o = []
    # peek node
    o.append(box(60, 96, 170, 40))
    o.append(txt(145, 116, "peek one byte", "a"))
    o.append(txt(145, 130, "(no bounds re-check)", "g"))
    # field labels
    fields = [("0x0a", "field 1: name", 44), ("0x10", "field 2: id", 104),
              ("0x1a", "field 3: tags", 164)]
    for tag, label, y in fields:
        o.append(box(420, y, 210, 36))
        o.append(txt(525, y + 22, label, "m"))
        o.append(path_arrow(f'M 230 116 C 320 116, 330 {y + 18}, 416 {y + 18}'))
        o.append(txt(330, y + 12, tag, "mg"))
    # fallback
    o.append(box(420, 228, 300, 36, dash="4 3"))
    o.append(txt(570, 250, "general fused tag read (step 4)", "a"))
    o.append(path_arrow('M 145 136 C 145 246, 260 246, 416 246', dash="4 3"))
    o.append(txt(240, 234, "anything else: higher fields,", "g", "start"))
    o.append(txt(240, 268, "unknown, non-minimal", "g", "start"))
    o.append(txt(660, 62, "each tag byte jumps straight", "g", "start"))
    o.append(txt(660, 76, "to that field&#8217;s label", "g", "start"))
    svg(880, 288, o, "diagram-hub.svg")


# ---------------------------------------------------------------- field-order threading
def threading():
    o = []
    fields = [(70, "field 1: name"), (350, "field 2: id"), (630, "field 3: tags")]
    for x, label in fields:
        o.append(box(x, 60, 180, 40))
        o.append(txt(x + 90, 78, label, "m"))
        o.append(txt(x + 90, 93, "decode, then:", "g"))
    # predicted chain: each label probes the next two expected tags
    o.append(arrow(250, 80, 346, 80, GAIN, marker="ahb"))
    o.append(txt(298, 70, "next == 0x10?", "mg"))
    o.append(path_arrow('M 160 60 C 250 8, 620 8, 696 56', GAIN, marker="ahb"))
    o.append(txt(430, 22, "or == 0x1a?", "mg"))
    o.append(arrow(530, 80, 626, 80, GAIN, marker="ahb"))
    o.append(txt(578, 70, "next == 0x1a?", "mg"))
    # repeated self-check on field 3
    o.append(path_arrow('M 810 68 C 856 44, 856 96, 814 90', GAIN, marker="ahb"))
    o.append(txt(874, 36, "another", "mg", "end"))
    o.append(txt(874, 49, "element?", "mg", "end"))
    # dispatch fallback
    o.append(box(330, 180, 220, 40, dash="4 3"))
    o.append(txt(440, 204, "dispatch switch (step 6)", "a"))
    for x, _ in fields:
        o.append(path_arrow(f'M {x + 90} 100 C {x + 90} 150, 440 130, 440 176',
                            dash="4 3"))
    o.append(txt(96, 160, "miss: fall back", "g", "start"))
    o.append(txt(70, 250, "each label probes the next two expected tags; on an in-order "
                          "wire the decoder runs down the blue chain of predictable "
                          "branches", "a", "start"))
    o.append(txt(70, 266, "and the switch only catches the exceptions", "a", "start"))
    svg(880, 280, o, "diagram-threading.svg")


strings()
presize()
swar()
hub()
threading()
