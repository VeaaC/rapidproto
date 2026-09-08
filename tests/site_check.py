#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Validate the RENDERED GitHub Pages site: every internal URL in the built HTML -- links,
stylesheets, scripts, and images -- must resolve to a file in the site tree, every #fragment must
match an id in the target page, and every site-absolute URL must carry the project-site base path.

Complements tests/check_doc_links.py, which checks the same links in the markdown SOURCE against
GitHub's anchor algorithm. The rendered site goes through a different pipeline -- kramdown
generates the heading ids, jekyll-relative-links rewrites the .md hrefs, jekyll-readme-index moves
READMEs to directory indexes, the layout emits its own asset URLs, and everything is served under
the baseurl -- and each of those steps can silently disagree with how github.com renders the same
markdown. This checks the output of that pipeline, so it must run AFTER the Jekyll build (see
.github/workflows/pages.yml).

Failure modes this exists to catch:
  - an internal href still ending in .md (jekyll-relative-links did not rewrite it -> 404)
  - a kramdown heading id that differs from the GitHub slug the source links were written for
  - a site-absolute URL missing the baseurl (the classic project-site breakage: layout assets or
    rewritten links resolving on <user>.github.io/ instead of <user>.github.io/<repo>/)
  - a layout-emitted asset the site does not ship (a favicon or stylesheet 404 on every page)
  - a rendered page the sidebar does not list (_data/nav.yml drifted from the staged set)

Usage: python3 tests/site_check.py <site-dir> --baseurl /rapidproto
"""
import argparse
import os
import re
import sys
import urllib.parse
from html.parser import HTMLParser

# Anything with a URI scheme (https:, mailto:, tel:, data:, ...) or protocol-relative (//host/...)
# is external: this is a structure check on the site's own tree, not a reachability probe --
# same stance as check_doc_links.py.
EXTERNAL = re.compile(r"^([A-Za-z][A-Za-z0-9+.\-]*:|//)")


class PageScan(HTMLParser):
    """Collect the ids a page defines, the URLs it references (links and page assets), and the
    subset of links inside the sidebar's <ul class="nav-list"> -- the nav-list scope (not the
    whole <nav>) so the site-title logo link cannot stand in for a missing nav.yml entry."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids = set()
        self.refs = []
        self.nav_refs = []
        self._in_navlist = 0

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "ul" and (self._in_navlist or "nav-list" in a.get("class", "").split()):
            self._in_navlist += 1  # count nested uls too, so their close does not end the scope
        if "id" in a:
            self.ids.add(a["id"])
        if tag == "a" and "name" in a:  # legacy anchors count as targets too
            self.ids.add(a["name"])
        for attr in ("href",) if tag in ("a", "link") else ("src",) if tag in ("script", "img") else ():
            if attr in a:
                self.refs.append(a[attr])
                if tag == "a" and self._in_navlist:
                    self.nav_refs.append(a[attr])

    def handle_endtag(self, tag):
        if tag == "ul" and self._in_navlist:
            self._in_navlist -= 1


def scan(path):
    p = PageScan()
    with open(path, encoding="utf-8") as f:
        p.feed(f.read())
    return p.ids, p.refs, p.nav_refs


# Rendered pages (site-relative) that may stay off the sidebar: the license notices, and the
# manual's own index (docs/README.md -- github.com's view of the manual, reachable here via the
# README's docs/ link; ON the site the sidebar is that index). Everything else must be navigable
# -- a published-but-unreachable page is the drift this rule exists to catch.
NAV_EXEMPT = {"THIRD_PARTY_NOTICES.html", "docs/index.html"}


