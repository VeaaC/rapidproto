# OSM PBF showcase (Rust)

`osmstat` decodes an OpenStreetMap `.osm.pbf` file with RapidProto's generated **arena** decoders and
prints statistics (counts, tags, bbox, top tag keys). It is the Rust companion to the C++ example at
[`../../../examples/osm-pbf`](../../../examples/osm-pbf), and prints **byte-identical** statistics to
it — both are held to the same golden on the same committed fixture, so the two implementations
cross-validate each other.

What it shows:

- **Parallel by default.** Each `PrimitiveBlock` is independent, so blocks decode across all cores
  (rayon), each into its own arena, and the per-axis aggregates merge order-independently. `--sequential`
  forces one thread and prints the *same* statistics — the fixture test holds both modes to the golden.
- Decoding is one `decode()` per block into an arena, then a walk over contiguous arrays: delta-coded
  `sint64` columns are `&[i64]`, every stringtable entry is a borrowed `&[u8]`.
- `--baseline` additionally decodes the file with the [`osmpbf`](https://crates.io/crates/osmpbf)
  crate (a materializing reader) and prints its counts + throughput on stderr, beside ours.

Statistics go to **stdout**; timing (read / frame / process, and the baseline) goes to **stderr**.

## Building and running

Built with cargo alone — zlib is the pure-Rust `miniz_oxide` (via `flate2`), no system library.

```sh
cargo run -p osm-pbf-example --release -- <file.osm.pbf>
cargo run -p osm-pbf-example --release -- --sequential --baseline <file.osm.pbf>
cargo test -p osm-pbf-example          # the fixture test (both modes, golden, truncation)
```

Real data comes from [Geofabrik](https://download.geofabrik.de/), e.g. the ~20 MB Bremen extract:

```sh
curl -O https://download.geofabrik.de/europe/germany/bremen-latest.osm.pbf
cargo run -p osm-pbf-example --release -- --baseline bremen-latest.osm.pbf
```

## Schema and fixture

The schema ([`fileformat.proto`](../../../examples/osm-pbf/proto/fileformat.proto),
[`osmformat.proto`](../../../examples/osm-pbf/proto/osmformat.proto)) and the test fixture
(`mini.osm.pbf`) are the ones the C++ example vendors (from
[OSM-binary](https://github.com/openstreetmap/OSM-binary) v1.7.0, MIT — Scott A. Crosby); this example
reuses that single copy rather than duplicating it. `build.rs` generates the decoders from it at build
time with `rapidproto-build` (pure Rust, no `protoc`).
