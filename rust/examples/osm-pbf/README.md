# OSM PBF statistics

This example puts RapidProto's two decoders to work on real data. `osmstat` reads an OpenStreetMap
[PBF file](../../../docs/osm-pbf.md#the-format) and prints statistics about it: how many nodes,
ways, relations and tags it holds, its bounding box and its ten most-used tag keys. By default it
decodes each block with the [arena decoder](https://docs.rs/rapidproto), and `--stream` switches to
the [streaming decoder](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html). Blocks
decode in parallel unless you pass `--sequential`.

Run the commands on this page from `rust/` or a directory below it, where Cargo finds the
workspace. Start with the fixture that comes with the example:

```sh
cargo run -p osm-pbf-example --release -- examples/osm-pbf/testdata/mini.osm.pbf
```

```text
blocks           2
nodes            4 (dense 3, plain 1)
ways             1 (refs 3)
relations        1 (members 2)
tags             7 (nodes 4, ways 2, relations 1)
metadata rows    5
id sum           0000000000002ee9
bbox             lat [53.0000000, 53.0000020]  lon [8.7999999, 8.8000020]
distinct keys    3
  key amenity                  3
  key highway                  2
  key name                     2
```

The path is relative to the directory you run Cargo in (from `rust/examples/osm-pbf/` it's
`testdata/mini.osm.pbf`). Every combination of flags prints these lines for the fixture.

## Real extracts

[Geofabrik](https://download.geofabrik.de/) publishes extracts by region. Bremen's is a small one,
about 20 MB at the start of 2025. Save it outside your checkout and give `osmstat` that path:

```sh
curl -o /tmp/bremen.osm.pbf https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
cargo run -p osm-pbf-example --release -- /tmp/bremen.osm.pbf
cargo run -p osm-pbf-example --release -- --stream /tmp/bremen.osm.pbf
cargo run -p osm-pbf-example --release -- --sequential /tmp/bremen.osm.pbf
```

Even with `--stream`, the `BlobHeader` and `Blob` around each block go through the arena decoder.
The statistics are printed to stdout, and the decoder in use and the timings to stderr.

## Source files

The [`rapidproto-build`](https://docs.rs/rapidproto-build) call in [`build.rs`](build.rs)
generates both decoders for the two schemas in [`proto/`](proto) into one file, `fileformat.rs`.
[`src/osm.rs`](src/osm.rs) includes that file and exposes the decoders as `osm::arena` and
`osm::stream`.

- [`src/main.rs`](src/main.rs) checks the header, then hands each data block to its
  `process_block` function to decode and walk.
- [`src/pbf_input.rs`](src/pbf_input.rs) splits the file into `BlobHeader` and `Blob` pairs and
  inflates zlib blobs with `flate2`.
- [`src/stats.rs`](src/stats.rs) holds the counters, combines the per-block results and prints the
  totals.

### The arena walk

`process_block` gives each block a fresh
[`Arena`](https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html) and decodes it there with
one `decode` call. [`src/walk_arena.rs`](src/walk_arena.rs) counts what the decoded block holds.
Not all of it lives in the arena. The string table's
[`ProtoBytes`](https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoBytes.html) entries point
back into the bytes the block was decoded from, which `process_block` inflated or, for a raw block,
took straight from the file. The borrow checker keeps those bytes alive for as long as the decoded
block is in use.

RapidProto doesn't fill in proto2 defaults.
[`granularity()`](https://docs.rs/rapidproto/latest/rapidproto/example/index.html#accessors) is
`None` when a block leaves the field out. Both walks then fall back to 100, the default the schema
declares.

### The streaming walk

A `--stream` run decodes no block into an arena. On real files that saves memory. The price is
that [`src/walk_stream.rs`](src/walk_stream.rs) reads each data block twice. A streaming decoder
hands over fields in the order they were written. Walking a block's primitive groups needs its
string table and its coordinate scaling fields, and a writer may put those after the groups, as
the fixture's raw data block does with its scaling fields. The walk gathers those values in a
first pass that skips the groups, then decodes only the groups in a second.

Both
[forms of the stream macro](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#handlers)
appear here. In the first pass and the header check, each closure has a local variable to itself:
`primitive_block_stream! { .. }` with a `string_table_stream! { .. }` nested in it, and
`header_block_stream! { .. }`. The second pass has many handlers feeding one set of totals. It
opens with `primitive_block_stream!(&mut agg; { .. })` and passes `agg` down into the nested
messages.

## Tests

```sh
cargo test -p osm-pbf-example
```

[`tests/fixture.rs`](tests/fixture.rs) runs the fixture with all four flag combinations and
compares each output with the lines shown above. A second test drops the last 20 bytes of the
fixture and looks for an error exit from `osmstat`.

## Untrusted input

RapidProto's decoders report
[malformed input](https://docs.rs/rapidproto/latest/rapidproto/#untrusted-input) as an error
instead of panicking. Not every bad file fails, though. With `--stream`, nothing looks inside a
sub-message that no handler decodes, such as a node's `Info`, or into a packed field without a
handler. A `--stream` run also lets a missing `required` field through, and when a singular field
occurs twice, its handler runs twice
([what a handler receives](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#what-a-handler-receives)).
A file like that can fail without `--stream` and print statistics with it, or print different
ones in the two runs.

The example sets no
[capacity limit](https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html#method.set_capacity_limit)
on its arenas. The first `--stream` pass also keeps a slice for every string-table entry, however
long the table is. A block can pass the size checks in `src/pbf_input.rs` and still grow far larger
once decoded, so a file with one such block per thread can take all the machine's memory in
either kind of run. For this tool that's an accepted risk. If your copy has to handle
untrusted files, set a capacity limit and give the first pass a maximum table size.

## Schema and fixture

The two schemas come unmodified from [OSM-binary](https://github.com/openstreetmap/OSM-binary)
v1.7.0 and are identical to the [C++ example](../../../examples/osm-pbf)'s copies. They're
MIT-licensed, with Scott A. Crosby's 2010 copyright.
[`THIRD_PARTY_NOTICES.md`](../../../THIRD_PARTY_NOTICES.md) lists the Rust and C++ copies.

The fixture, [`testdata/mini.osm.pbf`](testdata/mini.osm.pbf), is a 377-byte file holding dense
nodes, a way and a relation in a zlib block, and a plain node in a raw one. To rebuild it, run
[`testdata/make_fixture.py`](testdata/make_fixture.py) from `rust/examples/osm-pbf/`:

```sh
python3 testdata/make_fixture.py testdata/mini.osm.pbf
```
