//! Decode round-trips for the streaming decoder over the goldens -- the streaming counterpart of
//! tests/decode.rs, over the schemas the C++ tests use (the global corpus, plus the wire-fixture schema
//! tests/wire_fixtures/wire_all.proto); there are no Rust-local fixture protos.
//! Every corpus golden carries both an `arena` and a `stream` tree and exports its `*_stream!` macros
//! (`Config::export_stream_macros`, prefix = its stem; see tests/test_rustgen.cpp), so this separate
//! test crate drives them the way a consumer's `tests/` does: `use rustgen_golden::<schema>::stream::..::*`.
//!
//! Covered through the per-message macros: subset selection with the rest skipped, a packed repeated
//! field fired per element, an enum delivered as its typed newtype, a oneof member selected
//! individually, a recursive sub-message handed its typed reader handle (`sub.decode(..)`), error
//! propagation from a malformed sub-message, the `unknown`-field handler, `Flow::Stop`, map entries
//! (scalar, bytes and message values), the field-order probes agreeing with the decode loop, and a
//! hand-implemented visitor; the shared-state form `msg_stream!(&mut state; { field => |state,
//! value| .. })` aggregating many fields into one accumulator re-threaded across nested decodes. The glob of a message's stream module brings in its
//! handle, trait and macro plus the hidden per-message scope the expansion names everything through, so
//! globbing several stream modules into one scope leaves every expansion unambiguous (the `globbing_*`
//! tests), and names shaped like that glue cannot collide with it (the gluenames test).
//!
//! Parity with the C++ streaming decoder's tests (tests/test_streamgen.cpp): every behavioral case
//! there has a pendant here (marked `Mirrors C++ "<case>"`) decoding the same schemas -- and, where the
//! C++ case decodes a checked-in protoc-encoded fixture (tests/wire_fixtures/*.bin), the very same
//! bytes. The C++ cases with no Rust pendant test what Rust has no counterpart of: the generated
//! headers matching their goldens, colliding identifiers, cross-file includes and the WKT closure (for
//! Rust, pinned by the goldens, which this crate compiles and tests/test_rustgen.cpp byte-compares),
//! emission order (Rust items need none); the namespace-prefix option (C++ only); the field catch-all
//! handler and the exact-match dispatch-gate traits (Rust has no catch-all -- the per-field visitor is
//! the selection mechanism -- and the type checker enforces each handler's value type).

use rapidproto::stream::Flow;
use rapidproto::wire::WireType;
use rapidproto::{DecodeError, ErrorKind};
use rustgen_golden::proto3::stream::p3::*;

// The protoc-encoded fixtures the C++ tests decode too (tests/wire_fixtures/generate.py; the
// `.txtpb` beside each is its source).
const SCALARS_BIN: &[u8] = include_bytes!("../../../tests/wire_fixtures/scalars.bin");
const MSG_BIN: &[u8] = include_bytes!("../../../tests/wire_fixtures/msg.bin");
const CONTAINER_BIN: &[u8] = include_bytes!("../../../tests/wire_fixtures/container.bin");
const ALL_WIRE_BIN: &[u8] = include_bytes!("../../../tests/wire_fixtures/all_wire.bin");

