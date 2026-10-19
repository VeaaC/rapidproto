// Generate Google's upb-backed decoders for the shared benchmark schema (tests/bench/bench.proto).
// Unlike the other arms' pure-Rust codegen, Google's `protobuf-codegen` drives an external `protoc`
// whose version must EXACTLY match the crate (4.36.2 -> protoc 36.2) or it hard-panics. `protoc` is
// not pure-Rust and not on the normal toolchain, which is exactly why the whole arm sits behind
// `rapidproto-bench`'s off-by-default `google-upb` feature -- building this crate is an opt-in,
// toolchain-bearing step (see rapidproto-bench's `src/main.rs` header for the from-source protoc build).
//
// protoc is located via `RAPIDPROTO_UPB_PROTOC` (an absolute path to the matching protoc), falling
// back to `protoc` on `$PATH`. A missing / wrong-version binary surfaces as the codegen's own panic.
use std::path::{Path, PathBuf};

fn main() {
    // tests/bench/ lives at the repo root; this crate is rust/rapidproto-bench-upb/.
    let proto_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/bench");
    let proto = proto_root.join("bench.proto");
    println!("cargo:rerun-if-changed={}", proto.display());
    println!("cargo:rerun-if-env-changed=RAPIDPROTO_UPB_PROTOC");

    let protoc = std::env::var_os("RAPIDPROTO_UPB_PROTOC")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("protoc"));

    let out = std::env::var("OUT_DIR").unwrap();
    // The expected-output path is `output_dir/<input-as-written>`, while protoc writes relative to
    // `--proto_path`; they line up only when the input is the BARE filename and the include is its dir.
    protobuf_codegen::CodeGen::new()
        .protoc_path(Path::new(&protoc))
        .input("bench.proto")
        .include(&proto_root)
        .output_dir(&out)
        .generate_and_compile()
        .expect(
            "protobuf-codegen (google-upb): codegen failed -- need a protoc whose version EXACTLY \
             matches the crate (36.2). Set RAPIDPROTO_UPB_PROTOC to one (see rapidproto-bench \
             src/main.rs).",
        );
}
