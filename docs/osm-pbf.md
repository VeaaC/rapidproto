# OSM PBF, decoded with both models

*A real-world walkthrough: OpenStreetMap's planet file format, read by two small complete
programs in [`examples/osm-pbf/`](../examples/osm-pbf/) — one per decode model. Back to the
[README](../README.md); the models themselves: [arena](arena.md), [streaming](streaming.md).*

OSM PBF is protobuf all the way down, and it is *decode-only by nature* — the planet files are
produced by a handful of exporters and read by everything else. It is also built from exactly
the shapes this project optimizes for, which makes it the honest showcase: no synthetic schema,
no benchmark-friendly framing, just the format half the mapping world ships.

## What's on the wire

A `.osm.pbf` file is a sequence of length-framed **blobs** (`fileformat.proto`): a 4-byte
big-endian length, a tiny `BlobHeader` saying what follows, then a `Blob` whose payload is
usually zlib-deflated. The first blob is a `HeaderBlock` — including `required_features`, which
a reader must check and refuse if it doesn't implement (both programs do). Every other blob is
a `PrimitiveBlock` (`osmformat.proto`):

- a **stringtable**: every key, value, role and username in the block, as `bytes`, referenced
  everywhere else by index;
- **DenseNodes** — the dominant shape, ~98% of a typical file's entities: packed delta-coded
  `sint64` columns for ids, latitudes and longitudes, plus one interleaved `keys_vals` index
  stream carrying all node tags;
- **ways** and **relations**: packed delta-coded ref/member columns plus parallel key/value
  index arrays;
- optional per-entity metadata (`Info`/`DenseInfo`: versions, timestamps, uids — also packed
  and delta-coded).

Two things make this format a natural fit here. Packed delta-coded `sint64` columns are
precisely the arena decoder's strongest path — bulk packed-varint decode straight into
contiguous `int64` arrays. And the stringtable is a large table of strings a parser must not
copy; the decoders borrow every entry as a `string_view` into the inflated buffer instead.

## The two programs

Both compute the same statistics (entity counts, tags, bbox, top tag keys) and print
byte-identical stdout — the fixture test holds them to a hand-derived golden — but each is
written against one model only, so you can read one file end to end:

**[`osmstat_arena.cpp`](https://github.com/VeaaC/rapidproto/blob/main/examples/osm-pbf/osmstat_arena.cpp)**
materializes each block, then walks arrays. Things it shows that the synthetic examples don't:

- a **fresh `Arena` per block, seeded with a reused scratch buffer** — steady-state decoding of
  a multi-gigabyte file allocates nothing, and each block's tree (and every borrowed string)
  dies wholesale at the end of its loop iteration;
- the `Blob` payload **`oneof` read with the visitor** — raw and zlib handled, the four other
  compression schemes refused explicitly;
- proto2 **`[default=100]` fields** (`granularity`): explicit presence means the accessor is a
  `std::optional`, and the schema default is applied at the use site with `value_or`;
- the delta walks themselves: `id += ids[i]` over an `ArrayView<int64>` that the decoder
  already materialized contiguously.

**[`osmstat_stream.cpp`](https://github.com/VeaaC/rapidproto/blob/main/examples/osm-pbf/osmstat_stream.cpp)**
walks each block once with callbacks, materializing nothing. Its lessons are different:

- **wire order is real**: tag keys reference the stringtable by index, and nothing guarantees
  the stringtable arrives first — so each block is decoded twice, a cheap stringtable-only pass
  and then the walk. Skipping unwanted fields is what streaming decoders are good at, and the
  double decode still beats materializing for this task;
- **parallel packed arrays arrive one whole array at a time**, not interleaved — at "lon
  element *i*" the matching lat element is long gone unless you buffered it. The statistics
  are deliberately built from per-axis aggregates so nothing needs pairing; a task that truly
  needs `(lat, lon)` pairs per node buffers one column, and that buffering cost is part of an
  honest model choice;
- **no defaults are delivered**: an absent `granularity` fires no callback, so the schema's
  default is the initial value of a local;
- delta accumulators and the `keys_vals` key/value state machine live in plain locals — the
  whole walk is allocation-free except the stringtable index.

## Running it on real data

The fixture under `testdata/` is enough for the tests, but the point is real data —
[Geofabrik](https://download.geofabrik.de/) publishes extracts at every size from ~20 MB
(Bremen) to the full planet:

```sh
cmake --preset release && cmake --build --preset release --target osmstat-arena osmstat-stream
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-arena  bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-stream bremen-latest.osm.pbf
```

Statistics go to stdout; a per-stage timing breakdown (read / inflate / decode / walk) goes to
stderr. One structural fact jumps out of that breakdown on any input: **zlib inflate costs a
multiple of the decode** — the blobs must be inflated before any protobuf work starts, and that
step dominates end-to-end time. Any "how fast does X read PBF" comparison that doesn't separate
inflate from decode is mostly benchmarking zlib; the numbers in
[benchmarks.md](benchmarks.md) report both.
