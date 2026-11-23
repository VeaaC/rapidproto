# OSM PBF statistics

This example runs RapidProto's two decoders on a real format. `osmstat` reads an OpenStreetMap
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

`--stream` switches the decoder for the header and data blocks only. The `BlobHeader` and `Blob`
around each block always go through the arena decoder.

The statistics go to stdout and the timings to stderr. When `osmstat` stops with an error, it
reports it on stderr with an `osmstat:` prefix, prints nothing to stdout and exits with status 1.

## Source files

The [`rapidproto-build`](https://docs.rs/rapidproto-build) call in [`build.rs`](build.rs)
generates both decoders for the two schemas in [`proto/`](proto) into one file, `fileformat.rs`.
[`src/osm.rs`](src/osm.rs) includes that file and exposes the decoders as `osm::arena` and
`osm::stream`.

- [`src/main.rs`](src/main.rs) reads the blobs in order, checks the header block on the way, and
  hands each data blob to `process_block`.
- [`src/pbf_input.rs`](src/pbf_input.rs) splits the file into `BlobHeader` and `Blob` pairs and
  inflates zlib blobs with `flate2`.
- [`src/stats.rs`](src/stats.rs) holds the counters, combines the per-block results and prints the
  totals.

### The arena walk

`process_block` gives each block a fresh
[`Arena`](https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html) and decodes it there with
one `decode` call. [`src/walk_arena.rs`](src/walk_arena.rs) then counts what the decoded block
holds. The strings in its string table stay where they are, since each
[`ProtoBytes`](https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoBytes.html) entry points
into the block's bytes.

### The streaming walk

On real files a `--stream` run needs less memory, since no block is decoded into an arena. The
cost is that [`src/walk_stream.rs`](src/walk_stream.rs) reads each block twice. A streaming decoder
hands over fields in the order they were written. Walking a block's primitive groups needs its
string table and its coordinate scaling fields, and a writer may put those after the groups, as the
fixture's raw data block does with its scaling fields. So the first pass collects the string table
and the scaling fields and skips the groups, and the second decodes only the groups.

The first pass, like the header check, uses the plain
[macro form](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#handlers),
`primitive_block_stream! { .. }` around a `string_table_stream! { .. }`, since each of its closures
writes a variable of its own. Every handler in the second pass updates the same accumulator, `agg`,
so that pass uses `primitive_block_stream!(&mut agg; { .. })` to hand it down through the nested
handlers.

## Tests

```sh
cargo test -p osm-pbf-example
```

[`tests/fixture.rs`](tests/fixture.rs) runs the fixture through all four flag combinations and
expects each run to match the output at the top of this page. It also cuts 20 bytes off the end of
the fixture and checks that `osmstat` rejects the result.

## Malformed input

RapidProto's decoders report
[malformed input](https://docs.rs/rapidproto/latest/rapidproto/#untrusted-input) as an error
instead of panicking, and the example needs no `unsafe` code to use them.

That doesn't mean every bad file fails. With `--stream`, nothing looks inside a sub-message that no
handler decodes, such as a node's `Info`, or inside a packed field that has no handler. A `--stream`
run also lets a missing `required` field through, and when a singular field occurs twice, its
handler runs twice
([what a handler receives](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#what-a-handler-receives)).
On such a file the two decoders can print different statistics.

The example sets no
[capacity limit](https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html#method.set_capacity_limit)
on its arenas. The first `--stream` pass stores a slice for each string-table entry, however long
the table is. So a crafted file, with a large block for each thread, can use up all
memory whether or not you pass `--stream`. For this tool that's an accepted risk. If your copy has
to handle untrusted files, limit the arenas and the string table.

## Schema and fixture

The two schemas come unmodified from [OSM-binary](https://github.com/openstreetmap/OSM-binary)
v1.7.0 and are the same files the [C++ example](../../../examples/osm-pbf) uses. They're
MIT-licensed, with Scott A. Crosby's 2010 copyright.
[`THIRD_PARTY_NOTICES.md`](../../../THIRD_PARTY_NOTICES.md) lists the Rust and C++ copies.

The fixture, [`testdata/mini.osm.pbf`](testdata/mini.osm.pbf), is a 377-byte file holding dense
nodes, a way and a relation in a zlib block, and a plain node in a raw one. To rebuild it, run
[`testdata/make_fixture.py`](testdata/make_fixture.py) from `rust/examples/osm-pbf/`:

```sh
python3 testdata/make_fixture.py testdata/mini.osm.pbf
```
