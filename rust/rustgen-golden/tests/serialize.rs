//! Exercises the generated `serde::Serialize` impls through the differential dump serializer
//! (`rapidproto-diff`), asserting the exact one-line JSON contract the Python differential compares.
//! Compiled only with `--features serde` (the generated impls are behind that feature).
//!
//! This is the fast in-tree check that the emitter + the dump serializer agree on the contract;
//! `tests/differential.py`'s Rust arm is the schema-sweeping enforcement against protobuf.
#![cfg(feature = "serde")]

use rapidproto::Arena;
use rapidproto_diff::to_line;

use rustgen_golden::proto3::arena::p3::Msg;

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
fn put_var(b: &mut Vec<u8>, field: u32, v: u64) {
    put_tag(b, field, 0);
    put_varint(b, v);
}
fn put_len(b: &mut Vec<u8>, field: u32, payload: &[u8]) {
    put_tag(b, field, 2);
    put_varint(b, payload.len() as u64);
    b.extend_from_slice(payload);
}

#[test]
fn proto3_present_fields_only_in_declaration_order() {
    // implicit_i=300 (implicit int, non-zero -> emitted), explicit_i absent (-> skipped), name="hi",
    // state=ON(1) (implicit enum as its i32), self absent, nums=[1,2] (packed repeated).
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 300);
    put_len(&mut buf, 3, b"hi");
    put_var(&mut buf, 4, 1);
    let mut packed = Vec::new();
    put_varint(&mut packed, 1);
    put_varint(&mut packed, 2);
    put_len(&mut buf, 6, &packed);

    let arena = Arena::new();
    let msg = Msg::decode(&buf, &arena).unwrap();
    let json = to_line(msg).unwrap();

    // Only present fields, keyed by PROTO names, in field order; enum as its number; repeated array.
    assert_eq!(
        json,
        r#"{"implicit_i":300,"name":"hi","state":1,"nums":[1,2]}"#
    );
}

#[test]
fn proto3_empty_message_is_empty_object() {
    let arena = Arena::new();
    let msg = Msg::decode(&[], &arena).unwrap();
    assert_eq!(to_line(msg).unwrap(), "{}");
}

#[test]
fn proto3_default_valued_implicit_fields_are_omitted() {
    // implicit_i=0 and state=UNKNOWN(0) on the wire: implicit defaults are omitted, as protobuf JSON
    // does, so the object stays empty.
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 0);
    put_var(&mut buf, 4, 0);
    let arena = Arena::new();
    let msg = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(to_line(msg).unwrap(), "{}");
}
