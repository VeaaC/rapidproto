# OSM PBF showcase

Two small, complete decoders for OpenStreetMap's planet file format — the same statistics tool
written once per decode model, deliberately in **separate files** so each reads on its own:

- [**`osmstat_arena.cpp`**](osmstat_arena.cpp) — materializes each `PrimitiveBlock` into an
  arena and walks it as contiguous arrays. DenseNodes' packed delta-coded `sint64` columns
  decode straight into `int64` arrays; every stringtable entry is a borrowed `string_view`.
- [**`osmstat_stream.cpp`**](osmstat_stream.cpp) — one callback pass per block, nothing
  materialized, delta accumulators in locals. Decodes each block twice: a cheap
  stringtable-only pass, then the walk (wire order doesn't promise the stringtable first).

Both print identical statistics (counts, tags, bbox, top tag keys) on stdout — a CTest holds
them to a hand-derived golden on [`testdata/mini.osm.pbf`](testdata/mini.osm.pbf), a committed
fixture that reproduces byte-for-byte from
[`testdata/make_fixture.py`](testdata/make_fixture.py) — and a per-stage timing breakdown
(read / inflate / decode / walk) on stderr.

The walkthrough — what the format looks like on the wire, and what each program teaches about
its model — is the manual's [OSM PBF page](../../docs/osm-pbf.md).

The schema ([`proto/fileformat.proto`](proto/fileformat.proto),
[`proto/osmformat.proto`](proto/osmformat.proto)) is vendored from
[OSM-binary](https://github.com/openstreetmap/OSM-binary) v1.7.0; the two files are MIT
(Scott A. Crosby — the license header is in each file, and in the repository's
THIRD_PARTY_NOTICES.md).

## Building and running

Needs zlib (`find_package(ZLIB)`) — a dependency of this example, not of RapidProto; without
it the example skips itself. In-tree (the default when the RapidProto tests are enabled):

```sh
cmake --preset release && cmake --build --preset release --target osmstat-arena osmstat-stream
ctest --test-dir build/release -R rapidproto_osmstat    # the fixture test
```

Real data comes from [Geofabrik](https://download.geofabrik.de/) at every size — e.g. the
~20 MB Bremen extract:

```sh
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-arena bremen-latest.osm.pbf
```

Standalone against an **installed** RapidProto, like the consumer example:

```sh
cmake -S examples/osm-pbf -B osm-build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build osm-build && ctest --test-dir osm-build
```