// Wire encoders for hand-built inputs (scaffolding, not the system under test).
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
/// A field-1..15 tag as a two-byte, non-minimal varint (valid protobuf; the reference parsers accept
/// it) instead of its canonical single byte.
fn put_overlong_tag(b: &mut Vec<u8>, field: u32, wire: u32) {
    let tag = (field << 3) | wire; // < 128 for field <= 15
    b.push(0x80 | tag as u8);
    b.push(0x00);
}

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
fn a_malformed_packed_element_errors_at_its_own_offset() {
    // nums (field 6), packed after another field so the payload starts mid-body: 1 and 300 arrive,
    // then a varint the payload cuts off is Truncated at its own offset, not the payload's or the
    // field's. After implicit_i (1) the run meets its field-order probe; after a (10), the loop's arm.
    for prefix in [[0x08, 0x2a], [0x50, 0x01]] {
        let mut input = prefix.to_vec();
        put_len(&mut input, 6, &[0x01, 0xac, 0x02, 0x80]);
        let mut nums = Vec::new();
        let err = Msg::new(&input)
            .decode(msg_stream! { nums => |v| nums.push(v) })
            .unwrap_err();
        assert_eq!(nums, vec![1, 300]);
        assert_eq!(
            (err.kind(), err.offset()),
            (ErrorKind::Truncated, input.len() - 1)
        );
    }

    // An over-long element (ten continuation bytes) errors at its first byte as well.
    let mut payload = vec![0x05];
    payload.extend([0x80; 10]);
    let mut input = Vec::new();
    put_len(&mut input, 6, &payload);
    let err = Msg::new(&input)
        .decode(msg_stream! { nums => |_| {} })
        .unwrap_err();
    assert_eq!(
        (err.kind(), err.offset()),
        (ErrorKind::VarintOverflow, input.len() - 10)
    );
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
fn an_unknown_handler_receives_only_non_schema_fields() {
    // Mirrors C++ "an unknown-field handler receives only non-schema fields". The known, unselected
    // fields follow an undeclared one, so the loop's catch-all, not a field-order probe, meets them.
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 7); // implicit_i: known, selected
    put_var(&mut buf, 99, 42); // undeclared
    put_var(&mut buf, 2, 20); // explicit_i: known, NOT selected
    put_len(&mut buf, 100, b"hi"); // undeclared, length-delimited
    put_len(&mut buf, 9, &[0x0a, 0x01, b'k', 0x10, 0x01]); // counts (map): known, NOT selected
    put_var(&mut buf, 10, 5); // a (oneof member): known, NOT selected
    #[derive(Default)]
    struct Seen {
        known: i32,
        numbers: Vec<u32>,
        varint_bytes: Vec<u8>,
        len_bytes: Vec<u8>,
    }
    let mut seen = Seen::default();
    Msg::new(&buf)
        .decode(msg_stream!(&mut seen; {
            implicit_i => |s, v| s.known = v,
            unknown => |s, u| {
                s.numbers.push(u.field_number());
                match u.field_number() {
                    99 => s.varint_bytes = u.bytes().to_vec(),
                    100 => {
                        assert_eq!(u.wire_type(), Some(WireType::Len));
                        s.len_bytes = u.bytes().to_vec();
                    }
                    _ => {}
                }
            },
        }))
        .unwrap();
    assert_eq!(seen.known, 7);
    // Not fields 2, 9 or 10: known, merely unselected.
    assert_eq!(seen.numbers, vec![99, 100]);
    // The raw value bytes after the tag: the varint itself, or a LEN field with its length prefix.
    assert_eq!(seen.varint_bytes, [0x2a]);
    assert_eq!(seen.len_bytes, b"\x02hi");

    // An unknown handler can end the walk early, like any handler.
    let mut first = Vec::new();
    Msg::new(&buf)
        .decode(msg_stream! {
            unknown => |u| { first.push(u.field_number()); Flow::Stop },
        })
        .unwrap();
    assert_eq!(first, vec![99]);
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
fn map_with_bytes_value_delivers_a_slice() {
    // `blobs` is field 22 (a two-byte tag), map<string, bytes>: one entry "k" -> [0xff, 0x00].
    const BLOBS: &[u8] = &[0xb2, 0x01, 0x07, 0x0a, 0x01, b'k', 0x12, 0x02, 0xff, 0x00];
    let mut blobs = Vec::new();
    Msg::new(BLOBS)
        .decode(msg_stream! {
            blobs => |(k, v)| blobs.push((k.to_str().unwrap().to_owned(), v.to_vec())),
        })
        .unwrap();
    assert_eq!(blobs, vec![("k".to_owned(), vec![0xff, 0x00])]);
}

#[test]
fn map_with_message_value_delivers_a_child_handle() {
    // `al.Layout.grid` is field 16, map<int32, Point> -- a message value delivered as the child's
    // reader handle, which the consumer recurses into with `point_stream!`. One entry: 7 -> {x=3, y=4}.
    use rustgen_golden::arena_layout::stream::al::*;
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
fn shared_state_form_with_no_fields_is_valid() {
    // A zero-field state selection decodes nothing (every field skipped) and must still compile
    // warning-clean -- the generated struct's `rp_state` is read by no handler, so the state struct
    // carries a `dead_code` allow for exactly this case.
    let mut touched = false;
    Msg::new(MSG).decode(msg_stream!(&mut touched; {})).unwrap();
    assert!(!touched);
}

#[test]
fn a_visitor_type_can_be_hand_implemented() {
    // The macro is sugar over the generated trait; a consumer may implement it directly.
    struct Count(u32);
    #[allow(non_upper_case_globals)] // the generated `WANT_<field>` consts
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

#[test]
fn package_less_schema_with_a_nested_message() {
    // A package-less schema's messages live directly in `stream`; `Root.Sub`'s macro sits in the
    // nested `root` module.
    use rustgen_golden::nopkg::stream::root::*;
    use rustgen_golden::nopkg::stream::*;
    // Root { sub: Sub { y: 5 }, self: Root { sub: Sub { y: 6 } } }
    const ROOT: &[u8] = &[0x0a, 0x02, 0x08, 0x05, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x06];
    let mut ys = Vec::new();
    Root::new(ROOT)
        .decode(root_stream!(&mut ys; {
            sub => |ys, s| s.decode(sub_stream!(&mut *ys; { y => |ys, v| ys.push(v) })),
            self_ => |ys, r| r.decode(root_stream!(&mut *ys; {
                sub => |ys, s| s.decode(sub_stream!(&mut *ys; { y => |ys, v| ys.push(v) })),
            })),
        }))
        .unwrap();
    assert_eq!(ys, vec![5, 6]);
}

#[test]
fn the_same_message_from_two_generated_files_keeps_both_macros() {
    // `dep.Dep` is generated by both the `dep` golden (its own file) and `main` (which imports it),
    // each exporting its macros under its own prefix: only those prefixes keep the two crate-root
    // `#[macro_export]` names apart.
    const DEP: &[u8] = &[0x08, 0x09]; // dep.Dep { v: 9 }
    let from_dep = {
        use rustgen_golden::dep::stream::dep::*;
        let mut v = 0;
        Dep::new(DEP)
            .decode(dep_stream! { v => |x| v = x })
            .unwrap();
        v
    };
    let from_main = {
        use rustgen_golden::main::stream::dep::*;
        let mut v = 0;
        Dep::new(DEP)
            .decode(dep_stream! { v => |x| v = x })
            .unwrap();
        v
    };
    assert_eq!((from_dep, from_main), (9, 9));
}

// An exported golden `include!`d into THIS crate: the expansion's recursion runs through the friendly
// module path inside the defining crate too, where a `$crate::`-absolute call to the hidden
// `#[macro_export]` name would be rejected (`macro_expanded_macro_exports_accessed_by_absolute_paths`,
// since the code is `include!`d). Its own module, since a second copy of package `p3` globbed beside
// the file-level one would make the two copies' names ambiguous.
mod included_here {
    #[allow(
        clippy::all,
        unused,
        non_camel_case_types,
        non_snake_case,
        non_upper_case_globals
    )]
    mod proto3 {
        include!("../goldens/proto3.rs");
    }
    use super::MSG;
    use proto3::stream::p3::*;

    #[test]
    fn exported_macros_also_work_in_the_crate_that_includes_them() {
        let (mut outer, mut inner, mut sum) = (0i32, 0i32, 0i32);
        Msg::new(MSG)
            .decode(msg_stream! {
                implicit_i => |v| outer = v,
                self_ => |sub| sub.decode(msg_stream! { implicit_i => |v| inner = v }),
            })
            .unwrap();
        Msg::new(MSG)
            .decode(msg_stream!(&mut sum; { nums => |s, v| *s += v }))
            .unwrap();
        assert_eq!((outer, inner, sum), (42, 7, 6));
    }
}

#[test]
fn globbing_two_packages_keeps_cross_package_types_unambiguous() {
    // `stream::main` and `stream::dep` each define an `rp_stream_ty_0` (a `Dep` handle in one, the
    // `DepEnum` newtype in the other). Globbed together, a bare alias would be ambiguous (E0659); the
    // expansion names it through its own message's scope.
    use rustgen_golden::main::stream::dep::*;
    use rustgen_golden::main::stream::main::*;
    // main.Main { d: dep.Dep { v: 9, de: DEP_ONE }, e: DEP_ONE }
    const MAIN: &[u8] = &[0x0a, 0x04, 0x08, 0x09, 0x10, 0x01, 0x20, 0x01];
    let (mut v, mut de, mut e) = (0i32, 0i32, 0i32);
    Main::new(MAIN)
        .decode(main_stream! {
            d => |dep| dep.decode(dep_stream! { v => |x| v = x, de => |k| de = k.0 }),
            e => |k| e = k.0,
        })
        .unwrap();
    assert_eq!((v, de, e), (9, 1, 1));
}

#[test]
fn globbing_two_modules_with_the_same_message_name_keeps_each_macro_usable() {
    // `anv.Outer` and its nested `anv.Outer.Outer` both generate an `Outer`, an `OuterVisitor` and an
    // `outer_stream!`. With both modules globbed, each macro is still callable through its path: the
    // expansion's own recursion and trait impl go through its message's scope, not the bare names.
    use parent::outer as child;
    use rustgen_golden::arena_naming_valid::stream::anv as parent;
    use rustgen_golden::arena_naming_valid::stream::anv::outer::*;
    use rustgen_golden::arena_naming_valid::stream::anv::*;
    // anv.Outer { inner: anv.Outer.Outer { v: 5 } }
    const OUTER: &[u8] = &[0x0a, 0x02, 0x08, 0x05];
    let mut v = 0i32;
    parent::Outer::new(OUTER)
        .decode(parent::outer_stream! {
            inner => |c| c.decode(child::outer_stream! { v => |x| v = x }),
        })
        .unwrap();
    assert_eq!(v, 5);
}

#[test]
fn globbing_one_package_from_two_generated_files_keeps_both_macros_usable() {
    // pkgorder_t1 and pkgorder_t2 are separate generated files that both declare package `po` (with
    // different messages), as two `compile()` calls sharing a package produce. Each expansion names
    // its own message's scope, so globbing both `po` modules leaves both macros usable.
    use rustgen_golden::pkgorder_t1::stream::po::*;
    use rustgen_golden::pkgorder_t2::stream::po::*;
    const V7: &[u8] = &[0x08, 0x07];
    let (mut a, mut b) = (0i32, 0i32);
    linux_::new(V7)
        .decode(linux__stream! { v => |x| a = x })
        .unwrap();
    linux::new(V7)
        .decode(linux_stream! { v => |x| b = x })
        .unwrap();
    assert_eq!((a, b), (7, 7));
}

#[test]
fn schema_names_shaped_like_the_stream_glue_do_not_collide_with_it() {
    // nsedge/gluenames.proto declares protoc-valid names the stream glue would otherwise clash with:
    // a message submodule and a sub-package named `rp_rt`, messages named exactly like `Holder`'s
    // scope and macro definition, a `Sized`, and a `Foo` whose macro shares `FooStream`'s submodule
    // name. Most of the check is the golden compiling; this drives `Holder`'s macros with all of those
    // names in scope, through the shared-state form (whose expansion bounds its state `?Sized`).
    use rustgen_golden::gluenames::stream::gluenames as top;
    use rustgen_golden::gluenames::stream::gluenames::rp__rt::*; // the `rp_rt` sub-package, reserved
    use rustgen_golden::gluenames::stream::gluenames::rp__rt_::*; // `RpRt`'s nested types
    use rustgen_golden::gluenames::stream::gluenames::*;
    use top::rp__rt as sub_pkg;
    // Holder { nested: RpRt { x: X { v: 1 } }, sub: Sub { inner: rp_rt { v: 2 } },
    //          sized: Sized { v: 4 }, foo: Foo { v: 3 } }
    const HOLDER: &[u8] = &[
        0x0a, 0x04, 0x0a, 0x02, 0x08, 0x01, // nested (field 1)
        0x12, 0x04, 0x0a, 0x02, 0x08, 0x02, // sub (field 2)
        0x1a, 0x02, 0x08, 0x04, // sized (field 3)
        0x22, 0x02, 0x08, 0x03, // foo (field 4)
    ];
    #[derive(Default)]
    struct Seen {
        x: i32,
        inner: i32,
        sized: i32,
        foo: i32,
    }
    let mut seen = Seen::default();
    Holder::new(HOLDER)
        .decode(holder_stream!(&mut seen; {
            // `RpRt` and the sub-package's `rp_rt` both reserve to a `rp__rt_stream!`, so name each
            // through its module.
            nested => |s, n| n.decode(top::rp__rt_stream!(&mut *s; {
                x => |s, x| x.decode(x_stream!(&mut *s; { v => |s, v| s.x = v })),
            })),
            sub => |s, sub| sub.decode(sub_stream!(&mut *s; {
                inner => |s, i| i.decode(sub_pkg::rp__rt_stream!(&mut *s; { v => |s, v| s.inner = v })),
            })),
            sized => |s, z| z.decode(sized_stream!(&mut *s; { v => |s, v| s.sized = v })),
            foo => |s, f| f.decode(foo_stream!(&mut *s; { v => |s, v| s.foo = v })),
        }))
        .unwrap();
    assert_eq!((seen.x, seen.inner, seen.sized, seen.foo), (1, 2, 4, 3));
}

// The collide.Holder input of the test below, built outside its scope: there the globbed modules
// take the names `u8`, `u32` and `bool`.
fn collide_holder() -> Vec<u8> {
    // Holder { Some: 1, rp_out: 5, decode_into: 7,
    //          prims: Prims { a: u8 { v: 13 }, c: true, f: I32_ONE, h: "xyz", j: [4, 5] },
    //          v: V { child: V { v: 12 }, v: 11 }, type: Type { x: X { v: 14 } }, under: type_ { v: 22 },
    //          node: Node { v: 15 }, opt: collide.Option.Opt { v: 16 }, sub: collide.NodeVisitor.X { v: 17 } }
    let field = |f: u32, v: u64| {
        let mut b = Vec::new();
        put_var(&mut b, f, v);
        b
    };
    let nested = |f: u32, payload: &[u8]| {
        let mut b = Vec::new();
        put_len(&mut b, f, payload);
        b
    };
    let mut buf = [field(1, 1), field(5, 5), field(7, 7)].concat();
    let prims = [
        nested(1, &field(1, 13)),
        field(3, 1),
        field(6, 1),
        nested(8, b"xyz"),
        nested(10, &[4, 5]), // packed
    ];
    put_len(&mut buf, 15, &prims.concat());
    put_len(
        &mut buf,
        16,
        &[nested(1, &field(2, 12)), field(2, 11)].concat(),
    );
    put_len(&mut buf, 17, &nested(1, &field(1, 14)));
    put_len(&mut buf, 18, &field(1, 22));
    put_len(&mut buf, 19, &field(1, 15));
    put_len(&mut buf, 21, &field(1, 16));
    put_len(&mut buf, 22, &field(1, 17));
    buf
}

#[test]
fn schema_names_shaped_like_generated_references_do_not_collide_with_them() {
    // nsedge/collide.proto names types after what generated code refers to or derives -- primitive
    // `u8`/`bool`, the decode driver's type parameter `V`, `Node` beside a `NodeVisitor` sub-package,
    // `Type` beside `type` -- and fields after decode-body names (`Some`, `rp_out`, `decode_into`).
    // Most of the check is the golden compiling; this drives the macros from a scope that globs the
    // package and its `collide.Option` sub-package. Both hold submodules named `u8`, `u32` and `bool`,
    // so those bare names are ambiguous here, and the rows delivering `&[u8]`, `u32` and `bool` values
    // (`prims.h`, `prims.j`, `prims.c`) must not name them so.
    use rustgen_golden::collide::stream::collide::r#type::*; // `Type`'s nested `X`
    use rustgen_golden::collide::stream::collide::NodeVisitor::*; // the sub-package
    use rustgen_golden::collide::stream::collide::Option::*; // the sub-package
    use rustgen_golden::collide::stream::collide::*;
    let buf = collide_holder();
    // No field typed `u8`/`u32`/`bool` (nor `Option`, the sub-package here): this scope sees those names
    // as the globbed modules.
    #[derive(Default)]
    struct Seen {
        some: i32,
        rp_out: i32,
        decode_into: i32,
        u8_v: i32,
        flag: i32,
        i32_value: i32,
        h_len: usize,
        j_sum: u64,
        v: (i32, i32),
        x: i32,
        under: i32,
        node: i32,
        opt: i32,
        sub: i32,
    }
    let mut seen = Seen::default();
    Holder::new(&buf)
        .decode(holder_stream!(&mut seen; {
            Some => |s, v| s.some = v,
            rp_out => |s, v| s.rp_out = v,
            decode_into => |s, v| s.decode_into = v,
            prims => |s, p| p.decode(prims_stream!(&mut *s; {
                a => |s, a| a.decode(u8__stream!(&mut *s; { v => |s, v| s.u8_v = v })),
                c => |s, c| s.flag = i32::from(c),
                f => |s, f| s.i32_value = f.0,
                h => |s, h| s.h_len = h.len(),
                j => |s, j| s.j_sum += u64::from(j),
            })),
            v => |s, v| v.decode(v_stream!(&mut *s; {
                v => |s, x| s.v.0 = x,
                child => |s, c| c.decode(v_stream!(&mut *s; { v => |s, x| s.v.1 = x })),
            })),
            type_ => |s, t| t.decode(type_stream!(&mut *s; {
                x => |s, x| x.decode(r#type::x_stream!(&mut *s; { v => |s, v| s.x = v })),
            })),
            under => |s, u| u.decode(type___stream!(&mut *s; { v => |s, v| s.under = v })),
            node => |s, n| n.decode(node_stream!(&mut *s; { v => |s, v| s.node = v })),
            opt => |s, o| o.decode(opt_stream!(&mut *s; { v => |s, v| s.opt = v })),
            sub => |s, x| x.decode(NodeVisitor::x_stream!(&mut *s; { v => |s, v| s.sub = v })),
        }))
        .unwrap();
    assert_eq!((seen.some, seen.rp_out, seen.decode_into), (1, 5, 7));
    assert_eq!(
        (seen.u8_v, seen.flag, seen.i32_value, seen.h_len, seen.j_sum),
        (13, 1, 1, 3, 9)
    );
    assert_eq!((seen.v, seen.x, seen.under), ((11, 12), 14, 22));
    assert_eq!((seen.node, seen.opt, seen.sub), (15, 16, 17));
}

#[test]
fn packed_fixed_width_repeated_fields_decode_per_element() {
    // Mirrors C++ "packed fixed-width repeated fields decode per element": the packed I64/I32 element
    // paths (double, sfixed32, float), which varint-only packed fields never reach.
    use rustgen_golden::packed::stream::pk::*;
    let mut buf = Vec::new();
    put_len(
        &mut buf,
        1,
        &[1.5f64.to_le_bytes(), (-2.25f64).to_le_bytes()].concat(),
    );
    put_len(
        &mut buf,
        2,
        &[7i32.to_le_bytes(), (-3i32).to_le_bytes()].concat(),
    );
    put_len(&mut buf, 3, &0.5f32.to_le_bytes());
    #[derive(Default)]
    struct Seen {
        pd: Vec<f64>,
        psf: Vec<i32>,
        pf: Vec<f32>,
    }
    let mut seen = Seen::default();
    Packed::new(&buf)
        .decode(packed_stream!(&mut seen; {
            pd => |s, v| s.pd.push(v),
            psf => |s, v| s.psf.push(v),
            pf => |s, v| s.pf.push(v),
        }))
        .unwrap();
    assert_eq!(seen.pd, vec![1.5, -2.25]);
    assert_eq!(seen.psf, vec![7, -3]);
    assert_eq!(seen.pf, vec![0.5]);
}

#[test]
fn an_editions_delimited_field_decodes_via_group_wire_format() {
    // Mirrors C++ "editions DELIMITED field decodes via group wire format". `delim` is
    // message_encoding = DELIMITED: SGROUP..EGROUP framing, not a length prefix. Decoded as
    // length-prefixed it would be skipped and its inner value stay 0.
    use rustgen_golden::editions2023::stream::ed23::*;
    let mut buf = Vec::new();
    put_len(&mut buf, 3, &[0x08, 0x2a]); // child: { implicit_scalar: 42 }
    put_tag(&mut buf, 6, 3); // delim: SGROUP
    buf.extend([0x08, 0x07]); //   { implicit_scalar: 7 }
    put_tag(&mut buf, 6, 4); // EGROUP
    let (mut child, mut delim) = (0i32, 0i32);
    M::new(&buf)
        .decode(m_stream! {
            child => |c| c.decode(m_stream! { implicit_scalar => |v| child = v }),
            delim => |d| d.decode(m_stream! { implicit_scalar => |v| delim = v }),
        })
        .unwrap();
    assert_eq!((child, delim), (42, 7));
}

#[test]
fn oneof_members_fire_per_occurrence_in_wire_order() {
    // Mirrors C++ "oneof members fire per occurrence in wire order". Last-wins is the consumer's
    // call: two members of one oneof both fire, in wire order.
    let mut buf = Vec::new();
    put_var(&mut buf, 10, 1); // pick.a = 1
    put_len(&mut buf, 11, b"hi"); // pick.b = "hi"
    put_var(&mut buf, 10, 2); // pick.a = 2
    let mut events: Vec<String> = Vec::new();
    Msg::new(&buf)
        .decode(msg_stream!(&mut events; {
            a => |e, v| e.push(format!("a={v}")),
            b => |e, v| e.push(format!("b={}", v.to_str().unwrap())),
        }))
        .unwrap();
    assert_eq!(events, ["a=1", "b=hi", "a=2"]);
}

#[test]
fn a_field_past_a_straddling_oneof_is_reached() {
    // Mirrors C++ "a probe past a straddling oneof reaches the member". `mid` (4) precedes the oneof's
    // int member `tail` (5), whose oneof also holds a group member: the field-order probes take `mid`
    // and `tail` and leave the group to the loop.
    use rustgen_golden::proto2::stream::p2::*;
    let mut buf = Vec::new();
    put_var(&mut buf, 4, 7);
    put_var(&mut buf, 5, 9);
    let (mut mid, mut tail) = (0i32, 0i32);
    OneofGroupStraddle::new(&buf)
        .decode(oneof_group_straddle_stream! { mid => |v| mid = v, tail => |v| tail = v })
        .unwrap();
    assert_eq!((mid, tail), (7, 9));
}

#[test]
fn a_repeated_message_field_fires_per_element() {
    // Mirrors C++ "a repeated message field fires per element".
    use rustgen_golden::proto2::stream::p2::container::*;
    use rustgen_golden::proto2::stream::p2::*;
    let mut buf = Vec::new();
    put_len(&mut buf, 5, &[0x08, 0x0b]); // items: Nested { x: 11 }
    put_len(&mut buf, 5, &[0x08, 0x16]); // items: Nested { x: 22 }
    let mut xs = Vec::new();
    Container::new(&buf)
        .decode(container_stream! {
            items => |n| n.decode(nested_stream! { x => |v| xs.push(v) }),
        })
        .unwrap();
    assert_eq!(xs, vec![11, 22]);
}

#[test]
fn a_duplicated_singular_field_fires_per_occurrence() {
    // Mirrors C++ "a singular sub-message occurring twice fires per occurrence".
    // Streaming stores nothing, so it applies no last-wins: every occurrence of a singular field --
    // sub-message, scalar or string -- reaches the handler (the arena model instead rejects a duplicated
    // singular sub-message; docs/semantics.md documents the asymmetry).
    let mut subs = Vec::new();
    put_len(&mut subs, 5, &[0x08, 0x01]); // self: { implicit_i: 1 }
    put_len(&mut subs, 5, &[0x08, 0x02]); // self again: { implicit_i: 2 }
    let mut seen = Vec::new();
    Msg::new(&subs)
        .decode(msg_stream! {
            self_ => |sub| sub.decode(msg_stream! { implicit_i => |v| seen.push(v) }),
        })
        .unwrap();
    assert_eq!(seen, vec![1, 2]);

    let mut flat = Vec::new();
    put_var(&mut flat, 1, 1);
    put_len(&mut flat, 3, b"AAA");
    put_var(&mut flat, 1, 2);
    put_len(&mut flat, 3, b"BBB");
    let (mut ints, mut names) = (Vec::new(), Vec::new());
    Msg::new(&flat)
        .decode(msg_stream! {
            implicit_i => |v| ints.push(v),
            name => |n| names.push(n.to_str().unwrap().to_owned()),
        })
        .unwrap();
    assert_eq!(ints, vec![1, 2]);
    assert_eq!(names, ["AAA", "BBB"]); // handed over separately, not joined

    // The one exception: a map ENTRY hands over one (key, value) pair, so a duplicate INSIDE an entry
    // never reaches the handler -- only the entry's last key and last value do.
    let mut entry = Vec::new();
    put_len(&mut entry, 1, b"a");
    put_len(&mut entry, 1, b"b");
    put_var(&mut entry, 2, 7);
    put_var(&mut entry, 2, 9);
    let mut map = Vec::new();
    put_len(&mut map, 9, &entry); // counts
    let mut pairs = Vec::new();
    Msg::new(&map)
        .decode(msg_stream! {
            counts => |(k, v)| pairs.push((k.to_str().unwrap().to_owned(), v)),
        })
        .unwrap();
    assert_eq!(pairs, [("b".to_owned(), 9)]);
}

#[test]
fn map_and_group_decoders_report_malformed_input() {
    // Mirrors C++ "map and group decoders report malformed input". Rust's `ErrorKind::Truncated`
    // covers both C++ wire errors (`TruncatedVarint`, `UnterminatedGroup`).
    use rustgen_golden::proto2::stream::p2::*;
    // by_id (field 2, map<int32, Color>): a valid two-byte entry envelope whose value varint is
    // truncated inside it -- the entry reader's error, not the outer field's.
    let truncated_entry = [0x12, 0x02, 0x10, 0x80];
    let err = Container::new(&truncated_entry)
        .decode(container_stream! { by_id => |_entry| () })
        .unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Truncated);

    // MyGroup (field 1, a group): an SGROUP with no matching EGROUP, selected or skipped.
    let unterminated = [0x0b];
    let err = WithGroup::new(&unterminated)
        .decode(with_group_stream! { mygroup => |_group| () })
        .unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Truncated);
    let err = WithGroup::new(&unterminated)
        .decode(with_group_stream! {})
        .unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Truncated);
}

