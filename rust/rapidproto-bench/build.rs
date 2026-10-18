// Generate each competitor's decoders for the shared benchmark schema (tests/bench/bench.proto) at
// build time, both from pure-Rust parsers so this crate stays `protoc`-free:
//   * prost: protox compiles the .proto to a FileDescriptorSet, prost-build renders it to
//     `$OUT_DIR/bench.rs` (package `bench` -> items at top level; `competitors/prost.rs` includes it).
//   * rust-protobuf: protobuf-codegen's pure parser renders `$OUT_DIR/rust_protobuf_gen/bench.rs`
//     (included by `competitors/rust_protobuf.rs`).
use std::path::PathBuf;

fn main() {
    // tests/bench/ lives at the repo root; this crate is rust/rapidproto-bench/.
    let proto_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/bench");
    let proto = proto_root.join("bench.proto");
    println!("cargo:rerun-if-changed={}", proto.display());

    let fds = protox::compile([&proto], [&proto_root]).expect("protox: compile bench.proto");
    prost_build::Config::new()
        .compile_fds(fds)
        .expect("prost-build: render bench.proto");

    protobuf_codegen::Codegen::new()
        .pure()
        .includes([&proto_root])
        .input(&proto)
        .cargo_out_dir("rust_protobuf_gen")
        .run_from_script();
}
