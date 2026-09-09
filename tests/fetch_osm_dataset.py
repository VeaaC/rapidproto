#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Fetch the OSM PBF benchmark dataset: a PINNED Geofabrik extract, verified by sha256.

    python3 tests/fetch_osm_dataset.py            # fetch into build/corpus/osm/ if absent
    python3 tests/fetch_osm_dataset.py --force    # refetch even if the hash matches

Separate from tests/fetch_corpus.py because this is a plain HTTPS file, not a git checkout --
the corpus fetcher's sparse-checkout/stamp machinery has nothing to grip here. The pin is the
sha256: Geofabrik's `-latest` extracts change daily, so the dataset is one of their yearly
January-1st snapshots, which stay put. Numbers measured on it are checkable by anyone.

The data is (c) OpenStreetMap contributors, ODbL 1.0 (see THIRD_PARTY_NOTICES.md). It is
fetched for local benchmarking only -- gitignored, never distributed.
"""
import argparse
import hashlib
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_DEST = REPO / "build" / "corpus" / "osm"

NAME = "bremen-250101.osm.pbf"
URL = f"https://download.geofabrik.de/europe/germany/{NAME}"
SHA256 = "f650fc00f0545501ec75e93f5919b13ce037a8e0b22aba97495a0f6ec1356bef"
SIZE = 20324876  # bytes; a cheap first-line check before hashing


def file_ok(path: Path) -> bool:
    if not path.is_file() or path.stat().st_size != SIZE:
        return False
    return hashlib.sha256(path.read_bytes()).hexdigest() == SHA256


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    target = args.dest / NAME

    if not args.force and file_ok(target):
        print(f"osm dataset up to date: {target}")
        return 0

    args.dest.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix(".tmp")
    print(f"fetching {URL} ...")
    try:
        with urllib.request.urlopen(URL) as r, open(tmp, "wb") as f:
            while chunk := r.read(1 << 20):
                f.write(chunk)
    except OSError as exc:
        print(f">> fetch failed: {exc}", file=sys.stderr)
        tmp.unlink(missing_ok=True)
        return 1

    digest = hashlib.sha256(tmp.read_bytes()).hexdigest()
    if digest != SHA256:
        # Leave nothing plausible behind: a wrong-hash file must not be mistaken for the pin.
        tmp.unlink(missing_ok=True)
        print(f">> sha256 mismatch for {NAME}:\n   got      {digest}\n   expected {SHA256}\n"
              "   (upstream changed a supposedly-stable snapshot, or the download corrupted)",
              file=sys.stderr)
        return 1
    tmp.replace(target)
    print(f"fetched {target} ({SIZE} bytes, sha256 verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
