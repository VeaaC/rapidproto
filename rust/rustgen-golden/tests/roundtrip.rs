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
