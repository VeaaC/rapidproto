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
//! outer decode, the `unknown`-field handler, `Flow::Stop`, and a hand-implemented visitor. The
//! shared-state form `msg_stream!(&mut state; { field => |state, value| .. })` is covered too: many
//! fields aggregating into one `&mut` accumulator, re-threaded across a nested `sub.decode(..)`.
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
fn map_delivers_key_value_per_entry() {
    // `counts` is field 9, map<string, int32>. Three LEN entries: ("a", 1), ("bb", 2), and ("z", _)
    // which omits field 2 -> the value defaults to 0 (the absent-value rule).
    const MAP_MSG: &[u8] = &[
        0x4a, 0x05, 0x0a, 0x01, b'a', 0x10, 0x01, // counts["a"] = 1
        0x4a, 0x06, 0x0a, 0x02, b'b', b'b', 0x10, 0x02, // counts["bb"] = 2
        0x4a, 0x03, 0x0a, 0x01, b'z', // counts["z"] = <absent> -> 0
    ];
    let mut entries: Vec<(String, i32)> = Vec::new();
    Msg::new(MAP_MSG)
        .decode(msg_stream! {
            counts => |(k, v)| entries.push((k.to_str().unwrap().to_owned(), v)),
        })
        .unwrap();
    assert_eq!(
        entries,
        vec![
            ("a".to_owned(), 1),
            ("bb".to_owned(), 2),
            ("z".to_owned(), 0)
        ]
    );
}

#[test]
fn map_with_message_value_delivers_a_child_handle() {
    // `al.Layout.grid` is field 16, map<int32, Point> -- a message value delivered as the child's
    // reader handle, which the consumer recurses into with `point_stream!`. One entry: 7 -> {x=3, y=4}.
    use crate::arena_layout::stream::al::*;
    const LAYOUT: &[u8] = &[
        0x82, 0x01, // field 16, wire LEN (two-byte tag)
        0x08, // entry length = 8
        0x08, 0x07, // key (field 1) = 7
        0x12, 0x04, 0x08, 0x03, 0x10, 0x04, // value (field 2) = Point { x=3, y=4 }
    ];
    let mut seen: Vec<(i32, i32, i32)> = Vec::new();
    Layout::new(LAYOUT)
        .decode(layout_stream! {
            grid => |(k, pt)| {
                let (mut x, mut y) = (0i32, 0i32);
                pt.decode(point_stream! { x => |v| x = v, y => |v| y = v })?;
                seen.push((k, x, y));
                Ok::<_, DecodeError>(())
            },
        })
        .unwrap();
    assert_eq!(seen, vec![(7, 3, 4)]);
}

#[test]
fn shared_state_form_aggregates_many_fields() {
    // The whole point of the state form: several handlers mutate ONE accumulator, which exclusive-&mut
    // closure captures cannot express. No type annotations on the `|s, v|` parameters.
    #[derive(Default)]
    struct Acc {
        i: i32,
        name_len: usize,
        nums_sum: i32,
        inner_i: i32,
    }
    let mut acc = Acc::default();
    Msg::new(MSG)
        .decode(msg_stream!(&mut acc; {
            implicit_i => |a, v| a.i = v,
            name => |a, n| a.name_len = n.to_str().unwrap().len(),
            nums => |a, v| a.nums_sum += v,
            // the SAME accumulator re-threaded into the nested decode via `&mut *a`
            self_ => |a, sub| sub.decode(msg_stream!(&mut *a; { implicit_i => |a, v| a.inner_i = v })),
        }))
        .unwrap();
    assert_eq!(acc.i, 42);
    assert_eq!(acc.name_len, 5);
    assert_eq!(acc.nums_sum, 6);
    assert_eq!(acc.inner_i, 7);
}

#[test]
fn shared_state_form_handles_stop_and_maps() {
    // Flow::Stop out of a state handler halts; a map entry arrives as `(key, value)` beside the state.
    let mut sum = 0i32;
    Msg::new(MSG)
        .decode(msg_stream!(&mut sum; {
            nums => |s, v| { *s += v; Flow::Stop },
        }))
        .unwrap();
    assert_eq!(sum, 1); // stopped after the first packed element

    const MAP_MSG: &[u8] = &[
        0x4a, 0x05, 0x0a, 0x01, b'a', 0x10, 0x01, // counts["a"] = 1
        0x4a, 0x06, 0x0a, 0x02, b'b', b'b', 0x10, 0x02, // counts["bb"] = 2
    ];
    let mut total = 0i32;
    Msg::new(MAP_MSG)
        .decode(msg_stream!(&mut total; {
            counts => |s, (_k, v)| *s += v,
        }))
        .unwrap();
    assert_eq!(total, 3);
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