#[test]
fn an_extension_field_surfaces_as_an_unknown_field() {
    // Mirrors C++ "an extension field surfaces as an unknown field". `extend WithGroup { optional
    // int32 ext_a = 100; }` adds no generated field: field 100 is unknown to the message, so it
    // reaches the unknown handler.
    use rustgen_golden::proto2::stream::p2::*;
    let mut buf = Vec::new();
    put_var(&mut buf, 100, 9);
    let mut unknown = Vec::new();
    WithGroup::new(&buf)
        .decode(with_group_stream! { unknown => |u| unknown.push(u.field_number()) })
        .unwrap();
    assert_eq!(unknown, vec![100]);
}

#[test]
fn well_known_type_and_cross_file_decoders_run() {
    // Mirrors C++ "well-known-type and cross-file decoders run".
    {
        use rustgen_golden::usewkt::stream::google::protobuf::*;
        let (mut seconds, mut nanos) = (0i64, 0i32);
        Timestamp::new(&[0x08, 0x05, 0x10, 0x07])
            .decode(timestamp_stream! { seconds => |v| seconds = v, nanos => |v| nanos = v })
            .unwrap();
        assert_eq!((seconds, nanos), (5, 7));
        let mut seconds = 0i64;
        Duration::new(&[0x08, 0x03])
            .decode(duration_stream! { seconds => |v| seconds = v })
            .unwrap();
        assert_eq!(seconds, 3);
    }
    {
        // main.Main -> dep.Dep / pub.Pub (re-exported through forward.proto) / fwd.Fwd.
        use rustgen_golden::main::stream::{dep::*, fwd::*, main::*, r#pub::*};
        let mut buf = Vec::new();
        put_len(&mut buf, 1, &[0x08, 0x09]); // d: { v: 9 }
        put_len(&mut buf, 2, &[0x08, 0x08]); // p: { w: 8 }
        put_len(&mut buf, 3, &[0x08, 0x07]); // f: { z: 7 }
        let (mut v, mut w, mut z) = (0i32, 0i32, 0i32);
        Main::new(&buf)
            .decode(main_stream! {
                d => |d| d.decode(dep_stream! { v => |x| v = x }),
                p => |p| p.decode(pub_stream! { w => |x| w = x }),
                f => |f| f.decode(fwd_stream! { z => |x| z = x }),
            })
            .unwrap();
        assert_eq!((v, w, z), (9, 8, 7));
    }
    {
        // A weak import: its types are still generated and usable.
        use rustgen_golden::weakmain::stream::{wd::*, wm::*};
        let mut v = 0i32;
        WMain::new(&[0x0a, 0x02, 0x08, 0x05])
            .decode(w_main_stream! { d => |d| d.decode(w_dep_stream! { v => |x| v = x }) })
            .unwrap();
        assert_eq!(v, 5);
    }
}

#[test]
fn a_failed_read_never_fires_and_a_handler_error_ends_the_decode() {
    // Mirrors C++ "a generated decoder reports malformed input and callback aborts". Field 1, then a
    // varint whose only byte has the continuation bit set: the value read fails, and the handler never
    // sees a value.
    let mut fired = false;
    let err = Msg::new(&[0x08, 0x80])
        .decode(msg_stream! { implicit_i => |_v| fired = true })
        .unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Truncated);
    assert!(!fired);

    // A handler that returns an error ends the decode with that error, unchanged. (`Flow::Stop` is the
    // way to end early WITHOUT an error.)
    let err = Msg::new(&[0x08, 0x0a])
        .decode(msg_stream! {
            implicit_i => |_v| Err::<(), _>(DecodeError::new(ErrorKind::InvalidTag, 1234)),
        })
        .unwrap_err();
    assert_eq!((err.kind(), err.offset()), (ErrorKind::InvalidTag, 1234));
}

