// Generate decoders for the schema under `proto/` when the crate is built. The generated code lands
// in OUT_DIR; `src/main.rs` includes it. Editing a `.proto` re-runs this automatically.

fn main() {
    rapidproto_build::Config::new()
        // Emit BOTH decode models into the one generated file: the arena tree (`schema::arena`) and the
        // streaming visitor (`schema::stream`). main.rs decodes the same buffer with each.
        .stream(true)
        // `serde::Serialize` impls when this crate's optional `serde` feature is on.
        .serde(std::env::var_os("CARGO_FEATURE_SERDE").is_some())
        .include("proto")
        .compile(&["proto/message.proto"])
        .expect("generate decoders");
}
