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
# `PackedSlice` shows `[T]`'s methods with the standard library's docs: not ours, and nightly changes them.
STD_SECTION = re.compile(r'<h2 id="deref-methods-%5BT%5D".*?(?=<h2 id=")', re.DOTALL)


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__()
        self.ids = set()
        self.hrefs = []
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        self.literal_link = "[<code>" in STD_SECTION.sub("", text)
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.ids.add(attrs["id"])
        if tag == "a" and "href" in attrs:
            self.hrefs.append(attrs["href"])


def main(doc_dir, crates):
    pages = {}

    def page(path):
        if path not in pages:
            pages[path] = Page(path)
        return pages[path]

    errors = []
    for crate in crates:
        paths = [
            os.path.join(root, name)
            for root, _, names in os.walk(os.path.join(doc_dir, crate))
            for name in names
            if name.endswith(".html")
        ]
        # A wrong path must fail, or this check would silently stop checking anything.
        if not paths:
            errors.append(f"{crate}: no rendered pages under {doc_dir}")
        for path in sorted(paths):
            current = page(path)
            if current.literal_link:
                errors.append(f"{path}: literal [`name`], an unresolved link")
            for href in current.hrefs:
                target, _, fragment = href.split("?")[0].partition("#")
                if EXTERNAL.match(target) or "/src/" in target or target.startswith("src/"):
                    continue
                file = os.path.normpath(os.path.join(os.path.dirname(path), target)) if target else path
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