#[test]
fn every_scalar_kind_from_the_protoc_fixture() {
    // Mirrors C++ "a generated decoder decodes a wire fixture". scalars.bin also carries an enum and
    // two repeated fields, which -- unselected -- are skipped.
    use rustgen_golden::proto2::stream::p2::*;
    #[derive(Default)]
    struct Seen {
        i32: i32,
        i64: i64,
        u32: u32,
        u64: u64,
        s32: i32,
        s64: i64,
        f32: u32,
        f64: u64,
        sf32: i32,
        sf64: i64,
        b: bool,
        s: String,
        by: Vec<u8>,
        fl: f32,
        db: f64,
    }
    let mut seen = Seen::default();
    Scalars::new(SCALARS_BIN)
        .decode(scalars_stream!(&mut seen; {
            i32 => |s, v| s.i32 = v,
            i64 => |s, v| s.i64 = v,
            u32 => |s, v| s.u32 = v,
            u64 => |s, v| s.u64 = v,
            s32 => |s, v| s.s32 = v,
            s64 => |s, v| s.s64 = v,
            f32 => |s, v| s.f32 = v,
            f64 => |s, v| s.f64 = v,
            sf32 => |s, v| s.sf32 = v,
            sf64 => |s, v| s.sf64 = v,
            b => |s, v| s.b = v,
            s => |s, v| s.s = v.to_str().unwrap().to_owned(),
            by => |s, v| s.by = v.to_vec(),
            fl => |s, v| s.fl = v,
            db => |s, v| s.db = v,
        }))
        .unwrap();
    assert_eq!(
        (seen.i32, seen.i64, seen.u32, seen.u64),
        (-7, 42, 300, 1_000_000)
    );
    assert_eq!((seen.s32, seen.s64), (-5, -2));
    assert_eq!((seen.f32, seen.f64), (0x0102_0304, 0x0102_0304_0506_0708));
    assert_eq!((seen.sf32, seen.sf64), (-2, -3));
    assert!(seen.b);
    assert_eq!(seen.s, "hi");
    assert_eq!(seen.by, [0x00, 0x01, 0xff]);
    assert_eq!((seen.fl, seen.db), (1.5, -2.25));
}

