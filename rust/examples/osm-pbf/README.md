# OSM PBF showcase (Rust)

`osmstat` decodes an OpenStreetMap `.osm.pbf` extract with RapidProto's generated **arena** decoders
and prints statistics (counts, tags, bounding box, top tag keys). It is a worked example of pulling a
lot of data out of protobuf quickly with this crate.

```sh
cargo run -p osm-pbf-example --release -- <file.osm.pbf>
cargo run -p osm-pbf-example --release -- --sequential --baseline <file.osm.pbf>
cargo test -p osm-pbf-example          # the fixture test (both modes, golden, truncation)
```

Grab real data from [Geofabrik](https://download.geofabrik.de/), e.g. the ~20 MB Bremen extract:

```sh
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
cargo run -p osm-pbf-example --release -- --baseline bremen-latest.osm.pbf
```

Statistics go to **stdout**; timing goes to **stderr**, so you can redirect one without the other.

## How it uses RapidProto

The point of the example is the shape of an efficient decode:

- **Codegen is a build step, no `protoc`.** `build.rs` runs `rapidproto-build` over
  [`proto/`](proto) — pure Rust, no C++ toolchain; the output is pulled in by [`src/osm.rs`](src/osm.rs).
- **Decode into an `Arena`, read borrowed views.** One `decode()` bump-allocates the message and hands
  back a view that borrows it: no per-field allocation, repeated columns as `&[i64]`, strings as
  `&[u8]` (see [`src/walk.rs`](src/walk.rs)).
- **Arenas make parallelism trivial.** Blocks are independent, so they decode in parallel by default —
  one arena per rayon worker, partials merged order-independently. `--sequential` prints the same thing.
- **Untrusted input stays safe.** Lengths are capped and arithmetic wraps rather than panicking; a
  malformed file exits nonzero, never UB.

`--baseline` also decodes the file with the [`osmpbf`](https://crates.io/crates/osmpbf) crate (a
materializing reader) and prints its counts + throughput on stderr, for a side-by-side comparison.

## Schema and fixture

The OSM wire schema ([`proto/fileformat.proto`](proto/fileformat.proto),
[`proto/osmformat.proto`](proto/osmformat.proto)) is from
[OSM-binary](https://github.com/openstreetmap/OSM-binary) (MIT, © Scott A. Crosby). The test fixture
[`testdata/mini.osm.pbf`](testdata) is a tiny synthetic file; `testdata/make_fixture.py` regenerates it
byte-for-byte (hand-encoded wire format, no dependencies).
