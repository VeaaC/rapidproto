#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
#
# Assemble the GitHub Pages site source into <dest-dir> (wiped first). The site is the USER
# MANUAL: the README as landing page, docs/, the consumer example the manual links into, and the
# license files the README references. Contributor docs (architecture.md, CONTRIBUTING.md,
# SECURITY.md, CHANGELOG.md) are deliberately NOT staged -- the manual links them as absolute
# github.com URLs. Staging an explicit list rather than the whole repo keeps source trees and
# test fixtures off the published site, and makes a new page a conscious decision here (and in
# .github/pages/_data/nav.yml) instead of a silent publish.
#
# Used by .github/workflows/pages.yml; runnable locally against the same container -- see the
# workflow's header comment.
set -euo pipefail

dest=${1:?usage: tests/site_stage.sh <dest-dir>}
root=$(cd "$(dirname "$0")/.." && pwd)

# The wipe below is unconditional, so refuse a destination that is (or contains) a repo before
# it costs someone a working tree.
if [[ -e "$dest/.git" || "$(cd "$dest" 2>/dev/null && pwd)" == "$root" ]]; then
  echo "site_stage: refusing to wipe $dest (it looks like a repository)" >&2
  exit 1
fi
rm -rf "$dest"
mkdir -p "$dest/examples"

cp "$root"/README.md "$root"/LICENSE "$root"/NOTICE "$root"/THIRD_PARTY_NOTICES.md "$dest/"
cp -R "$root/docs" "$dest/docs"
cp -R "$root/examples/consumer" "$dest/examples/consumer"
cp -R "$root/examples/osm-pbf" "$dest/examples/osm-pbf"

# The site chrome: config, the sidebar layout + nav data, the stylesheet, the favicon the layout
# links from every page, and the social card that doubles as every page's og:image (the
# `defaults` block in _config.yml).
cp "$root/.github/pages/_config.yml" "$dest/_config.yml"
cp -R "$root/.github/pages/_layouts" "$dest/_layouts"
cp -R "$root/.github/pages/_data" "$dest/_data"
cp -R "$root/.github/pages/assets" "$dest/assets"
cp "$root/.github/pages/favicon.ico" "$dest/favicon.ico"
cp "$root/.github/pages/social-preview.png" "$dest/assets/social-preview.png"
