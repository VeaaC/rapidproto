// Generates the OSM decoders from the two schemas in `proto/`. `compile` writes a single file,
// named `fileformat.rs` after the first schema listed, with every `OSMPBF` type from the listed
// schemas and their imports. Editing a `.proto` under `proto/` re-runs this script.

fn main() {
    let proto = concat!(env!("CARGO_MANIFEST_DIR"), "/proto");
    // Must name the module that includes the generated file, `osm::generated` in `src/osm.rs`.
    rapidproto_build::Config::new("crate::osm::generated")
        .include(proto)
        // Adds the streaming decoders, `stream::OSMPBF`, beside the default arena decoders in
        // `arena::OSMPBF`. `src/walk_arena.rs` and `src/walk_stream.rs` do the same work with each.
        .stream(true)
        .compile(&[
            format!("{proto}/fileformat.proto"),
            format!("{proto}/osmformat.proto"),
        ])
        .expect("generate OSM decoders");
}
