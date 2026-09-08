#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Validate the RENDERED GitHub Pages site: every internal link in the built HTML must resolve to
a file in the site tree, every #fragment must match an id in the target page, and every rendered
URL must carry the project-site base path.

Complements tests/check_doc_links.py, which checks the same links in the markdown SOURCE against
GitHub's anchor algorithm. The rendered site goes through a different pipeline -- kramdown
generates the heading ids, jekyll-relative-links rewrites the .md hrefs, jekyll-readme-index moves
READMEs to directory indexes, and everything is served under the baseurl -- and each of those
steps can silently disagree with how github.com renders the same markdown. This checks the output
of that pipeline, so it must run AFTER the Jekyll build (see .github/workflows/pages.yml).

Failure modes this exists to catch:
  - an internal href still ending in .md (jekyll-relative-links did not rewrite it -> 404)
  - a kramdown heading id that differs from the GitHub slug the source links were written for
  - a rendered URL missing the baseurl (the classic project-site breakage: links resolve on
    <user>.github.io/ instead of <user>.github.io/<repo>/)

Usage: tests/site_check.py <site-dir> --baseurl /rapidproto
"""
import argparse
import os
import sys
from html.parser import HTMLParser


class PageScan(HTMLParser):
    """Collect the ids defined by a page and the hrefs it links."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids = set()
        self.hrefs = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        if tag == "a":
            if "name" in a:  # legacy anchors count as targets too
                self.ids.add(a["name"])
            if "href" in a:
                self.hrefs.append(a["href"])


def scan(path):
    p = PageScan()
    with open(path, encoding="utf-8") as f:
        p.feed(f.read())
    return p.ids, p.hrefs


def resolve(site, baseurl, page_rel, href):
    """Map an internal href to (site-relative file path, fragment). Returns (None, reason) when
    the href is malformed for this site."""
    target, _, frag = href.partition("#")
    if not target:  # same-page fragment
        return page_rel, frag
    if target.startswith("/"):
        # Site-absolute: must live under the baseurl, else it 404s on github.io/<repo>/.
        if baseurl and not (target == baseurl or target.startswith(baseurl + "/")):
            return None, f"absolute href escapes baseurl {baseurl!r}"
        target = target[len(baseurl):].lstrip("/") or "."
        base = ""
    else:
        base = os.path.dirname(page_rel)
    path = os.path.normpath(os.path.join(base, target))
    if path.startswith(".."):
        return None, "href escapes the site root"
    if os.path.isdir(os.path.join(site, path)):
        path = os.path.join(path, "index.html")
    return path, frag


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("site", help="built site directory (_site)")
    ap.add_argument("--baseurl", default="", help="expected base path, e.g. /rapidproto")
    args = ap.parse_args()
    site = args.site
    baseurl = args.baseurl.rstrip("/")

    pages = {}  # site-relative path -> (ids, hrefs)
    for dirpath, _, files in os.walk(site):
        for name in files:
            if name.endswith(".html"):
                full = os.path.join(dirpath, name)
                pages[os.path.relpath(full, site)] = scan(full)
    if "index.html" not in pages:
        sys.exit(f"site_check: no index.html in {site} -- README.md was not served as the index")

    errors = []
    for page_rel, (_, hrefs) in sorted(pages.items()):
        for href in hrefs:
            scheme = href.split(":", 1)[0].lower() if ":" in href else ""
            if scheme in ("http", "https", "mailto"):
                continue  # external: structure check only, like check_doc_links.py
            path, frag = resolve(site, baseurl, page_rel, href)
            if path is None:
                errors.append(f"{page_rel}: {href!r}: {frag}")
                continue
            if path.endswith(".md"):
                errors.append(f"{page_rel}: {href!r}: unrewritten markdown link (renders as 404)")
                continue
            full = os.path.join(site, path)
            if path in pages:
                if frag and frag not in pages[path][0]:
                    errors.append(f"{page_rel}: {href!r}: no id {frag!r} in {path}")
            elif not os.path.isfile(full):
                errors.append(f"{page_rel}: {href!r}: no file {path} in the site")
            # Static (non-HTML) targets exist but carry no ids; a fragment on one is inert, not
            # broken, so only existence is checked.

    if errors:
        print(f"site_check: {len(errors)} broken link(s) in the rendered site:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        sys.exit(1)
    total = sum(len(h) for _, h in pages.values())
    print(f"site_check: OK ({len(pages)} pages, {total} links checked)")


if __name__ == "__main__":
    main()
