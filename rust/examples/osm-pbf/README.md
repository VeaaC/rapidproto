# OSM PBF statistics

`osmstat` reads an OpenStreetMap [PBF file](../../../docs/osm-pbf.md#the-format) and prints
statistics about it: how many nodes, ways, relations and tags it holds, its bounding box and its
ten most-used tag keys. By default it decodes each block with the
[arena decoder](https://docs.rs/rapidproto), and `--stream` switches to the
[streaming decoder](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html). Blocks decode
in parallel unless you pass `--sequential`.

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
`testdata/mini.osm.pbf`). Every combination of flags prints these lines for the fixture. The id sum
is every id added into one 64-bit number, shown in hex. `metadata rows` counts the nodes, ways and
relations that come with an `Info` or `DenseInfo` entry.

## Real extracts

[Geofabrik](https://download.geofabrik.de/) publishes extracts by region. Bremen's is a small one,
about 20 MB at the start of 2025. Save it outside your checkout and give `osmstat` that path:

```sh
curl -o /tmp/bremen.osm.pbf https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
cargo run -p osm-pbf-example --release -- /tmp/bremen.osm.pbf
cargo run -p osm-pbf-example --release -- --stream /tmp/bremen.osm.pbf
cargo run -p osm-pbf-example --release -- --sequential /tmp/bremen.osm.pbf
```

`--stream` applies to the header and data blocks; the `BlobHeader` and `Blob` around each block go
through the arena decoder whatever the flags. A parallel run uses rayon's default number of
threads. `RAYON_NUM_THREADS` overrides it. `osmstat` loads the whole file into memory before
decoding, so the file has to fit in RAM.

Only the statistics go to stdout. On stderr, a `model` line names the decoder and the thread count.
Below it, one line each for `read`, `frame` and `process` gives the time taken, and `read` and
`process` add a byte count and a rate. If `osmstat` hits an error, nothing is printed to stdout and
the exit status is 1. The errors go to stderr with an `osmstat:` prefix, as in
`osmstat: no OSMHeader block -- not an .osm.pbf file?`. Blobs must be raw or zlib. Any other
compression fails with `unsupported blob compression (only raw/zlib)`.

## Source files

The [`rapidproto-build`](https://docs.rs/rapidproto-build) call in [`build.rs`](build.rs)
generates both decoders for the two schemas in [`proto/`](proto) into one file, `fileformat.rs`.
[`src/osm.rs`](src/osm.rs) includes that file in a module named `generated`. The rest of the
program uses the decoders through `osm::arena` and `osm::stream`.

- [`src/main.rs`](src/main.rs) goes through the framing once, in order, and checks the header block
  when it reaches it. Then it runs `process_block` on each data blob, in parallel through rayon or
  one after another with `--sequential`. Any error ends the run there.
- [`src/pbf_input.rs`](src/pbf_input.rs) splits the file into `BlobHeader` and `Blob` pairs and
  inflates zlib blobs with `flate2`. Its default backend is pure Rust and needs no zlib on the
  system.
- [`src/stats.rs`](src/stats.rs) keeps the counters and merges the per-block results. Block order
  makes no difference to the merge.

### The arena walk

`process_block` gives each block a fresh
[`Arena`](https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html) and decodes it there with
one `decode` call. [`src/walk_arena.rs`](src/walk_arena.rs) then goes through the decoded block.
The dense-node columns `id()`, `lat()` and `lon()` are `&[i64]` slices in the arena. The string
table's `s()` returns
[`ProtoBytes`](https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoBytes.html) entries that
point into the encoded block.

### The streaming walk

On real files a `--stream` run needs less memory, since no block is decoded into an arena. The
cost is that [`src/walk_stream.rs`](src/walk_stream.rs) reads each block twice. Walking the groups
(field 2) needs the string table and the coordinate scaling fields (`granularity`, `lat_offset`,
`lon_offset`), but the handlers get fields in the order they were written. The string table is
field 1, yet nothing obliges a writer to put it first. The scaling fields are numbered 17, 19 and
20, which puts them after the groups when a writer goes in field-number order, as in the fixture's
raw data block. The walk gathers those values in a first pass that skips the groups, then decodes
only the groups in a second.

The first pass uses the plain
[macro form](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#handlers),
`primitive_block_stream! { .. }` with a nested `string_table_stream! { .. }`, where no two closures
touch the same variable. The header check's `header_block_stream! { .. }` works the same way. The
second pass hands one accumulator, `agg`, down through its nested handlers with
`primitive_block_stream!(&mut agg; { .. })`. Inside `DenseNodes` it travels wrapped with that
group's running state. `agg` holds the statistics for the block and the scaling values from the
first pass.

## Tests

```sh
cargo test -p osm-pbf-example
```

Both tests are in [`tests/fixture.rs`](tests/fixture.rs). `golden_and_modes_agree` runs `osmstat`
on the fixture with no flags, `--sequential`, `--stream` and `--stream --sequential`. Every run has
to succeed with the output shown at the top of this page. `truncated_file_is_rejected` cuts the
last 20 bytes off the fixture and expects `osmstat` to fail on it with no flags, with
`--sequential` and with `--stream`.

## Malformed input

A bad file can't make `osmstat` panic. RapidProto's decoders return an error on malformed input
(see the
[`rapidproto` docs on untrusted input](https://docs.rs/rapidproto/latest/rapidproto/#untrusted-input)).
The example itself has no `unsafe` code. A `--stream` run can still finish without an error on a
malformed file, because it reads less of the file than an arena run does.

An arena run decodes the header and data blocks in full and exits 1 at the first malformed field.
A `--stream` run checks the tag and the length of every field in the messages its handlers decode,
but a sub-message or packed field it skips goes unread. This walk skips the header's `HeaderBBox`,
the `Info` on each node, way and relation, and `Node.vals`. Damage inside any of them leaves the
exit status at 0. The streaming decoder also ignores `required`, and every occurrence of a
singular field goes to its handler
([what a handler receives](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#what-a-handler-receives)).
Either can make the two runs disagree.

With `--stream` the handlers also decide how deep the decode goes. Here they go four levels deep at
most, from `PrimitiveBlock` down to `DenseInfo`. The schema declares no groups, but a file can
still contain unknown ones, one inside another. The decoder skips through them up to the
[depth limit](https://docs.rs/rapidproto/latest/rapidproto/stream/index.html#untrusted-input) and
returns an error for any nested deeper.

The framing code checks each length it reads from the file. A `BlobHeader` must be under 64 KiB.
A `Blob` may be at most 32 MiB and must lie within the file. Before inflating a zlib blob, it
requires a declared `raw_size` from 1 byte to 64 MiB, and the inflated data must come out exactly
that long. Even in a debug build, an overflowing delta, coordinate or id sum wraps instead of
panicking.

The example sets no
[capacity limit](https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html#method.set_capacity_limit)
on its arenas, and nothing caps what the `--stream` handlers keep. A block's arena can outgrow the
inflated block many times over, and the first `--stream` pass stores an entry for every string in
the block's string table. Each thread works on one block at a time, but a crafted file can still
exhaust memory with or without `--stream`. That's an accepted risk for this tool. If your version
of this code reads files you don't trust, limit both.

## Schema and fixture

The two schemas come unmodified from [OSM-binary](https://github.com/openstreetmap/OSM-binary)
v1.7.0 and match the copies in the [C++ example](../../../examples/osm-pbf). Each file carries
Scott A. Crosby's 2010 copyright and the MIT license.
[`THIRD_PARTY_NOTICES.md`](../../../THIRD_PARTY_NOTICES.md) lists the Rust and the C++ copies.

The fixture, [`testdata/mini.osm.pbf`](testdata/mini.osm.pbf), is a synthetic 377-byte file with
the same bytes as the C++ example's. It has a zlib data block with dense nodes, a way and a
relation, and a raw one with a single plain node.
[`testdata/make_fixture.py`](testdata/make_fixture.py) writes it using only Python's standard
library. From `rust/examples/osm-pbf/`:

```sh
python3 testdata/make_fixture.py testdata/mini.osm.pbf
```

With CPython 3.12 and zlib 1.3 the result matches the checked-in file byte for byte.