#[test]
fn the_one_byte_tag_fast_path_matches_the_general_path() {
    // Mirrors C++ "raw-byte fast path matches the general path across the 15/16 boundary".
    // Scalars spans the 15/16 boundary: `i32` (field 1) has a one-byte tag, `color` (field 16) a
    // two-byte one.
    use rustgen_golden::proto2::stream::p2::*;
    #[derive(Default, Debug, PartialEq)]
    struct Seen {
        i32: Option<i32>,
        color: Option<i32>,
        unknown: Vec<u32>,
    }
    let decode = |buf: &[u8]| -> Result<Seen, DecodeError> {
        let mut seen = Seen::default();
        Scalars::new(buf).decode(scalars_stream!(&mut seen; {
            i32 => |s, v| s.i32 = Some(v),
            color => |s, c| s.color = Some(c.0),
            unknown => |s, u| s.unknown.push(u.field_number()),
        }))?;
        Ok(seen)
    };

    // Both sides of the boundary decode in one message: in field order through the field-order
    // probes, and after a repeat of field 1 through the loop's one- and two-byte tag reads.
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 123);
    put_var(&mut buf, 16, 2);
    let seen = decode(&buf).unwrap();
    assert_eq!((seen.i32, seen.color), (Some(123), Some(2)));
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 5);
    put_var(&mut buf, 1, 123);
    put_var(&mut buf, 16, 2);
    let seen = decode(&buf).unwrap();
    assert_eq!((seen.i32, seen.color), (Some(123), Some(2)));

    // A KNOWN field with the wrong wire type is skipped on both sides -- and is never reported to the
    // unknown handler, which is for field numbers the schema does not declare.
    let mut buf = Vec::new();
    put_len(&mut buf, 1, b"x");
    put_len(&mut buf, 16, b"y");
    assert_eq!(decode(&buf).unwrap(), Seen::default());

    // A reserved wire type (6) is rejected whether its tag takes one byte or two.
    let mut one = Vec::new();
    put_tag(&mut one, 1, 6);
    let mut two = Vec::new();
    put_tag(&mut two, 16, 6);
    assert_eq!(decode(&one).unwrap_err().kind(), ErrorKind::InvalidTag);
    assert_eq!(decode(&two).unwrap_err().kind(), ErrorKind::InvalidTag);

    // A non-minimal (two-byte) tag for field 1 misses the one-byte path but decodes the same.
    let mut buf = Vec::new();
    put_overlong_tag(&mut buf, 1, 0);
    put_varint(&mut buf, 55);
    let seen = decode(&buf).unwrap();
    assert_eq!((seen.i32, seen.unknown.len()), (Some(55), 0));
}

