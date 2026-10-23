//! Drives `export_stream_macros` output from OUTSIDE the crate that `include!`s it -- this `tests/`
//! crate, the case the option exists for (as is a crate depending on a shared proto crate). The
//! `streamexport_*` goldens are generated stream-only with exported macros, each under its own prefix,
//! so the golden crate holds several exported files at once. The default output's macros are
//! `pub(crate)`, so none of these calls would resolve from here without the option.

use rapidproto::stream::Flow;

mod p3 {
    use super::Flow;
    use rustgen_golden::streamexport_p3::stream::p3::*;

    // Msg { implicit_i = 42, name = "hello", state = 1, nums = [1, 2, 3] (packed),
    //       self = Msg { implicit_i = 7 }, a = 99 (oneof pick), counts = {"a": 1},
    //       blobs = {"k": [0xff, 0x00]}, field 99 = varint 5 (undeclared) }
    const MSG: &[u8] = &[
        0x08, 0x2a, // implicit_i = 42 (field 1)
        0x1a, 0x05, b'h', b'e', b'l', b'l', b'o', // name = "hello" (field 3)
        0x20, 0x01, // state = 1 (field 4)
        0x32, 0x03, 0x01, 0x02, 0x03, // nums = [1, 2, 3] packed (field 6)
        0x2a, 0x02, 0x08, 0x07, // self = Msg { implicit_i = 7 } (field 5)
        0x50, 0x63, // pick.a = 99 (field 10)
        0x4a, 0x05, 0x0a, 0x01, b'a', 0x10, 0x01, // counts["a"] = 1 (field 9)
        0xb2, 0x01, 0x07, 0x0a, 0x01, b'k', 0x12, 0x02, 0xff, 0x00, // blobs["k"] (field 22)
        0x98, 0x06, 0x05, // field 99 = 5 (undeclared)
    ];

    #[test]
    fn plain_form_reaches_each_value_type() {
        // A string (`ProtoStr`), an enum newtype (a foreign-type alias), packed elements, a recursive
        // child handle (the nested call is a second cross-crate expansion), a map entry with a string
        // key, one with a bytes value, and the unknown handler.
        let (mut name, mut state, mut nums, mut inner, mut unknown) =
            (String::new(), -1i32, Vec::new(), 0i32, 0u32);
        let (mut counts, mut blobs) = (Vec::new(), Vec::new());
        Msg::new(MSG)
            .decode(msg_stream! {
                name => |n| name = n.to_str().unwrap().to_owned(),
                state => |s| state = s.0,
                nums => |v| nums.push(v),
                self_ => |sub| sub.decode(msg_stream! { implicit_i => |v| inner = v }),
                counts => |(k, v)| counts.push((k.to_str().unwrap().to_owned(), v)),
                blobs => |(k, v)| blobs.push((k.to_str().unwrap().to_owned(), v.to_vec())),
                unknown => |u| unknown = u.field_number(),
            })
            .unwrap();
        assert_eq!(name, "hello");
        assert_eq!(state, 1);
        assert_eq!(nums, vec![1, 2, 3]);
        assert_eq!(inner, 7);
        assert_eq!(counts, vec![("a".to_owned(), 1)]);
        assert_eq!(blobs, vec![("k".to_owned(), vec![0xff, 0x00])]);
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
}

#[test]
fn two_exports_of_one_schema_coexist_under_their_prefixes() {
    // `streamexport_p3` and `streamexport_p3_twin` export the same schema, so they define the same
    // messages; only their prefixes keep the crate-root `#[macro_export]` names apart.
    const MSG: &[u8] = &[0x08, 0x2a]; // implicit_i = 42
    let first = {
        use rustgen_golden::streamexport_p3::stream::p3::*;
        let mut v = 0;
        Msg::new(MSG)
            .decode(msg_stream! { implicit_i => |x| v = x })
            .unwrap();
        v
    };
    let second = {
        use rustgen_golden::streamexport_p3_twin::stream::p3::*;
        let mut v = 0;
        Msg::new(MSG)
            .decode(msg_stream! { implicit_i => |x| v = x })
            .unwrap();
        v
    };
    assert_eq!((first, second), (42, 42));
}

#[test]
fn cross_package_closure_with_both_packages_globbed() {
    // `main.Main` holds a `dep.Dep` child and a `dep.DepEnum` -- both foreign types, named through
    // re-export aliases -- and the two packages' modules are globbed into one scope.
    use rustgen_golden::streamexport_main::stream::dep::*;
    use rustgen_golden::streamexport_main::stream::main::*;
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
fn package_less_schema_with_a_nested_message() {
    // A package-less schema's messages live directly in `stream`; `Root.Sub`'s macro sits in the
    // nested `root` module.
    use rustgen_golden::streamexport_nopkg::stream::root::*;
    use rustgen_golden::streamexport_nopkg::stream::*;
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
