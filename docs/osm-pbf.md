# OSM PBF, decoded with both models

*OpenStreetMap's planet file format, read by two small programs in
[`examples/osm-pbf/`](../examples/osm-pbf/) - one per decode model. Back to the
[README](../README.md); the models: [arena](arena.md), [streaming](streaming.md).*

OSM PBF exercises both models on real data. It is protobuf underneath, almost nothing writes
it (a few exporters produce it, everything else consumes it), and its
heaviest shapes - packed delta-coded integer columns, and a string table meant to be
referenced rather than copied - are the shapes the decoders are built around.

## The format

A `.osm.pbf` file is a sequence of blobs, each framed by a 4-byte big-endian length and a
small `BlobHeader` (`fileformat.proto`). Blob payloads are usually zlib-deflated. The first
blob carries a `HeaderBlock`; a reader has to check its `required_features` and give up on
any it doesn't know. Everything after that is `PrimitiveBlock`s (`osmformat.proto`), each
holding:

- a **stringtable**: every key, value, role and username in the block, referenced by index
  from everywhere else;
- **DenseNodes**: packed delta-coded `sint64` columns for ids, latitudes and longitudes,
  plus one interleaved `keys_vals` stream for the tags. Nodes dominate OSM data (83% of the
  Bremen extract, more at planet scale), so this is where the bytes are;
- **ways** and **relations**: packed delta-coded ref/member columns and parallel key/value
  index arrays;
- optional metadata: an `Info` sub-message per way, relation or plain node, and packed
  `DenseInfo` columns for dense nodes.

## The programs

`osmstat` prints entity counts, tag counts, a bounding box, and the most common tag keys. It
exists twice, once per model, and the two versions print byte-identical output (a fixture
test keeps them that way). Each file is self-contained, so pick the model you care about and
read that one.

[`osmstat_arena.cpp`](https://github.com/VeaaC/rapidproto/blob/main/examples/osm-pbf/osmstat_arena.cpp)
decodes each block into an arena and walks the result as arrays. The program keeps one
arena for the whole run, seeded with a scratch buffer, and
`reset()`s it per block - the reset rewinds without freeing, so after warm-up nothing
allocates, and each block's tree and borrowed strings die together. The `Blob` payload is a
`oneof`, read with the visitor: raw and zlib handled, the other four compression schemes
refused. `granularity` is a proto2 field with `[default=100]`, which means explicit
presence - the accessor returns a `std::optional` and the default is applied with `value_or`
at the use site. The delta walks themselves are `id += ids[i]` over an
`ArrayView<int64>`; the decoder already produced contiguous arrays.

[`osmstat_stream.cpp`](https://github.com/VeaaC/rapidproto/blob/main/examples/osm-pbf/osmstat_stream.cpp)
computes the same numbers without materializing anything. It has to deal with wire order:
tag keys refer to the stringtable by index, and nothing promises the stringtable comes first
in a block, so the program decodes each block twice - a cheap pass that collects only the
stringtable and the coordinate scaling fields, then the real walk. Skipping fields is what a
streaming decoder is good at; even with the double decode, the streaming arm matches the arena
end to end ([benchmarks.md](benchmarks.md#real-world-data---osm-pbf-vs-libosmium)). Packed
parallel arrays arrive one whole array at a time, not interleaved, so by the
time lon element *i* shows up, lat element *i* is long gone; the statistics use per-axis
aggregates precisely so nothing needs pairing. And since an absent field fires no callback,
schema defaults like the granularity are the initial values of locals.

## Running it

[Geofabrik](https://download.geofabrik.de/) publishes extracts from ~20 MB (Bremen) up to
the full planet:

```sh
cmake --preset release && cmake --build --preset release --target osmstat-arena osmstat-stream
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-arena  bremen-latest.osm.pbf
./build/release/examples/osm-pbf/osmstat-stream bremen-latest.osm.pbf
```

Statistics go to stdout, timing to stderr. The arena program reports read / inflate /
decode / walk - with a materialized tree, decoding and reading are separate steps - while
the streaming program reports decode+walk as one number, because for it they are one pass.
Zlib inflate costs several times the protobuf decode, so end-to-end PBF throughput is mostly
a zlib number; [benchmarks.md](benchmarks.md#real-world-data---osm-pbf-vs-libosmium) keeps
the two layers separate.