#[test]
fn the_field_order_probes_and_the_loop_deliver_the_same_events() {
    // In field order, fields go through the field-order probes ahead of the decode loop; reversed (an
    // undeclared field first), every probe misses and the loop's arms take them. Both deliver the same
    // events in wire order, with every field selected and with a subset, whose other fields a probe's
    // constant-key skip or the loop's catch-all skips.
    let mut nested = Vec::new();
    put_var(&mut nested, 1, 9);
    let mut packed = Vec::new();
    put_varint(&mut packed, 1);
    put_varint(&mut packed, 300);
    let record = |put: &dyn Fn(&mut Vec<u8>)| {
        let mut b = Vec::new();
        put(&mut b);
        b
    };
    // Each field occurrence with the events it yields, in field order.
    let records = [
        (record(&|b| put_var(b, 1, 7)), "i=7"),
        (record(&|b| put_var(b, 2, 300)), "x=300"),
        (record(&|b| put_len(b, 3, b"n")), "name=n"),
        (record(&|b| put_var(b, 4, 1)), "state=1"),
        (record(&|b| put_len(b, 5, &nested)), "self.i=9"),
        (record(&|b| put_var(b, 6, 5)), "nums=5"),
        (record(&|b| put_len(b, 6, &packed)), "nums=1 nums=300"),
        (
            record(&|b| put_len(b, 9, &[0x0a, 0x01, b'k', 0x10, 0x02])),
            "counts=k:2",
        ),
        (record(&|b| put_var(b, 10, 42)), "a=42"),
        (
            record(&|b| {
                put_tag(b, 20, 1);
                b.extend(2.5f64.to_le_bytes());
            }),
            "ratio=2.5",
        ),
        (
            record(&|b| {
                put_tag(b, 21, 5);
                b.extend(1.5f32.to_le_bytes());
            }),
            "scale=1.5",
        ),
        (record(&|b| put_var(b, 99, 1)), "unknown=99"),
    ];
    let all = |buf: &[u8]| {
        let mut events: Vec<String> = Vec::new();
        Msg::new(buf)
            .decode(msg_stream!(&mut events; {
                implicit_i => |e, v| e.push(format!("i={v}")),
                explicit_i => |e, v| e.push(format!("x={v}")),
                name => |e, v| e.push(format!("name={}", v.to_str().unwrap())),
                state => |e, v| e.push(format!("state={}", v.0)),
                self_ => |e, sub| {
                    let mut i = 0;
                    sub.decode(msg_stream! { implicit_i => |v| i = v })?;
                    e.push(format!("self.i={i}"));
                    Ok::<(), DecodeError>(())
                },
                nums => |e, v| e.push(format!("nums={v}")),
                counts => |e, (k, v)| e.push(format!("counts={}:{v}", k.to_str().unwrap())),
                a => |e, v| e.push(format!("a={v}")),
                ratio => |e, v| e.push(format!("ratio={v}")),
                scale => |e, v| e.push(format!("scale={v}")),
                unknown => |e, u| e.push(format!("unknown={}", u.field_number())),
            }))
            .unwrap();
        events
    };
    let subset = |buf: &[u8]| {
        let mut events: Vec<String> = Vec::new();
        Msg::new(buf)
            .decode(msg_stream!(&mut events; {
                explicit_i => |e, v| e.push(format!("x={v}")),
                counts => |e, (k, v)| e.push(format!("counts={}:{v}", k.to_str().unwrap())),
                scale => |e, v| e.push(format!("scale={v}")),
                unknown => |e, u| e.push(format!("unknown={}", u.field_number())),
            }))
            .unwrap();
        events
    };
    let in_subset = |e: &&str| {
        ["x=", "counts=", "scale=", "unknown="]
            .iter()
            .any(|p| e.starts_with(p))
    };
    let forward: Vec<_> = records.iter().collect();
    let reversed: Vec<_> = records.iter().rev().collect();
    for order in [&forward, &reversed] {
        let buf: Vec<u8> = order.iter().flat_map(|(b, _)| b.iter().copied()).collect();
        let expected: Vec<&str> = order.iter().flat_map(|(_, e)| e.split(' ')).collect();
        assert_eq!(all(&buf), expected);
        let expected: Vec<&str> = expected.into_iter().filter(in_subset).collect();
        assert_eq!(subset(&buf), expected);
    }

    // A handler stopping inside a self-looping probe ends the walk there.
    let buf: Vec<u8> = forward
        .iter()
        .flat_map(|(b, _)| b.iter().copied())
        .collect();
    let mut nums = Vec::new();
    Msg::new(&buf)
        .decode(msg_stream! { nums => |v| { nums.push(v); Flow::Stop } })
        .unwrap();
    assert_eq!(nums, [5]);

    // A malformed field nobody selected errors at its value either way: through its probe's
    // constant-key skip, and through the loop's catch-all once a oneof member came first.
    let err = Msg::new(&[0x08, 0x80]).decode(msg_stream! {}).unwrap_err();
    assert_eq!((err.kind(), err.offset()), (ErrorKind::Truncated, 1));
    let err = Msg::new(&[0x50, 0x01, 0x08, 0x80])
        .decode(msg_stream! {})
        .unwrap_err();
    assert_eq!((err.kind(), err.offset()), (ErrorKind::Truncated, 3));
}

