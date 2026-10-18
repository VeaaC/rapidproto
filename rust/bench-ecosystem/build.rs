// Generate the prost decoders for the shared benchmark schema (tests/bench/bench.proto) at build
// time. protox compiles the .proto to a FileDescriptorSet in pure Rust (no `protoc`), which
// prost-build then renders to `$OUT_DIR/bench.rs` (package `bench` -> one file, items at top level;
// `src/main.rs` includes it inside `mod prost_bench`).
use std::path::PathBuf;

fn main() {
    // tests/bench/ lives at the repo root; this crate is rust/bench-ecosystem/.
    let proto_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/bench");
    let proto = proto_root.join("bench.proto");
    println!("cargo:rerun-if-changed={}", proto.display());

    let fds = protox::compile([&proto], [&proto_root]).expect("protox: compile bench.proto");
    prost_build::Config::new()
        .compile_fds(fds)
        .expect("prost-build: render bench.proto");
}
