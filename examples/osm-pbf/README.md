# OSM PBF showcase

The same statistics tool for OpenStreetMap's planet file format, written twice — once per
decode model, in separate files so each reads on its own:

- [`osmstat_arena.cpp`](osmstat_arena.cpp) materializes each `PrimitiveBlock` into an arena
  and walks it as contiguous arrays. The packed delta-coded `sint64` columns decode straight
  into `int64` arrays; every stringtable entry is a borrowed `string_view`.
- [`osmstat_stream.cpp`](osmstat_stream.cpp) materializes nothing: callbacks per field,
  delta accumulators in locals. It decodes each block twice — a cheap stringtable-only pass,
  then the walk — because wire order doesn't promise the stringtable comes first.

Both print identical statistics (counts, tags, bbox, top tag keys) on stdout; a CTest holds
them to a hand-derived golden on [`testdata/mini.osm.pbf`](testdata/mini.osm.pbf), which in
turn must reproduce byte-for-byte from [`testdata/make_fixture.py`](testdata/make_fixture.py).
Timing goes to stderr: read / inflate / decode / walk from the arena program, read / inflate /
decode+walk from the streaming one.

The manual's [OSM PBF page](../../docs/osm-pbf.md) walks through the format and what each
program demonstrates about its model.

The schema ([`proto/fileformat.proto`](proto/fileformat.proto),
[`proto/osmformat.proto`](proto/osmformat.proto)) is vendored from
[OSM-binary](https://github.com/openstreetmap/OSM-binary) v1.7.0. Both files are MIT
(Scott A. Crosby); the license text is in each file's header and in the repository's
THIRD_PARTY_NOTICES.md.

## Building and running

Needs zlib — a dependency of this example, not of RapidProto; without it the example skips
itself. In-tree (the default when the RapidProto tests are enabled):

```sh
cmake --preset release && cmake --build --preset release --target osmstat-arena osmstat-stream
ctest --test-dir build/release -R rapidproto_osmstat    # the fixture test
```

Real data comes from [Geofabrik](https://download.geofabrik.de/), e.g. the ~20 MB Bremen
extract:

```sh
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-arena bremen-latest.osm.pbf
```

Standalone against an installed RapidProto, like the consumer example:

```sh
cmake -S examples/osm-pbf -B osm-build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build osm-build && ctest --test-dir osm-build
```
