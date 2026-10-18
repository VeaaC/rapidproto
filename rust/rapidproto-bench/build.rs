// Generate each competitor's decoders for the shared benchmark schema (tests/bench/bench.proto) at
// build time, both from pure-Rust parsers so this crate stays `protoc`-free:
//   * prost: protox compiles the .proto to a FileDescriptorSet, prost-build renders it to
//     `$OUT_DIR/bench.rs` (package `bench` -> items at top level; `competitors/prost.rs` includes it).
//   * rust-protobuf: protobuf-codegen's pure parser renders `$OUT_DIR/rust_protobuf_gen/bench.rs`
//     (included by `competitors/rust_protobuf.rs`).
//   * quick-protobuf: pb-rs renders `$OUT_DIR/quick_protobuf_gen/bench.rs`
//     (included by `competitors/quick_protobuf.rs`).
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

    let qpb_out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("quick_protobuf_gen");
    let _ = std::fs::remove_dir_all(&qpb_out); // pb-rs refuses to overwrite an existing file
    std::fs::create_dir_all(&qpb_out).expect("mkdir quick_protobuf_gen");
    // `ConfigBuilder::new` ties all four path args to one type parameter (`P = PathBuf` here), so they
    // must be the same type -- `slice::from_ref` makes the 1-element input/include slices without a
    // clone (output file: none, we render into `output_dir`).
    let cfg = pb_rs::ConfigBuilder::new(
        std::slice::from_ref(&proto),
        None,
        Some(&qpb_out),
        std::slice::from_ref(&proto_root),
    )
    .expect("pb-rs: config")
    .build();
    pb_rs::types::FileDescriptor::run(&cfg).expect("pb-rs: render bench.proto");
}
