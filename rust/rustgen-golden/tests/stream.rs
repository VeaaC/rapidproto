//! Decode round-trip for the streaming decoder, over the SAME corpus schema the arena decode test
//! uses (`proto3`'s `p3.Msg`) -- there are no Rust-local fixture protos. Every corpus golden now
//! carries both an `arena` and a `stream` tree (see tests/test_rustgen.cpp); this exercises the
//! generated `stream` builders: subset selection with the rest skipped on the wire, a packed repeated
//! field fired per element, an enum newtype, a recursive sub-message delivered as a configurable
//! sub-builder, a oneof member selected individually, and an early `Flow::Stop`. The `std` feature
//! gates it (it builds owned `String`s/`Vec`s), as the arena decode test's owned paths are.
#![cfg(feature = "std")]

use rapidproto::stream::Flow;
use rustgen_golden::proto3::stream::p3::Msg;

// Msg { implicit_i = 42, name = "hello", state = 1, nums = [1, 2, 3] (packed),
//       self = Msg { implicit_i = 7 }, a = 99 (oneof pick) }
const MSG: &[u8] = &[
    0x08, 0x2a, // implicit_i = 42 (field 1)
    0x1a, 0x05, b'h', b'e', b'l', b'l', b'o', // name = "hello" (field 3)
    0x20, 0x01, // state = 1 (field 4)
    0x32, 0x03, 0x01, 0x02, 0x03, // nums = [1, 2, 3] packed (field 6)
    0x2a, 0x02, 0x08, 0x07, // self = Msg { implicit_i = 7 } (field 5)
    0x50, 0x63, // pick.a = 99 (field 10)
];

#[test]
fn selects_a_subset_and_skips_the_rest() {
    let mut implicit_i = 0i32;
    let mut name = String::new();
    let mut state = -1i32;
    Msg::stream(MSG)
        .on_implicit_i(|v| implicit_i = v)
        .on_name(|n| name = n.to_str().unwrap().to_owned())
        .on_state(|s| state = s.0)
        .decode()
        .unwrap();
    assert_eq!(implicit_i, 42);
    assert_eq!(name, "hello");
    assert_eq!(state, 1); // the enum newtype's raw i32
}

#[test]
fn packed_repeated_fires_once_per_element() {
    let mut nums = Vec::new();
    Msg::stream(MSG).on_nums(|v| nums.push(v)).decode().unwrap();
    assert_eq!(nums, vec![1, 2, 3]);
}

#[test]
fn recursive_submessage_arrives_as_a_subbuilder() {
    let mut inner_i = 0i32;
    Msg::stream(MSG)
        .on_self(|sub| {
            sub.on_implicit_i(|v| inner_i = v).decode().unwrap();
        })
        .decode()
        .unwrap();
    assert_eq!(inner_i, 7);
}

#[test]
fn oneof_member_is_individually_selectable() {
    let mut a = 0i32;
    Msg::stream(MSG).on_a(|v| a = v).decode().unwrap();
    assert_eq!(a, 99);
}

#[test]
fn flow_stop_halts_early() {
    let mut seen = Vec::new();
    Msg::stream(MSG)
        .on_nums(|v| {
            seen.push(v);
            Flow::Stop
        })
        .decode()
        .unwrap();
    assert_eq!(seen, vec![1]); // stopped after the first packed element
}
