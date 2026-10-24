// Generate each competitor's decoders for the shared benchmark schema (tests/bench/bench.proto) at
// build time, both from pure-Rust parsers so this crate stays `protoc`-free:
//   * prost: protox compiles the .proto to a FileDescriptorSet, prost-build renders it to
//     `$OUT_DIR/bench.rs` (package `bench` -> items at top level; `competitors/prost.rs` includes it).
//   * rust-protobuf: protobuf-codegen's pure parser renders `$OUT_DIR/rust_protobuf_gen/bench.rs`
//     (included by `competitors/rust_protobuf.rs`).
//   * quick-protobuf: pb-rs renders `$OUT_DIR/quick_protobuf_gen/bench.rs`
//     (included by `competitors/quick_protobuf.rs`).
//   * femtopb: femtopb-build (protox + prost-build internally, no protoc) renders
//     `$OUT_DIR/femtopb_gen/bench.rs` (included by `competitors/femtopb.rs`).
// Plus RapidProto's own decoders for the real-world datasets (`external`, below).
use std::path::{Path, PathBuf};

fn main() {
    external();
    // tests/bench/ lives at the repo root; this crate is rust/rapidproto-bench/.
    let proto_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/bench");
    let proto = proto_root.join("bench.proto");
    println!("cargo::rerun-if-changed={}", proto.display());

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

    // femtopb does not support proto3 `map` fields -- its codegen emits a `compile_error!` for them.
    // Strip the one map field (`Person.counters`) before handing the schema to femtopb-build, so the
    // rest of the schema compiles. Consequence: femtopb's `Person`/`Dataset` lack `counters`, so the
    // femtopb arm cannot faithfully decode the `dataset` scenario and is omitted from it (see lib.rs).
    let femto_src = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("femtopb_src");
    std::fs::create_dir_all(&femto_src).expect("mkdir femtopb_src");
    let femto_proto = femto_src.join("bench.proto");
    let schema = std::fs::read_to_string(&proto).expect("read bench.proto");
    let stripped = schema
        .lines()
        .filter(|l| !l.contains("map<string, int32> counters"))
        .collect::<Vec<_>>()
        .join("\n");
    std::fs::write(&femto_proto, stripped).expect("write stripped femtopb proto");

    let femto_out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("femtopb_gen");
    std::fs::create_dir_all(&femto_out).expect("mkdir femtopb_gen");
    femtopb_build::Config::new()
        .protos(std::slice::from_ref(&femto_proto))
        .includes(std::slice::from_ref(&femto_src))
        .target(&femto_out)
        .compile()
        .expect("femtopb-build: render bench.proto");
}

// RapidProto's decoders for the real-world datasets (`src/external/`), through rapidproto-build like
// any consumer's build script: OSM PBF always (its schema is in-repo); the google_message pair when the
// fetched corpus has it (cfg `rp_bench_gm`); compute.proto under the off-by-default `compute` feature
// (its decoder is slow to compile). The corpus is `RAPIDPROTO_FETCHED_CORPUS` (a relative path is
// taken from the repo root), else the C++ build's `build/corpus` (`python3 tests/fetch_corpus.py`);
// a corpus fetched after this crate was built is picked up once the build script re-runs
// (`cargo clean -p rapidproto-bench`).
fn external() {
    println!("cargo::rustc-check-cfg=cfg(rp_bench_gm)");
    // The generated code gates its serde and owning-handle surfaces on the INCLUDING crate's features;
    // this crate declares neither, so name them for the cfg check.
    println!("cargo::rustc-check-cfg=cfg(feature, values(\"serde\", \"alloc\"))");
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let generate = |includes: &[&Path], entries: &[PathBuf], stream: bool| {
        let mut cfg = rapidproto_build::Config::new().stream(stream);
        for dir in includes {
            cfg = cfg.include(dir);
        }
        cfg.compile(entries)
            .unwrap_or_else(|e| panic!("rapidproto-build: {entries:?}: {e:?}"));
    };

    let osm = repo.join("examples/osm-pbf/proto");
    generate(
        &[&osm],
        &[osm.join("fileformat.proto"), osm.join("osmformat.proto")],
        true,
    );

    println!("cargo::rerun-if-env-changed=RAPIDPROTO_FETCHED_CORPUS");
    let corpus = repo.join(
        std::env::var_os("RAPIDPROTO_FETCHED_CORPUS").unwrap_or_else(|| "build/corpus".into()),
    );
    let datasets = corpus.join("protobuf-benchmarks/benchmarks/datasets");
    let gm = [
        datasets.join("google_message1/proto2/benchmark_message1_proto2.proto"),
        datasets.join("google_message2/benchmark_message2.proto"),
    ];
    if gm.iter().all(|p| p.is_file()) {
        for proto in &gm {
            generate(
                &[proto.parent().unwrap()],
                std::slice::from_ref(proto),
                true,
            );
        }
        println!("cargo::rustc-cfg=rp_bench_gm");
    }

    if std::env::var_os("CARGO_FEATURE_COMPUTE").is_some() {
        let googleapis = corpus.join("googleapis");
        let proto = googleapis.join("google/cloud/compute/v1beta/compute.proto");
        assert!(
            proto.is_file(),
            "feature `compute` needs the corpus at {} (python3 tests/fetch_corpus.py)",
            corpus.display()
        );
        generate(
            &[&googleapis, &corpus.join("protobuf/src")],
            &[proto],
            false,
        );
    }
}
