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

rm -rf "$dest"
mkdir -p "$dest/examples"

cp "$root"/README.md "$root"/architecture.md "$root"/SECURITY.md \
   "$root"/CHANGELOG.md "$root"/LICENSE "$root"/NOTICE "$root"/THIRD_PARTY_NOTICES.md "$dest/"

# jekyll-optional-front-matter deliberately skips CONTRIBUTING.md (it is on the plugin's default
# exclude list, unlike every other page here), which would publish it as raw markdown instead of a
# rendered page. An injected empty front-matter block makes Jekyll render it like the rest.
printf -- '---\n---\n' > "$dest/CONTRIBUTING.md"
cat "$root/CONTRIBUTING.md" >> "$dest/CONTRIBUTING.md"
cp -R "$root/docs" "$dest/docs"
cp -R "$root/examples/consumer" "$dest/examples/consumer"
cp "$root/.github/pages/_config.yml" "$dest/_config.yml"
