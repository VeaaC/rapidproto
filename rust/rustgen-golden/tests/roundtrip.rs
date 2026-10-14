//! Decode round-trip tests for the Rust generator's scalar-message output (M1.4 slice 1).
//!
//! `fixtures/scalars_fixture.rs` is generated from `rp_scalars.proto` (see that file for the regen
//! command) and included here directly, so these tests exercise the ACTUAL emitted `decode` body over
//! hand-encoded protobuf wire bytes covering every scalar type, implicit and explicit presence, and
//! the error/skip edges.
#![allow(dead_code)]

include!("fixtures/scalars_fixture.rs");

use rapidproto::{Arena, ErrorKind};

type Scalars<'a> = arena::rp::scalars::Scalars<'a>;
use common::rp::scalars::Shade;

// --- minimal protobuf wire encoders (the decoder is the system under test, not these) ---
fn put_varint(b: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 {
        b.push(0x80 | (v & 0x7f) as u8);
        v >>= 7;
    }
    b.push(v as u8);
}
fn put_tag(b: &mut Vec<u8>, field: u32, wire: u32) {
    put_varint(b, (u64::from(field) << 3) | u64::from(wire));
}
fn put_len(b: &mut Vec<u8>, field: u32, payload: &[u8]) {
    put_tag(b, field, 2);
    put_varint(b, payload.len() as u64);
    b.extend_from_slice(payload);
}

#[test]
fn round_trips_every_scalar_type() {
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 0);
    put_varint(&mut buf, 300); // a_i32
    put_tag(&mut buf, 2, 0);
    put_varint(&mut buf, (-5i64) as u64); // a_i64 (negative -> 10-byte varint)
    put_tag(&mut buf, 3, 0);
    put_varint(&mut buf, 4_000_000_000); // a_u32 (> i32::MAX)
    put_tag(&mut buf, 4, 0);
    put_varint(&mut buf, 1_000_000_000_000_000_000); // a_u64
    put_tag(&mut buf, 5, 0);
    put_varint(&mut buf, 13); // a_s32: zigzag(-7) == 13
    put_tag(&mut buf, 6, 0);
    put_varint(&mut buf, 199); // a_s64: zigzag(-100) == 199
    put_tag(&mut buf, 7, 5);
    buf.extend_from_slice(&0xDEAD_BEEFu32.to_le_bytes()); // a_f32 (fixed32)
    put_tag(&mut buf, 8, 1);
    buf.extend_from_slice(&0x0102_0304_0506_0708u64.to_le_bytes()); // a_f64 (fixed64)
    put_tag(&mut buf, 9, 5);
    buf.extend_from_slice(&(-2i32).to_le_bytes()); // a_sf32
    put_tag(&mut buf, 10, 1);
    buf.extend_from_slice(&(-3i64).to_le_bytes()); // a_sf64
    put_tag(&mut buf, 11, 5);
    buf.extend_from_slice(&1.5f32.to_bits().to_le_bytes()); // a_float
    put_tag(&mut buf, 12, 1);
    buf.extend_from_slice(&2.25f64.to_bits().to_le_bytes()); // a_double
    put_tag(&mut buf, 13, 0);
    put_varint(&mut buf, 1); // a_bool
    put_len(&mut buf, 14, b"hi"); // a_str
    put_len(&mut buf, 15, &[0xDE, 0xAD]); // a_bytes
    put_tag(&mut buf, 16, 0);
    put_varint(&mut buf, 2); // a_enum = SHADE_TWO (field 16 -> a 2-byte tag)
    put_tag(&mut buf, 17, 0);
    put_varint(&mut buf, 42); // opt_i32
    put_len(&mut buf, 18, b"opt"); // opt_str

    let arena = Arena::new();
    let m = Scalars::decode(&buf, &arena).unwrap();
    assert_eq!(m.a_i32(), 300);
    assert_eq!(m.a_i64(), -5);
    assert_eq!(m.a_u32(), 4_000_000_000);
    assert_eq!(m.a_u64(), 1_000_000_000_000_000_000);
    assert_eq!(m.a_s32(), -7);
    assert_eq!(m.a_s64(), -100);
    assert_eq!(m.a_f32(), 0xDEAD_BEEF);
    assert_eq!(m.a_f64(), 0x0102_0304_0506_0708);
    assert_eq!(m.a_sf32(), -2);
    assert_eq!(m.a_sf64(), -3);
    assert_eq!(m.a_float(), 1.5);
    assert_eq!(m.a_double(), 2.25);
    assert!(m.a_bool());
    assert_eq!(m.a_str().as_bytes(), b"hi");
    assert_eq!(m.a_bytes(), b"\xDE\xAD");
    assert_eq!(m.a_enum(), Shade::TWO);
    assert_eq!(m.opt_i32(), Some(42));
    assert_eq!(m.opt_str().map(|s| s.as_bytes()), Some(&b"opt"[..]));
}

