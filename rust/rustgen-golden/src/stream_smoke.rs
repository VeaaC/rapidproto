//! Decode round-trip for the streaming decoder, over the SAME corpus schema the arena decode test
//! uses (`proto3`'s `p3.Msg`) -- there are no Rust-local fixture protos. Every corpus golden carries
//! both an `arena` and a `stream` tree (see tests/test_rustgen.cpp). This is an IN-CRATE test module
//! (wired into the generated lib.rs), not a `tests/` integration test, because the generated
//! `*_stream!` macros are `pub(crate) use`-exported -- reachable within the including crate (the normal
//! consumer case: generated code is `include!`d into the consumer's own crate) but not across a crate
//! boundary. It drives the generated visitor: the `msg_stream!` macro builds an anonymous visitor from
//! `field => closure` pairs capturing locals, unselected fields are skipped on the wire, a packed
//! repeated field fires per element, an enum newtype and a oneof member are selected individually, a
//! recursive sub-message is handed its payload bytes (recurse with the child decoder), `Flow::Stop`
//! halts early, and the trait is hand-implementable as the primitive the macro sugars.

use rapidproto::stream::Flow;
// The visitor trait + decode marker + builder macro for p3.Msg. The macro's expansion names the trait
// by bare name, so it must be in scope -- brought in here alongside the decode marker.
use crate::proto3::stream::p3::{msg_stream, Msg, MsgVisitor};

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
    Msg::decode(
        MSG,
        msg_stream! {
            implicit_i => |v| implicit_i = v,
            name => |n: rapidproto::ProtoStr| name = n.to_str().unwrap().to_owned(),
            state => |s| state = s, // an enum is delivered as its raw i32 in streaming
        },
    )
    .unwrap();
    assert_eq!(implicit_i, 42);
    assert_eq!(name, "hello");
    assert_eq!(state, 1);
}

#[test]
fn packed_repeated_fires_once_per_element() {
    let mut nums = Vec::new();
    Msg::decode(MSG, msg_stream! { nums => |v| nums.push(v) }).unwrap();
    assert_eq!(nums, vec![1, 2, 3]);
}

#[test]
fn recursive_submessage_delivers_payload_bytes() {
    // `self` is a keyword, so its slot (and macro key) escapes to `self_`.
    let mut inner_i = 0i32;
    Msg::decode(
        MSG,
        msg_stream! {
            self_ => |bytes: &[u8]| {
                Msg::decode(bytes, msg_stream! { implicit_i => |v| inner_i = v }).unwrap();
            },
        },
    )
    .unwrap();
    assert_eq!(inner_i, 7);
}

#[test]
fn oneof_member_is_individually_selectable() {
    let mut a = 0i32;
    Msg::decode(MSG, msg_stream! { a => |v| a = v }).unwrap();
    assert_eq!(a, 99);
}

#[test]
fn flow_stop_halts_early() {
    let mut seen = Vec::new();
    Msg::decode(
        MSG,
        msg_stream! {
            nums => |v| { seen.push(v); Flow::Stop },
        },
    )
    .unwrap();
    assert_eq!(seen, vec![1]); // stopped after the first packed element
}

#[test]
fn a_visitor_type_can_be_hand_implemented() {
    // The macro is sugar over the generated trait; a consumer may implement it directly.
    struct Count(u32);
    impl<'a> MsgVisitor<'a> for &mut Count {
        const WANT_nums: bool = true;
        fn on_nums(&mut self, _v: i32) -> Flow {
            self.0 += 1;
            Flow::Continue
        }
    }
    let mut c = Count(0);
    Msg::decode(MSG, &mut c).unwrap();
    assert_eq!(c.0, 3);
}
