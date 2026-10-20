// A minimal consumer of the generated decoders. `build.rs` turned `proto/message.proto` into Rust at
// build time; here we include that code and decode the same message with BOTH models: the arena tree
// (materialize, then read borrowed accessors) and the streaming visitor (one callback per field,
// nothing materialized). The streaming pass also shows the mid-decode hybrid -- stream the outer
// message, but materialize a chosen sub-message with the arena model from its bytes.

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

use rapidproto::{Arena, DecodeError};
use schema::arena::demo::{Point as ArenaPoint, Shape as ArenaShape};
use schema::common::demo::{Kind, KindClass};
// Glob the stream module: it brings the reader handle, the `shape_stream!` macro, the visitor trait,
// and the re-exported value types the macro names -- all of which the macro expansion needs in scope
// at this call site (the same idiom a consumer uses for any generated stream module).
use schema::stream::demo::*;

// Encoded `demo.Shape { name: "hi", origin: { x: 3, y: 4 }, kind: KIND_CIRCLE, sides: [3, 4, 5] }`,
// plus one field the schema does not define (decoding skips it).
const SHAPE_WIRE: &[u8] = &[
    0x0A, 0x02, b'h', b'i', 0x12, 0x04, 0x08, 0x03, 0x10, 0x04, 0x18, 0x01, 0x22, 0x03, 0x03, 0x04,
    0x05, 0x98, 0x06, 0x2A,
];

fn main() {
    // A decoded message borrows from the arena, so the arena must outlive it.
    let arena = Arena::new();
    let shape = ArenaShape::decode(SHAPE_WIRE, &arena).expect("decode demo.Shape");

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
    assert!(ArenaShape::decode(&SHAPE_WIRE[..5], &arena).is_err());

    println!(
        "decoded demo.Shape {{ name={:?}, origin=({}, {}), kind={kind}, sides sum={sides_sum} }}",
        shape.name(),
        origin.x(),
        origin.y(),
    );

    // --- The streaming model: visit each field as it is decoded, materializing nothing. The macro's
    // shared-state form lends one `&mut parts` accumulator to every handler, so the whole message
    // parses into a struct in a few lines. `origin` uses the mid-decode HYBRID: the stream hands the
    // sub-message over as a reader handle, and `bytes()` feeds the ARENA decoder to materialize just it.
    #[derive(Default)]
    struct ShapeParts {
        name: String,
        x: i32,
        y: i32,
        kind: i32,
        sides_sum: i32,
    }
    let mut parts = ShapeParts::default();
    Shape::new(SHAPE_WIRE)
        .decode(shape_stream!(&mut parts; {
            name => |p, n| p.name = n.to_str().unwrap().to_owned(),
            origin => |p, pt| {
                let sub_arena = Arena::new();
                let point = ArenaPoint::decode(pt.bytes(), &sub_arena)?;
                p.x = point.x();
                p.y = point.y();
                Ok::<_, DecodeError>(())
            },
            kind => |p, k| p.kind = k.0, // the open-enum newtype; `.0` is the raw i32
            sides => |p, v| p.sides_sum += v,
        }))
        .expect("stream demo.Shape");

    // The two models agree field-for-field.
    assert_eq!(parts.name, "hi");
    assert_eq!((parts.x, parts.y), (3, 4));
    assert_eq!(parts.kind, Kind::CIRCLE.0);
    assert_eq!(parts.sides_sum, sides_sum);
    println!("streamed the same demo.Shape; its fields agree with the arena decode");

    // With the optional `serde` feature (serialize-only; RapidProto stays decode-only), a decoded
    // message implements `serde::Serialize`, so any serde backend can serialize it. Run with
    // `--features serde` to also print the shape as JSON. (serde_json can't represent a few protobuf
    // shapes — non-string map keys, non-finite floats — but demo.Shape has none.)
    #[cfg(feature = "serde")]
    {
        let json = serde_json::to_string(shape).expect("serialize demo.Shape");
        println!("as JSON: {json}");
    }
}
