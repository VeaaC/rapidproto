# OSM PBF showcase (Rust)

`osmstat` decodes an OpenStreetMap `.osm.pbf` extract with RapidProto and prints statistics (counts,
tags, bounding box, top tag keys). It is a worked example of pulling a lot of data out of protobuf
quickly — and it runs the **same workload through both decode models**, so you can compare them:

- **arena** (default): decode each block into an `Arena`, then read borrowed views ([`src/walk_arena.rs`](src/walk_arena.rs));
- **streaming** (`--stream`): materialize nothing, handle each field with a callback as it is decoded
  ([`src/walk_stream.rs`](src/walk_stream.rs)).

Both print byte-identical statistics — the fixture test holds all four (model × execution) combinations
to one golden.

```sh
cargo run -p osm-pbf-example --release -- <file.osm.pbf>            # arena (default)
cargo run -p osm-pbf-example --release -- --stream <file.osm.pbf>  # streaming visitor
cargo run -p osm-pbf-example --release -- --sequential <file.osm.pbf>
cargo test -p osm-pbf-example          # fixture: both models agree, golden, truncation
```

Grab real data from [Geofabrik](https://download.geofabrik.de/), e.g. the ~20 MB Bremen extract:

```sh
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
cargo run -p osm-pbf-example --release -- bremen-latest.osm.pbf
```

Statistics go to **stdout**; timing goes to **stderr**, so you can redirect one without the other.

## How it uses RapidProto

The point of the example is the shape of an efficient decode:

- **Codegen is a build step, no `protoc`.** `build.rs` runs `rapidproto-build` over
  [`proto/`](proto) — pure Rust, no C++ toolchain. `.stream(true)` asks for both models, so the one
  generated file exposes `arena::OSMPBF` and `stream::OSMPBF` (pulled in by [`src/osm.rs`](src/osm.rs)).
- **Arena: decode once, read borrowed views.** One `decode()` bump-allocates the message and hands back
  a view that borrows it: no per-field allocation, repeated columns as `&[i64]`, strings as `&[u8]`.
- **Streaming: materialize nothing, visit fields.** `Msg::new(bytes).decode(visitor)` fires one callback
  per field as it is decoded. The walk shows both macro forms: the plain `*_stream! { field => |v| .. }`
  for pass 1 (each field sets a disjoint local), and the shared-state form
  `*_stream!(&mut agg; { field => |agg, v| .. })` for pass 2, where the decoder lends one `&mut agg`
  accumulator to every handler across the whole nested walk (which `&mut`-exclusive closure captures
  can't). It also shows the two-pass idiom a back-referencing schema needs (the stringtable must be read
  before the groups that index it).
- **Parallelism is trivial either way.** Blocks are independent, so they decode in parallel by default,
  partials merged order-independently. `--sequential` prints the same thing.
- **Untrusted input stays safe.** Lengths are capped and arithmetic wraps rather than panicking; a
  wire-malformed file exits nonzero, never UB. (Streaming hands sub-messages to the consumer as child
  handles — recursing into them is the consumer's depth to bound; this walk recurses a fixed, shallow
  depth set by the schema, not the input: block → group → entity → metadata.)

## Schema and fixture

The OSM wire schema ([`proto/fileformat.proto`](proto/fileformat.proto),
[`proto/osmformat.proto`](proto/osmformat.proto)) is from
[OSM-binary](https://github.com/openstreetmap/OSM-binary) (MIT, © Scott A. Crosby). The test fixture
[`testdata/mini.osm.pbf`](testdata) is a tiny synthetic file; `testdata/make_fixture.py` regenerates it
byte-for-byte (hand-encoded wire format, no dependencies).