#[test]
fn non_minimal_tags_decode_like_canonical_ones_across_field_kinds() {
    // Mirrors C++ "non-minimal tags decode identically across fast field kinds".
    use rustgen_golden::proto2::stream::p2::container::*;
    use rustgen_golden::proto2::stream::p2::*;
    // A string (Scalars.s, field 12).
    let mut buf = Vec::new();
    put_overlong_tag(&mut buf, 12, 2);
    put_varint(&mut buf, 5);
    buf.extend_from_slice(b"hello");
    let (mut s, mut unknown) = (String::new(), 0);
    Scalars::new(&buf)
        .decode(scalars_stream! {
            s => |v| s = v.to_str().unwrap().to_owned(),
            unknown => |_u| unknown += 1,
        })
        .unwrap();
    assert_eq!((s.as_str(), unknown), ("hello", 0));

    // A oneof sub-message (Container.cn, field 4): oneof dispatch plus recursion.
    let mut buf = Vec::new();
    put_overlong_tag(&mut buf, 4, 2);
    buf.extend([0x02, 0x08, 0x2a]); // { x: 42 }
    let mut x = 0i32;
    Container::new(&buf)
        .decode(container_stream! { cn => |n| n.decode(nested_stream! { x => |v| x = v }) })
        .unwrap();
    assert_eq!(x, 42);

    // A repeated sub-message (Container.items, field 5): an over-long element tag, then a canonical
    // one, both fire.
    let mut buf = Vec::new();
    put_overlong_tag(&mut buf, 5, 2);
    buf.extend([0x02, 0x08, 0x07]); // { x: 7 }
    put_len(&mut buf, 5, &[0x08, 0x08]); // { x: 8 }
    let mut xs = Vec::new();
    Container::new(&buf)
        .decode(container_stream! { items => |n| n.decode(nested_stream! { x => |v| xs.push(v) }) })
        .unwrap();
    assert_eq!(xs, vec![7, 8]);
}

