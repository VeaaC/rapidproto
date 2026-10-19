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

The whole point of this example is the shape of an efficient decode. Four things to take away:

- **Codegen is a build step, no `protoc`.** `build.rs` adds `rapidproto-build` as a build-dependency
  and calls `Config::new().include("proto").compile(&[...])` — pure Rust, no C++ toolchain. The schema
  lives in [`proto/`](proto); the generated code lands in `OUT_DIR` and is pulled in from
  [`src/osm.rs`](src/osm.rs).
- **Decode into an `Arena`, read borrowed views.** One `decode()` bump-allocates the whole message
  into an arena and hands back a lightweight view that borrows it — no per-field heap allocation, no
  copying. Repeated scalar columns come back as `&[i64]`, every stringtable entry as a borrowed
  `&[u8]`. The field walk in [`src/walk.rs`](src/walk.rs) is just reads off that arena.
- **Arenas make parallelism trivial.** Each `PrimitiveBlock` in the file is independent, so blocks
  decode in parallel by default — one `Arena` per rayon worker, partial stats merged
  order-independently. `--sequential` runs the identical per-block work on one thread and prints the
  *same* output.
- **Untrusted input stays safe.** The example never trusts lengths or deltas from the file: allocation
  is capped, and delta/coordinate arithmetic wraps rather than panicking in release (see
  [`src/stats.rs`](src/stats.rs)). A malformed file is rejected with a nonzero exit, never UB.

`--baseline` additionally decodes the file with the [`osmpbf`](https://crates.io/crates/osmpbf) crate
(a materializing reader) and prints its counts + throughput on stderr, for a side-by-side comparison.

## Schema and fixture

The OSM wire schema ([`proto/fileformat.proto`](proto/fileformat.proto),
[`proto/osmformat.proto`](proto/osmformat.proto)) is from
[OSM-binary](https://github.com/openstreetmap/OSM-binary) (MIT, © Scott A. Crosby). The test fixture
[`testdata/mini.osm.pbf`](testdata) is a tiny synthetic file; `testdata/make_fixture.py` regenerates it
byte-for-byte (hand-encoded wire format, no dependencies).