#[test]
fn absent_fields_decode_to_defaults() {
    let arena = Arena::new();
    let m = Scalars::decode(&[], &arena).unwrap();
    assert_eq!(m.a_i32(), 0);
    assert_eq!(m.a_u64(), 0);
    assert_eq!(m.a_double(), 0.0);
    assert!(!m.a_bool());
    assert_eq!(m.a_str().as_bytes(), b"");
    assert_eq!(m.a_bytes(), b"");
    assert_eq!(m.a_enum(), Shade::ZERO);
    assert_eq!(m.opt_i32(), None); // explicit presence: absent is None
    assert_eq!(m.opt_str(), None);
}

#[test]
fn unknown_and_wrong_wire_fields_are_skipped() {
    let mut buf = Vec::new();
    put_tag(&mut buf, 99, 0);
    put_varint(&mut buf, 7); // unknown varint field
    put_len(&mut buf, 100, b"junk"); // unknown length-delimited field
                                     // field 1 (a_i32) arriving as LEN instead of Varint: wrong wire type -> skipped like an unknown.
    put_len(&mut buf, 1, b"xyz");
    put_tag(&mut buf, 13, 0);
    put_varint(&mut buf, 1); // a_bool, a real field after the junk

    let arena = Arena::new();
    let m = Scalars::decode(&buf, &arena).unwrap();
    assert_eq!(
        m.a_i32(),
        0,
        "wrong-wire-type field 1 must be skipped, leaving the default"
    );
    assert!(
        m.a_bool(),
        "the real field after the skipped junk must still decode"
    );
}

#[test]
fn last_occurrence_wins_for_a_singular_scalar() {
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 0);
    put_varint(&mut buf, 1);
    put_tag(&mut buf, 1, 0);
    put_varint(&mut buf, 2);
    let arena = Arena::new();
    let m = Scalars::decode(&buf, &arena).unwrap();
    assert_eq!(m.a_i32(), 2);
}

