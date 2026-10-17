// Cargo-only consumer of the RapidProto Rust decoders -- no C++ toolchain. build.rs generated the
// decoders for demo.Shape (examples/consumer/proto/message.proto, which imports types.proto) through
// the wasm blob into OUT_DIR; here we decode a wire buffer and read it back with the arena
// object-tree API a real consumer uses. The Rust mirror of examples/consumer/main.cpp, decoding the
// identical bytes.

// Generated code: lint-exempt, the way a real consumer wraps it. `unused` covers the parts of the
// schema this binary does not touch; the casing allows cover schemas whose proto names are not
// Rust-conventional (the emitter preserves proto names rather than re-casing them).
#[allow(
    clippy::all,
    unused,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod schema {
    // One generated file for the whole import closure (message.proto + types.proto).
    include!(concat!(env!("OUT_DIR"), "/message.rs"));
}

use rapidproto::Arena;
use schema::arena::demo::Shape;
use schema::common::demo::Kind;

// Wire bytes for demo.Shape{ name: "hi", origin: { x: 3, y: 4 }, kind: KIND_CIRCLE, sides: [3, 4, 5] },
// plus a field number 99 the schema does not define (a newer producer) -- skipped on decode:
//   field 1 (name, len):     0x0A 0x02 'h' 'i'
//   field 2 (origin, msg):   0x12 0x04  [0x08 0x03  0x10 0x04]
//   field 3 (kind, varint):  0x18 0x01
//   field 4 (sides, packed): 0x22 0x03  [0x03 0x04 0x05]
//   field 99 (unknown):      0x98 0x06 0x2A        (varint 42)
const SHAPE_WIRE: &[u8] = &[
    0x0A, 0x02, b'h', b'i', 0x12, 0x04, 0x08, 0x03, 0x10, 0x04, 0x18, 0x01, 0x22, 0x03, 0x03, 0x04,
    0x05, 0x98, 0x06, 0x2A,
];

fn main() {
    let arena = Arena::new();
    let shape = Shape::decode(SHAPE_WIRE, &arena).expect("decode demo.Shape");

    let origin = shape.origin().expect("origin is present");
    let sides_sum: i32 = shape.sides().iter().sum();

    // The cross-file reference (Shape -> Point, from the imported types.proto), the shared enum, a
    // string, and a packed repeated field -- all read back from the one decoded tree.
    assert_eq!(shape.name().as_bytes(), b"hi");
    assert_eq!((origin.x(), origin.y()), (3, 4));
    assert_eq!(shape.kind(), Kind::CIRCLE);
    assert_eq!(sides_sum, 12);

    // Malformed input fails cleanly, never panics/UB: a buffer truncated mid-record is a decode error.
    assert!(
        Shape::decode(&SHAPE_WIRE[..5], &arena).is_err(),
        "truncated input should be rejected, not accepted"
    );

    println!(
        "consumer-example: decoded demo.Shape {{ name={:?}, origin=({}, {}), kind={:?}, sides sum={} }}",
        shape.name(),
        origin.x(),
        origin.y(),
        shape.kind(),
        sides_sum,
    );
}
