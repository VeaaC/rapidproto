# OSM PBF, decoded with both models

*OpenStreetMap's planet file format, read by two small complete programs in
[`examples/osm-pbf/`](../examples/osm-pbf/) — one per decode model. Back to the
[README](../README.md); the models themselves: [arena](arena.md), [streaming](streaming.md).*

OSM PBF is protobuf all the way down, and nobody serializes it from a reader — a decode-only
format, read by half the mapping world. It is also built from the shapes this project
optimizes for, which is why it makes a good real-world example.

## What's on the wire

A `.osm.pbf` file is a sequence of length-framed **blobs** (`fileformat.proto`): a 4-byte
big-endian length, a small `BlobHeader`, then a `Blob` whose payload is usually
zlib-deflated. The first blob is a `HeaderBlock` — its `required_features` must be checked
and unknown ones refused. Every other blob is a `PrimitiveBlock` (`osmformat.proto`):

- a **stringtable**: every key, value, role and username in the block, referenced everywhere
  else by index;
- **DenseNodes** — the dominant shape (83% of the Bremen extract's entities; more at planet
  scale): packed delta-coded `sint64` columns for ids, latitudes and longitudes, plus one
  interleaved `keys_vals` index stream for all node tags;
- **ways** and **relations**: packed delta-coded ref/member columns plus parallel key/value
  index arrays;
- optional per-entity metadata: a plain `Info` sub-message on ways/relations/plain nodes,
  packed delta-coded `DenseInfo` columns for dense nodes.

Packed delta-coded `sint64` columns are the arena decoder's strongest path (bulk
packed-varint decode into contiguous `int64` arrays), and the stringtable is a table of
strings a parser should not copy — the decoders borrow every entry as a `string_view` into
the inflated buffer.

## The two programs

Both compute the same statistics (entity counts, tags, bbox, top tag keys) and print
byte-identical stdout — a fixture test holds them to a hand-derived golden. Each is written
against one model only, so you can read one file end to end.

**[`osmstat_arena.cpp`](https://github.com/VeaaC/rapidproto/blob/main/examples/osm-pbf/osmstat_arena.cpp)**
materializes each block, then walks arrays. What it shows:

- one seeded `Arena`, `reset()` per block: the reset rewinds but keeps the memory, so
  steady-state decoding allocates nothing, and each block's tree and borrowed strings die
  together at its reset;
- the `Blob` payload `oneof` read with the visitor — raw and zlib handled, the other four
  compression schemes refused;
- proto2 `[default=100]` fields (`granularity`): explicit presence means a `std::optional`
  accessor, and the schema default is applied at the use site with `value_or`;
- the delta walks: `id += ids[i]` over an `ArrayView<int64>` the decoder already
  materialized contiguously.

**[`osmstat_stream.cpp`](https://github.com/VeaaC/rapidproto/blob/main/examples/osm-pbf/osmstat_stream.cpp)**
walks each block with callbacks, materializing nothing. Its lessons:

- **wire order is real**: tag keys reference the stringtable by index, and nothing
  guarantees the stringtable arrives first — so each block is decoded twice, a cheap
  stringtable-only pass and then the walk. Skipping unwanted fields is what streaming
  decoders are good at, and the double decode still beats materializing for this task;
- **parallel packed arrays arrive one whole array at a time**, not interleaved: at "lon
  element *i*" the matching lat element is long gone unless you buffered it. The statistics
  use per-axis aggregates, so nothing needs pairing;
- **no defaults are delivered**: an absent `granularity` fires no callback, so the schema
  default is a local's initial value;
- delta accumulators and the `keys_vals` state machine are plain locals.

## Running it on real data

[Geofabrik](https://download.geofabrik.de/) publishes extracts from ~20 MB (Bremen) up to
the full planet:

```sh
cmake --preset release && cmake --build --preset release --target osmstat-arena osmstat-stream
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-arena  bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-stream bremen-latest.osm.pbf
```

Statistics go to stdout. Timing goes to stderr: read / inflate / decode / walk from the
arena program (a materialized tree makes decode and walk separate steps), read / inflate /
decode+walk from the streaming one. The breakdown makes one fact obvious on any input: zlib
inflate costs a multiple of the protobuf decode, so an end-to-end PBF number is mostly a
zlib number. [benchmarks.md](benchmarks.md) reports the two layers separately.
