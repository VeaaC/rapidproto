// Generate the OSM decoders from the two .proto files in proto/. This is all it takes to use
// RapidProto: add rapidproto-build as a build-dependency and point `compile` at your schema -- no
// protoc. `compile` renders the whole import closure into one generated file named
// after the first entry's stem (here `fileformat.rs`, holding every `OSMPBF` type), re-running
// whenever a .proto changes.
//
// `.stream(true)` emits the streaming visitor model ALONGSIDE the default arena model, so the one
// generated file exposes both `arena::OSMPBF` and `stream::OSMPBF` -- the two decode models this
// example runs the same workload through (see src/walk.rs and src/walk_stream.rs).

fn main() {
    let proto = concat!(env!("CARGO_MANIFEST_DIR"), "/proto");
    rapidproto_build::Config::new()
        .include(proto)
        .stream(true)
        .compile(&[
            format!("{proto}/fileformat.proto"),
            format!("{proto}/osmformat.proto"),
        ])
        .expect("generate OSM decoders");
}