#[test]
fn the_protoc_msg_fixture_decodes_enums_sub_messages_oneofs_and_maps() {
    // Mirrors C++ "a generated decoder decodes enum, sub-message, and oneof fields".
    use rustgen_golden::proto3::common::p3::State;
    #[derive(Default)]
    struct Seen {
        implicit_i: i32,
        state: Option<State>,
        a: i32,
        self_implicit_i: i32,
        nums: Vec<i32>,
        unpacked: Vec<i32>,
        states: Vec<State>,
        counts: Vec<(String, i32)>,
    }
    let mut seen = Seen::default();
    Msg::new(MSG_BIN)
        .decode(msg_stream!(&mut seen; {
            implicit_i => |s, v| s.implicit_i = v,
            state => |s, v| s.state = Some(v),
            self_ => |s, sub| sub.decode(msg_stream!(&mut *s; {
                implicit_i => |s, v| s.self_implicit_i = v,
            })),
            nums => |s, v| s.nums.push(v), // packed on the wire
            unpacked => |s, v| s.unpacked.push(v), // expanded on the wire
            states => |s, v| s.states.push(v), // packed enums
            counts => |s, (k, v)| s.counts.push((k.to_str().unwrap().to_owned(), v)),
            a => |s, v| s.a = v, // the oneof member that is set
        }))
        .unwrap();
    assert_eq!(seen.implicit_i, 10);
    assert_eq!(seen.state, Some(State::ON));
    assert_eq!(seen.a, 7);
    assert_eq!(seen.self_implicit_i, 99);
    assert_eq!(seen.nums, vec![1, 2, 3]);
    assert_eq!(seen.unpacked, vec![4, 5]);
    assert_eq!(seen.states, vec![State::ON, State::UNKNOWN]);
    assert_eq!(seen.counts, [("x".to_owned(), 1), ("y".to_owned(), 2)]);
}

#[test]
fn the_protoc_container_fixture_decodes_message_and_enum_valued_maps() {
    // Mirrors C++ "a generated decoder decodes message-value and enum-value maps".
    use rustgen_golden::proto2::common::p2::Color;
    use rustgen_golden::proto2::stream::p2::container::*;
    use rustgen_golden::proto2::stream::p2::*;
    #[derive(Default)]
    struct Seen {
        by_name: Vec<(String, i32)>,
        by_id: Vec<(i32, Color)>,
    }
    let mut seen = Seen::default();
    Container::new(CONTAINER_BIN)
        .decode(container_stream!(&mut seen; {
            // A message value arrives as the child's reader handle.
            by_name => |s, (k, n)| {
                let mut x = 0;
                n.decode(nested_stream! { x => |v| x = v })?;
                s.by_name.push((k.to_str().unwrap().to_owned(), x));
                Ok::<_, DecodeError>(())
            },
            // COLOR_NEG is negative: a ten-byte varint on the wire.
            by_id => |s, (k, v)| s.by_id.push((k, v)),
        }))
        .unwrap();
    assert_eq!(
        seen.by_name,
        [("alpha".to_owned(), 11), ("beta".to_owned(), 22)]
    );
    assert_eq!(seen.by_id, [(1, Color::RED), (2, Color::NEG)]);
}

#[test]
fn the_protoc_all_wire_fixture_decodes_every_wire_type() {
    // Mirrors C++ "a generated decoder decodes packed-repeated and group fields". wire.AllWire covers
    // each wire type, a nested message, packed varints, a group and a oneof.
    use rustgen_golden::wire_all::stream::wire::all_wire::*;
    use rustgen_golden::wire_all::stream::wire::*;
    #[derive(Default)]
    struct Seen {
        zz: i64,
        db: f64,
        fx: u32,
        s: String,
        by: Vec<u8>,
        nested_zz: i64,
        packed: Vec<i32>,
        group_a: i32,
        oi: i32,
    }
    let mut seen = Seen::default();
    AllWire::new(ALL_WIRE_BIN)
        .decode(all_wire_stream!(&mut seen; {
            zz => |s, v| s.zz = v, // sint64, zigzag
            db => |s, v| s.db = v,
            fx => |s, v| s.fx = v,
            s => |s, v| s.s = v.to_str().unwrap().to_owned(),
            by => |s, v| s.by = v.to_vec(),
            nested => |s, n| n.decode(all_wire_stream!(&mut *s; { zz => |s, v| s.nested_zz = v })),
            packed => |s, v| s.packed.push(v),
            g => |s, g| g.decode(g_stream!(&mut *s; { a => |s, v| s.group_a = v })),
            oi => |s, v| s.oi = v,
        }))
        .unwrap();
    assert_eq!(seen.zz, -1_234_567_890_123);
    #[allow(clippy::approx_constant)] // all_wire.txtpb's literal, not an approximation of π
    let db = 3.14159;
    assert_eq!(seen.db, db);
    assert_eq!(seen.fx, 305_419_896);
    assert_eq!(seen.s, "wire");
    assert_eq!(seen.by, [0xde, 0xad, 0xbe, 0xef]);
    assert_eq!(seen.nested_zz, 7);
    assert_eq!(seen.packed, vec![10, 20, 30]);
    assert_eq!(seen.group_a, 99);
    assert_eq!(seen.oi, 5);
}

#[test]
fn bytes_is_the_exact_undecoded_span() {
    // Mirrors C++ "rp_bytes() exposes the exact undecoded span". The stream -> arena hybrid hands a child's `bytes()` to the arena model's `decode`, which reads a
    // plain field sequence: so a LEN child's span excludes its length prefix and a group/DELIMITED
    // child's span excludes its SGROUP/EGROUP framing -- and either aliases the input (zero-copy).
    use rustgen_golden::editions2023::stream::ed23::*;
    let body = [0x08, 0x2a]; // { implicit_scalar: 42 }
    let mut len_buf = Vec::new();
    put_len(&mut len_buf, 3, &body); // child: LEN framing
    let mut group_buf = Vec::new();
    put_tag(&mut group_buf, 6, 3); // delim: SGROUP
    group_buf.extend_from_slice(&body);
    put_tag(&mut group_buf, 6, 4); // EGROUP

    let mut len_span: &[u8] = &[];
    M::new(&len_buf)
        .decode(m_stream! { child => |c| len_span = c.bytes() })
        .unwrap();
    let mut group_span: &[u8] = &[];
    M::new(&group_buf)
        .decode(m_stream! { delim => |d| group_span = d.bytes() })
        .unwrap();
    assert_eq!(len_span, body);
    assert_eq!(group_span, body);
    // Zero-copy: each span starts right after its framing (tag + length byte; SGROUP tag).
    assert_eq!(len_span.as_ptr(), len_buf[2..].as_ptr());
    assert_eq!(group_span.as_ptr(), group_buf[1..].as_ptr());

    // The root handle's span is the whole input.
    assert_eq!(M::new(&len_buf).bytes().as_ptr(), len_buf.as_ptr());
    assert_eq!(M::new(&len_buf).bytes(), &len_buf[..]);
}
