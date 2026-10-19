// Generate the OSM decoders from the schema the C++ example already vendors (OSM-binary v1.7.0,
// MIT), so the two examples share one copy. `compile` emits a single file covering the whole closure,
// named after the first entry's stem (`fileformat.rs`); it holds both schemas' types (package
// `OSMPBF`). Editing either .proto re-runs this.

fn main() {
    // This crate is rust/examples/osm-pbf/; the vendored schema is at the repo root's examples/.
    let proto = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../examples/osm-pbf/proto"
    );
    rapidproto_build::Config::new()
        .include(proto)
        .compile(&[
            format!("{proto}/fileformat.proto"),
            format!("{proto}/osmformat.proto"),
        ])
        .expect("generate OSM decoders");
}
