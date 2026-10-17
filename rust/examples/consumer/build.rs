// Generate the Rust decoders for the example schema at build time, through the committed wasm blob
// (driven by wasmi) -- cargo alone, no C++ toolchain. This is the consumer entry point
// rapidproto-build exists to provide; src/main.rs include!s the OUT_DIR file this writes.

use std::path::PathBuf;

fn main() {
    // The schema is shared with the C++ consumer example (examples/consumer/proto), so both languages
    // decode the identical demo.Shape. message.proto imports types.proto, so the import closure is
    // exercised too.
    let proto_dir =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../examples/consumer/proto");
    rapidproto_build::Config::new()
        .include(proto_dir.clone())
        .compile(&[proto_dir.join("message.proto")])
        .expect("generate rapidproto decoders for the example schema");
}
