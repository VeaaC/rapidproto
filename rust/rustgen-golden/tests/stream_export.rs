//! Drives `export_stream_macros` output from OUTSIDE the crate that `include!`s it -- this `tests/`
//! crate, the case the option exists for (as is a crate depending on a shared proto crate). The
//! `streamexport` golden is proto3 generated stream-only with exported macros. The default output's
//! macros are `pub(crate)`, so none of these calls would resolve from here without the option.

use rapidproto::stream::Flow;
use rustgen_golden::streamexport::stream::p3::*;

// Msg { implicit_i = 42, name = "hello", state = 1, nums = [1, 2, 3] (packed),
//       self = Msg { implicit_i = 7 }, a = 99 (oneof pick), field 99 = varint 5 (undeclared) }
const MSG: &[u8] = &[
    0x08, 0x2a, // implicit_i = 42 (field 1)
    0x1a, 0x05, b'h', b'e', b'l', b'l', b'o', // name = "hello" (field 3)
    0x20, 0x01, // state = 1 (field 4)
    0x32, 0x03, 0x01, 0x02, 0x03, // nums = [1, 2, 3] packed (field 6)
    0x2a, 0x02, 0x08, 0x07, // self = Msg { implicit_i = 7 } (field 5)
    0x50, 0x63, // pick.a = 99 (field 10)
    0x98, 0x06, 0x05, // field 99 = 5 (undeclared)
];

#[test]
fn plain_form_reaches_every_kind_of_value_type() {
    // A string (`ProtoStr`), an enum newtype (a foreign-type alias), packed elements, a recursive
    // child handle (the nested call is a second cross-crate expansion) and the unknown handler.
    let (mut name, mut state, mut nums, mut inner, mut unknown) =
        (String::new(), -1i32, Vec::new(), 0i32, 0u32);
    Msg::new(MSG)
        .decode(msg_stream! {
            name => |n| name = n.to_str().unwrap().to_owned(),
            state => |s| state = s.0,
            nums => |v| nums.push(v),
            self_ => |sub| sub.decode(msg_stream! { implicit_i => |v| inner = v }),
            unknown => |u| unknown = u.field_number(),
        })
        .unwrap();
    assert_eq!(name, "hello");
    assert_eq!(state, 1);
    assert_eq!(nums, vec![1, 2, 3]);
    assert_eq!(inner, 7);
    assert_eq!(unknown, 99);
}

#[test]
fn shared_state_form_threads_one_accumulator_across_the_nested_walk() {
    #[derive(Default)]
    struct Acc {
        i: i32,
        nums_sum: i32,
        inner_i: i32,
        a: i32,
    }
    let mut acc = Acc::default();
    Msg::new(MSG)
        .decode(msg_stream!(&mut acc; {
            implicit_i => |s, v| s.i = v,
            nums => |s, v| s.nums_sum += v,
            self_ => |s, sub| sub.decode(msg_stream!(&mut *s; { implicit_i => |s, v| s.inner_i = v })),
            a => |s, v| { s.a = v; Flow::Stop },
        }))
        .unwrap();
    assert_eq!((acc.i, acc.nums_sum, acc.inner_i, acc.a), (42, 6, 7, 99));
}