def resolve(site, baseurl, page_rel, ref):
    """Map an internal URL to (site-relative file path, fragment). Returns (None, reason) when
    the URL is malformed for this site."""
    target, _, frag = ref.partition("#")
    target = urllib.parse.unquote(target.partition("?")[0])  # servers resolve the decoded path
    if not target:  # same-page fragment (or a bare ?query)
        return page_rel, frag
    if target.startswith("/"):
        # Site-absolute: must live under the baseurl, else it 404s on github.io/<repo>/.
        if baseurl and not (target == baseurl or target.startswith(baseurl + "/")):
            return None, f"absolute URL escapes baseurl {baseurl!r}"
        target = target[len(baseurl):].lstrip("/") or "."
        base = ""
    else:
        base = os.path.dirname(page_rel)
    path = os.path.normpath(os.path.join(base, target))
    if path == ".." or path.startswith("../"):
        return None, "URL escapes the site root"
    if os.path.isdir(os.path.join(site, path)):
        path = os.path.normpath(os.path.join(path, "index.html"))
    return path, frag


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("site", help="built site directory (_site)")
    ap.add_argument("--baseurl", default="", help="expected base path, e.g. /rapidproto")
    args = ap.parse_args()
    site = args.site
    baseurl = args.baseurl.rstrip("/")

    pages = {}  # site-relative path -> (ids, refs, nav_refs)
    for dirpath, _, files in os.walk(site):
        for name in files:
            if name.endswith(".html"):
                full = os.path.join(dirpath, name)
                pages[os.path.relpath(full, site)] = scan(full)
    if "index.html" not in pages:
        sys.exit(f"site_check: no index.html in {site} -- README.md was not served as the index")

    errors, checked = [], 0
    nav_seen = {}  # page -> frozenset of resolved nav targets, for the consistency check below
    for page_rel, (_, refs, nav_refs) in sorted(pages.items()):
        # Sidebar coverage: every page must be reachable from every page's nav-list (the layout
        # stamps one sidebar everywhere, so a page whose nav lost entries means the layout broke,
        # and a page absent from the shared nav means _data/nav.yml drifted from the staged set).
        nav_targets = {resolve(site, baseurl, page_rel, r)[0] for r in nav_refs if not EXTERNAL.match(r)}
        # Raw anchor count as well as the resolved set: an unclosed tag that folds body links
        # into the nav scope can leak targets the nav already covers, leaving the SET unchanged.
        nav_seen[page_rel] = (len(nav_refs), frozenset(nav_targets))
        for wanted in pages:
            if wanted not in nav_targets and wanted.replace(os.sep, "/") not in NAV_EXEMPT:
                errors.append(f"{page_rel}: sidebar has no link to {wanted} (nav.yml drifted?)")
        for ref in refs:
            if EXTERNAL.match(ref):
                continue
            checked += 1
            path, frag = resolve(site, baseurl, page_rel, ref)
            if path is None:
                errors.append(f"{page_rel}: {ref!r}: {frag}")
                continue
            if path.endswith(".md"):
                errors.append(f"{page_rel}: {ref!r}: unrewritten markdown link (renders as 404)")
                continue
            full = os.path.join(site, path)
            if path in pages:
                ids = pages[path][0]
                if frag and frag not in ids:
                    errors.append(f"{page_rel}: {ref!r}: no id {frag!r} in {path}")
            elif not os.path.isfile(full):
                errors.append(f"{page_rel}: {ref!r}: no file {path} in the site")
            # Static (non-HTML) targets exist but carry no ids; a fragment on one is inert, not
            # broken, so only existence is checked.

    # The sidebar must be IDENTICAL on every page: it is stamped by one layout, so any
    # divergence means broken markup, not intent -- e.g. an unclosed tag making the parser fold
    # body links into one page's nav scope (which coverage alone would happily accept).
    if len(set(nav_seen.values())) > 1:
        smallest = min(nav_seen, key=lambda p: nav_seen[p][0])
        errors.append(f"sidebars differ across pages ({smallest} has {nav_seen[smallest][0]} "
                      "nav links) -- the layout or its markup broke")

    # Anti-vacuity: an empty or wrongly-pathed site must not pass as a clean one. The floors sit
    # well under today's real size (12 pages, ~250 internal URLs) but far above anything a broken
    # stage/build could produce. Same rationale as check_doc_links.py's link floor.
    if not errors and (len(pages) < 10 or checked < 100):
        errors.append(f"only {len(pages)} pages / {checked} internal URLs -- the site did not fully build")

    if errors:
        print(f"site_check: {len(errors)} problem(s) in the rendered site:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        sys.exit(1)
    print(f"site_check: OK ({len(pages)} pages, {checked} internal URLs checked)")


if __name__ == "__main__":
    main()
