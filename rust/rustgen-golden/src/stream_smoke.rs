//! Decode round-trip for the streaming decoder, over the SAME corpus schema the arena decode test
//! uses (`proto3`'s `p3.Msg`) -- there are no Rust-local fixture protos. Every corpus golden carries
//! both an `arena` and a `stream` tree (see tests/test_rustgen.cpp). This is an IN-CRATE test module
//! (wired into the generated lib.rs), not a `tests/` integration test, because the generated
//! `*_stream!` macros are `pub(crate) use`-exported -- reachable within the including crate (the normal
//! consumer case: generated code is `include!`d into the consumer's own crate) but not cross-crate.
//!
//! It drives the generated visitor via the per-message `msg_stream!` macro (closures capturing locals):
//! subset selection with the rest skipped, a packed repeated field fired per element, an enum delivered
//! as its typed newtype, a oneof member selected individually, a recursive sub-message handed its typed
//! reader handle (`sub.decode(..)`), error propagation from a malformed sub-message out through the
//! outer decode, the `unknown`-field handler, `Flow::Stop`, and a hand-implemented visitor.
//!
//! A `use crate::proto3::stream::p3::*;` glob brings the handle, visitor trait, macro, and the enum
//! newtypes the macro names (re-exported into the stream module) into scope together.
#![cfg(feature = "std")]

use crate::proto3::stream::p3::*;
use rapidproto::stream::Flow;
use rapidproto::DecodeError;

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
    Msg::new(MSG)
        .decode(msg_stream! {
            implicit_i => |v| implicit_i = v,
            name => |n| name = n.to_str().unwrap().to_owned(),
            state => |s| state = s.0, // the enum newtype (State), aligned with the arena model
        })
        .unwrap();
    assert_eq!(implicit_i, 42);
    assert_eq!(name, "hello");
    assert_eq!(state, 1);
}

#[test]
fn packed_repeated_fires_once_per_element() {
    let mut nums = Vec::new();
    Msg::new(MSG)
        .decode(msg_stream! { nums => |v| nums.push(v) })
        .unwrap();
    assert_eq!(nums, vec![1, 2, 3]);
}

#[test]
fn recursive_submessage_is_a_typed_handle() {
    // `self` is a keyword, so its slot (and macro key) escapes to `self_`; it delivers a `Msg<'a>`
    // reader handle the consumer recurses into with its own visitor.
    let mut inner_i = 0i32;
    Msg::new(MSG)
        .decode(msg_stream! {
            self_ => |sub| sub.decode(msg_stream! { implicit_i => |v| inner_i = v }),
        })
        .unwrap();
    assert_eq!(inner_i, 7);
}

#[test]
fn a_malformed_submessage_error_propagates() {
    // self (field 5) = a 1-byte payload `[0x08]`: a varint tag with no value -> the nested decode
    // errors, and the `?`-less `sub.decode(..)` result flows out of the outer decode as Err.
    const BAD: &[u8] = &[0x2a, 0x01, 0x08];
    let err = Msg::new(BAD)
        .decode(msg_stream! {
            self_ => |sub| sub.decode(msg_stream! { implicit_i => |v| { let _ = v; } }),
        })
        .unwrap_err();
    let _: DecodeError = err;
}

#[test]
fn oneof_member_is_individually_selectable() {
    let mut a = 0i32;
    Msg::new(MSG)
        .decode(msg_stream! { a => |v| a = v })
        .unwrap();
    assert_eq!(a, 99);
}

#[test]
fn unknown_fields_reach_the_unknown_handler() {
    // implicit_i = 7 (field 1, known), then field 99 (undeclared) = varint 42.
    const WITH_UNKNOWN: &[u8] = &[0x08, 0x07, 0x98, 0x06, 0x2a];
    let mut seen = None;
    Msg::new(WITH_UNKNOWN)
        .decode(msg_stream! {
            unknown => |u| seen = Some((u.field_number(), u.bytes().to_vec())),
        })
        .unwrap();
    assert_eq!(seen, Some((99, vec![0x2au8]))); // the known field 1 was skipped, not delivered
}

#[test]
fn flow_stop_halts_early() {
    let mut seen = Vec::new();
    Msg::new(MSG)
        .decode(msg_stream! {
            nums => |v| { seen.push(v); Flow::Stop },
        })
        .unwrap();
    assert_eq!(seen, vec![1]); // stopped after the first packed element
}

#[test]
fn a_visitor_type_can_be_hand_implemented() {
    // The macro is sugar over the generated trait; a consumer may implement it directly.
    struct Count(u32);
    impl<'a> MsgVisitor<'a> for &mut Count {
        const WANT_nums: bool = true;
        fn on_nums(&mut self, _v: i32) -> Result<Flow, DecodeError> {
            self.0 += 1;
            Ok(Flow::Continue)
        }
    }
    let mut c = Count(0);
    Msg::new(MSG).decode(&mut c).unwrap();
    assert_eq!(c.0, 3);
}
