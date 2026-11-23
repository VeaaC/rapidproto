// Generates the decoders for `proto/message.proto` into `OUT_DIR`, where `src/main.rs` includes
// them. rapidproto-build tells Cargo to re-run this script when any `.proto` under `proto/`
// changes.

fn main() {
    // The path of `mod schema` in `main.rs`, which includes the output. Generated code spells
    // paths to its own types from there.
    rapidproto_build::Config::new("crate::schema")
        // Streaming decoders in `schema::stream` next to the arena decoders in `schema::arena`, in
        // the same file, with the enums both use in `schema::common`. `main.rs` runs both on one
        // input.
        .stream(true)
        // `Serialize` impls only when this crate's optional `serde` feature is on.
        .serde(std::env::var_os("CARGO_FEATURE_SERDE").is_some())
        // Where `message.proto`'s import of `types.proto` is found.
        .include("proto")
        .compile(&["proto/message.proto"])
        .expect("generate decoders");
}
