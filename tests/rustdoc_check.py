#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Validate rendered rustdoc: every internal link must resolve, including its #fragment.

Complements `RUSTDOCFLAGS=-D warnings`, which lets links to `#[doc(hidden)]` items through. Such a link
renders as literal `[name]`, as a dead `href="crate::name"`, or, for a hidden method on a visible type,
as a dead anchor. Links to source pages are skipped, because their line-range fragments are not ids.

Usage: rustdoc_check.py DOC_DIR CRATE...
"""

import os
import re
import sys
from html.parser import HTMLParser

EXTERNAL = re.compile(r"[a-z][a-z0-9+.-]*:(//|[^:])")
LITERAL_LINK = re.compile(r"\[[\w:!()<>]+\]")
# `ProtoBytes` shows `[u8]`'s methods with the standard library's docs: not ours, and nightly changes them.
STD_SECTION = "deref-methods-%5Bu8%5D"


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__()
        self.ids = set()
        self.hrefs = []
        self.in_std_section = False
        self.docblock_divs = 0
        self.pre = 0
        self.code = 0
        self.doc_text = []
        with open(path, encoding="utf-8", errors="replace") as f:
            self.feed(f.read())
        self.literal_links = LITERAL_LINK.findall("".join(self.doc_text))

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        # Anchors in the standard library's section stay valid targets: the sidebar links to them.
        if "id" in attrs:
            self.ids.add(attrs["id"])
        if tag == "h2":
            self.in_std_section = attrs.get("id") == STD_SECTION
        if self.in_std_section:
            return
        if tag == "a" and "href" in attrs:
            self.hrefs.append(attrs["href"])
        elif tag == "div" and (self.docblock_divs or "docblock" in (attrs.get("class") or "").split()):
            self.docblock_divs += 1
        elif tag == "pre":
            self.pre += 1
        elif tag == "code":
            self.code += 1

    def handle_endtag(self, tag):
        if self.in_std_section:
            return
        if tag == "div" and self.docblock_divs:
            self.docblock_divs -= 1
        elif tag == "pre" and self.pre:
            self.pre -= 1
        elif tag == "code" and self.code:
            self.code -= 1

    def handle_data(self, data):
        if self.in_std_section or not self.docblock_divs or self.pre:
            return
        # Brackets inside `code` are real, as in `[T]`. Those around it are a link that did not resolve.
        self.doc_text.append(data.replace("[", "(").replace("]", ")") if self.code else data)


def main(doc_dir, crates):
    pages = {}

    def page(path):
        if path not in pages:
            pages[path] = Page(path)
        return pages[path]

    source_dir = os.path.join(os.path.normpath(doc_dir), "src") + os.sep
    errors = []
    for crate in crates:
        paths = [
            os.path.normpath(os.path.join(root, name))
            for root, _, names in os.walk(os.path.join(doc_dir, crate))
            for name in names
            if name.endswith(".html")
        ]
        # A wrong path must fail, or this check would silently stop checking anything.
        if not paths:
            errors.append(f"{crate}: no rendered pages under {doc_dir}")
        for path in sorted(paths):
            current = page(path)
            for link in current.literal_links:
                errors.append(f"{path}: unresolved link {link}")
            for href in current.hrefs:
                target, _, fragment = href.split("?")[0].partition("#")
                if EXTERNAL.match(target):
                    continue
                file = os.path.normpath(os.path.join(os.path.dirname(path), target)) if target else path
                if file.startswith(source_dir):
                    continue
                if not os.path.exists(file):
                    errors.append(f"{path}: dead link {href}")
                elif fragment and file.endswith(".html") and fragment not in page(file).ids:
                    errors.append(f"{path}: dead anchor {href}")

    for error in errors:
        print(error)
    print(f"{len(pages)} pages, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2:]))
