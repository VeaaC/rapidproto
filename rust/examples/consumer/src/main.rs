// A minimal consumer of the generated decoders. `build.rs` turned `proto/message.proto` into Rust at
// build time; here we include that code and decode a message with it.

// The generated module: one file covers the whole schema (imports included).
#[allow(
    clippy::all,
    unused,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod schema {
    include!(concat!(env!("OUT_DIR"), "/message.rs"));
}

use rapidproto::Arena;
use schema::arena::demo::Shape;
use schema::common::demo::{Kind, KindClass};

// Encoded `demo.Shape { name: "hi", origin: { x: 3, y: 4 }, kind: KIND_CIRCLE, sides: [3, 4, 5] }`,
// plus one field the schema does not define (decoding skips it).
const SHAPE_WIRE: &[u8] = &[
    0x0A, 0x02, b'h', b'i', 0x12, 0x04, 0x08, 0x03, 0x10, 0x04, 0x18, 0x01, 0x22, 0x03, 0x03, 0x04,
    0x05, 0x98, 0x06, 0x2A,
];

fn main() {
    // A decoded message borrows from the arena, so the arena must outlive it.
    let arena = Arena::new();
    let shape = Shape::decode(SHAPE_WIRE, &arena).expect("decode demo.Shape");

    assert_eq!(shape.name().as_bytes(), b"hi");
    let origin = shape.origin().expect("origin is set"); // a nested message, from types.proto
    assert_eq!(shape.kind(), Kind::CIRCLE);
    let sides_sum: i32 = shape.sides().iter().sum(); // a repeated field is a slice
    assert_eq!(sides_sum, 12);

    // An enum field is an open i32 wrapper (it can hold values added by newer schemas). Call
    // `classify()` to match on the known variants, with `Unknown(i32)` catching the rest.
    let kind = match shape.kind().classify() {
        KindClass::Circle => "circle",
        KindClass::Square => "square",
        _ => "other",
    };

    // Invalid input returns an error rather than panicking.
    assert!(Shape::decode(&SHAPE_WIRE[..5], &arena).is_err());

    println!(
        "decoded demo.Shape {{ name={:?}, origin=({}, {}), kind={kind}, sides sum={sides_sum} }}",
        shape.name(),
        origin.x(),
        origin.y(),
    );
}
