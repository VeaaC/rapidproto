// Generate decoders for the schema under `proto/` when the crate is built. The generated code lands
// in OUT_DIR; `src/main.rs` includes it. Editing a `.proto` re-runs this automatically.

fn main() {
    rapidproto_build::Config::new()
        .include("proto")
        .compile(&["proto/message.proto"])
        .expect("generate decoders");
}