#[test]
fn truncated_value_is_an_error_at_a_body_relative_offset() {
    // field 2 tag (1 byte) then a lone continuation byte: the varint value is truncated.
    let buf = [0x10u8, 0x80];
    let arena = Arena::new();
    let err = match Scalars::decode(&buf, &arena) {
        Ok(_) => panic!("expected a decode error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::Truncated);
    assert_eq!(
        err.offset(),
        1,
        "offset is where the value began, relative to the body"
    );
}

#[test]
fn malformed_tag_is_an_error() {
    // A field-0 tag (0x00) is invalid.
    let arena = Arena::new();
    let err = match Scalars::decode(&[0x00u8], &arena) {
        Ok(_) => panic!("expected a decode error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::InvalidTag);
    assert_eq!(err.offset(), 0);
}

type Aggregate<'a> = arena::rp::scalars::Aggregate<'a>;
type Tree<'a> = arena::rp::scalars::Tree<'a>;

#[test]
fn singular_and_repeated_messages_and_strings() {
    let mut nested = Vec::new();
    put_tag(&mut nested, 1, 0);
    put_varint(&mut nested, 7);
    put_len(&mut nested, 2, b"n");

    let mut n1 = Vec::new();
    put_tag(&mut n1, 1, 0);
    put_varint(&mut n1, 1);
    let mut n2 = Vec::new();
    put_tag(&mut n2, 1, 0);
    put_varint(&mut n2, 2);

    let mut buf = Vec::new();
    put_len(&mut buf, 1, &nested); // one = Nested{7,"n"}
    put_len(&mut buf, 2, &n1); // many[0]
    put_len(&mut buf, 2, &n2); // many[1]
    put_len(&mut buf, 4, b"a"); // tags[0]
    put_len(&mut buf, 4, b"bb"); // tags[1]

    let arena = Arena::new();
    let a = Aggregate::decode(&buf, &arena).unwrap();
    let one = a.one().expect("one present");
    assert_eq!(one.x(), 7);
    assert_eq!(one.label().as_bytes(), b"n");
    assert_eq!(a.many().len(), 2);
    assert_eq!(a.many()[0].x(), 1);
    assert_eq!(a.many()[1].x(), 2);
    assert_eq!(a.tags().len(), 2);
    assert_eq!(a.tags()[0].as_bytes(), b"a");
    assert_eq!(a.tags()[1].as_bytes(), b"bb");
    // Absent repeated/message fields are empty / None.
    assert!(a.nums().is_empty());
}

#[test]
fn repeated_scalar_accepts_both_packed_and_unpacked() {
    // Unpacked: field 3 (varint) repeated. Enum field 5 unpacked too.
    let mut buf = Vec::new();
    for v in [5u64, 6, 7] {
        put_tag(&mut buf, 3, 0);
        put_varint(&mut buf, v);
    }
    put_tag(&mut buf, 5, 0);
    put_varint(&mut buf, 1);
    put_tag(&mut buf, 5, 0);
    put_varint(&mut buf, 2);
    let arena = Arena::new();
    let a = Aggregate::decode(&buf, &arena).unwrap();
    assert_eq!(a.nums(), &[5, 6, 7]);
    assert_eq!(a.shades(), &[Shade::ONE, Shade::TWO]);

    // Packed: field 3 and field 5 as a single length-delimited run each.
    let mut nums = Vec::new();
    for v in [8u64, 9] {
        put_varint(&mut nums, v);
    }
    let mut shades = Vec::new();
    for v in [2u64, 1] {
        put_varint(&mut shades, v);
    }
    let mut buf2 = Vec::new();
    put_len(&mut buf2, 3, &nums);
    put_len(&mut buf2, 5, &shades);
    let arena2 = Arena::new();
    let a2 = Aggregate::decode(&buf2, &arena2).unwrap();
    assert_eq!(a2.nums(), &[8, 9]);
    assert_eq!(a2.shades(), &[Shade::TWO, Shade::ONE]);
}

#[test]
fn a_repeated_singular_message_is_rejected() {
    let mut n = Vec::new();
    put_tag(&mut n, 1, 0);
    put_varint(&mut n, 1);
    let mut buf = Vec::new();
    put_len(&mut buf, 1, &n); // one
    put_len(&mut buf, 1, &n); // one AGAIN -> error (matches C++, not protobuf merge)
    let arena = Arena::new();
    let err = match Aggregate::decode(&buf, &arena) {
        Ok(_) => panic!("expected a decode error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::RepeatedSingularMessage);
    assert_eq!(err.field_number(), Some(1));
}

#[test]
fn nested_message_recursion_is_depth_capped() {
    // Wrap an empty Tree in `levels` `child` (field 2) layers; the deepest decode_into runs at
    // depth == levels, and the cap rejects at depth > MAX_DECODE_DEPTH (100).
    fn nest(levels: u32) -> Vec<u8> {
        let mut buf = Vec::new();
        for _ in 0..levels {
            let inner = core::mem::take(&mut buf);
            put_len(&mut buf, 2, &inner);
        }
        buf
    }
    let arena = Arena::new();
    assert!(Tree::decode(&nest(100), &arena).is_ok());
    let err = match Tree::decode(&nest(101), &arena) {
        Ok(_) => panic!("expected a depth error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::DepthLimitExceeded);
}
