#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Validate the markdown docs' relative links: every target file must exist, and every #fragment must
match a GitHub-style anchor of a heading in the target. The docs are split across README / docs/ /
architecture.md with heavy cross-linking, so a renamed heading or a moved page rots silently without
a mechanical check. External (http/https/mailto) links are not checked -- this is a structure check,
not a reachability probe."""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def doc_files():
    return sorted(
        glob.glob(os.path.join(ROOT, "*.md"))
        + glob.glob(os.path.join(ROOT, "docs", "*.md"))
        + glob.glob(os.path.join(ROOT, "examples", "**", "*.md"), recursive=True)
    )


def strip_fenced(text):
    """Drop fenced code blocks: a '# comment' line inside one is not a heading. An unterminated
    trailing fence is stripped to end-of-file rather than left behind."""
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    return re.sub(r"```.*", "", text, flags=re.S)


def strip_code(text):
    """Additionally drop inline code spans (which may wrap across lines): lambda syntax like
    `[](Tag, T v){}` is not a markdown link. Only for link extraction -- headings keep their span
    TEXT in the anchor, so anchors_of must not use this."""
    return re.sub(r"`[^`]*`", "", strip_fenced(text))


def github_anchor(heading):
    """GitHub's anchor algorithm: strip formatting, lowercase, drop punctuation (word chars, spaces,
    and hyphens survive), spaces -> hyphens."""
    h = re.sub(r"[`*]", "", heading).strip().lower()
    h = re.sub(r"[^\w\- ]", "", h)
    return h.replace(" ", "-")


def anchors_of(path, cache={}):
    if path not in cache:
        with open(path, encoding="utf-8") as f:
            text = strip_fenced(f.read())
        seen, out = {}, set()
        for m in re.finditer(r"^#{1,6}\s+(.*)$", text, flags=re.M):
            a = github_anchor(m.group(1))
            n = seen.get(a, 0)
            seen[a] = n + 1
            out.add(a if n == 0 else f"{a}-{n}")  # GitHub dedups repeated headings with -1, -2, ...
        cache[path] = out
    return cache[path]


def main():
    errors = 0
    files = doc_files()
    # A zero-file run is a broken invocation, not a pass: the gate stage would report success
    # having checked nothing (the same case tests/check_fixture_coverage.sh guards).
    if not files:
        print(">> no documentation files found -- check_doc_links.py is looking in the wrong place")
        return 1
    for path in files:
        rel = os.path.relpath(path, ROOT)
        base = os.path.dirname(path)
        with open(path, encoding="utf-8") as f:
            text = strip_code(f.read())
        for m in re.finditer(r"\]\(([^)\s]+)\)", text):
            target = m.group(1)
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            if target.startswith("#"):
                tfile, frag = path, target[1:]
            elif "#" in target:
                tpath, frag = target.split("#", 1)
                tfile = os.path.normpath(os.path.join(base, tpath))
            else:
                tfile, frag = os.path.normpath(os.path.join(base, target)), None
            if not os.path.exists(tfile):
                print(f">> {rel}: broken link ({target}): no such file")
                errors += 1
            elif frag is not None and tfile.endswith(".md") and frag not in anchors_of(tfile):
                print(f">> {rel}: broken link ({target}): no such anchor")
                errors += 1
    if errors:
        return 1
    print(f"doc links ok ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
