// Generate the OSM arena decoders from the two .proto files in proto/. This is all it takes to use
// RapidProto: add rapidproto-build as a build-dependency and point `compile` at your schema -- no
// protoc, no C toolchain. `compile` renders the whole import closure into one generated file named
// after the first entry's stem (here `fileformat.rs`, holding every `OSMPBF` type), re-running
// whenever a .proto changes.

fn main() {
    let proto = concat!(env!("CARGO_MANIFEST_DIR"), "/proto");
    rapidproto_build::Config::new()
        .include(proto)
        .compile(&[
            format!("{proto}/fileformat.proto"),
            format!("{proto}/osmformat.proto"),
        ])
        .expect("generate OSM decoders");
}
