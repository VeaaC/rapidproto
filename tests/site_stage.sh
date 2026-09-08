#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
#
# Assemble the GitHub Pages site source into <dest-dir> (wiped first). The site is the repo's
# documentation surface -- the closure of the manual's relative links: the root markdown pages,
# docs/, the license files they reference, and examples/consumer (the runnable example the docs
# link into). Staging an explicit list rather than the whole repo keeps source trees and test
# fixtures off the published site, and makes an added top-level directory a conscious decision
# here instead of a silent publish.
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
mkdir -p "$dest/examples" "$dest/assets"

# CONTRIBUTING.md renders like the rest only because _config.yml lists it under `include:` --
# jekyll-optional-front-matter refuses it by filename otherwise (see the comment there).
cp "$root"/README.md "$root"/architecture.md "$root"/CONTRIBUTING.md "$root"/SECURITY.md \
   "$root"/CHANGELOG.md "$root"/LICENSE "$root"/NOTICE "$root"/THIRD_PARTY_NOTICES.md "$dest/"
cp -R "$root/docs" "$dest/docs"
cp -R "$root/examples/consumer" "$dest/examples/consumer"
cp "$root/.github/pages/_config.yml" "$dest/_config.yml"
# _includes/head-custom.html links /favicon.ico from every page (the theme's own include is an
# inert comment); the card doubles as every page's og:image (the `defaults` block in _config.yml).
cp -R "$root/.github/pages/_includes" "$dest/_includes"
cp "$root/.github/pages/favicon.ico" "$dest/favicon.ico"
cp "$root/.github/pages/social-preview.png" "$dest/assets/social-preview.png"
