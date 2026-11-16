//! The generated `Debug` impls: which fields print, under which names, and how values look.

use rapidproto::Arena;

use rustgen_golden::arena_modes_lean::arena::fm::Holder;
use rustgen_golden::proto2::arena::p2::WithGroup;
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
fn packed(values: &[u64]) -> Vec<u8> {
    let mut b = Vec::new();
    for &v in values {
        put_varint(&mut b, v);
    }
    b
}

#[test]
fn prints_the_set_fields_under_their_proto_names() {
    let mut inner = Vec::new();
    put_len(&mut inner, 3, b"in");
    let mut entry = Vec::new();
    put_len(&mut entry, 1, b"a");
    put_var(&mut entry, 2, 1);

    let mut buf = Vec::new();
    put_var(&mut buf, 1, 300); // implicit_i
    put_var(&mut buf, 2, 0); // explicit_i: set, though zero
    put_len(&mut buf, 3, b"hi"); // name
    put_var(&mut buf, 4, 1); // state: ON
    put_len(&mut buf, 5, &inner); // self
    put_len(&mut buf, 6, &packed(&[1, 2])); // nums
    put_len(&mut buf, 8, &packed(&[1, 7])); // states: ON, and a number the enum does not name
    put_tag(&mut buf, 20, 1); // ratio: -0.0 is not the default
    buf.extend_from_slice(&(-0.0f64).to_le_bytes());
    put_len(&mut buf, 24, &[0xff]); // chunks
    put_len(&mut buf, 24, &[]);
    put_len(&mut buf, 9, &entry); // counts
    put_len(&mut buf, 11, b"txt"); // pick: b

    let arena = Arena::new();
    let msg = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(
        format!("{msg:?}"),
        r#"Msg { implicit_i: 300, explicit_i: 0, name: "hi", state: On, self: Msg { name: "in" }, nums: [1, 2], states: [On, Unknown(7)], ratio: -0.0, chunks: [b"\xff", b""], counts: {"a": 1}, b: "txt" }"#
    );
}

#[test]
fn an_empty_message_prints_its_name() {
    let arena = Arena::new();
    let msg = Msg::decode(&[], &arena).unwrap();
    assert_eq!(format!("{msg:?}"), "Msg");
}

#[test]
fn pretty_printing_nests() {
    let mut inner = Vec::new();
    put_var(&mut inner, 1, 1);
    let mut buf = Vec::new();
    put_len(&mut buf, 3, b"x");
    put_len(&mut buf, 5, &inner);

    let arena = Arena::new();
    let msg = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(
        format!("{msg:#?}"),
        "Msg {\n    name: \"x\",\n    self: Msg {\n        implicit_i: 1,\n    },\n}"
    );
}

#[test]
fn invalid_utf8_and_oneof_values() {
    let mut buf = Vec::new();
    put_len(&mut buf, 3, &[0xff]); // name
    put_len(&mut buf, 23, &[0xab]); // pick: e (bytes)

    let arena = Arena::new();
    let msg = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(
        format!("{msg:?}"),
        r#"Msg { name: ProtoStr(b"\xff"), e: b"\xab" }"#
    );
    assert_eq!(format!("{:?}", msg.pick()), r#"Some(E(b"\xab"))"#);
}

#[test]
fn groups_print_as_messages() {
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 3); // MyGroup
    put_var(&mut buf, 2, 1);
    put_tag(&mut buf, 3, 3); // Inner
    put_var(&mut buf, 4, 1);
    put_tag(&mut buf, 3, 4);
    put_tag(&mut buf, 1, 4);

    let arena = Arena::new();
    let msg = WithGroup::decode(&buf, &arena).unwrap();
    assert_eq!(
        format!("{msg:?}"),
        "WithGroup { mygroup: MyGroup { a: 1, inner: Inner { flag: true } } }"
    );
}

#[test]
fn a_dropped_field_is_left_out_and_a_raw_one_prints_its_payload() {
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 2); // keep
    put_var(&mut buf, 2, 3); // debug: dropped by the profile
    put_var(&mut buf, 5, 1); // must
    put_len(&mut buf, 13, &[0x08, 0x01]); // req_blob: raw

    let arena = Arena::new();
    let msg = Holder::decode(&buf, &arena).unwrap();
    assert_eq!(
        format!("{msg:?}"),
        r#"Holder { keep: 2, must: 1, req_blob: b"\x08\x01" }"#
    );
}

#[test]
fn a_stream_handle_prints_its_length() {
    let handle = rustgen_golden::proto3::stream::p3::Msg::new(&[0x08, 0x01]).unwrap();
    assert_eq!(format!("{handle:?}"), "Msg { len: 2 }");
}

#[test]
fn an_owned_message_prints_the_message() {
    let mut buf = Vec::new();
    put_len(&mut buf, 3, b"hi");
    let owned = Msg::decode_owned(buf).unwrap();
    assert_eq!(format!("{owned:?}"), r#"Msg { name: "hi" }"#);
}

#[test]
fn a_message_at_the_depth_limit_prints_on_a_default_thread() {
    // Wrap 100 `self` layers, as deep as `decode` allows.
    let mut buf = Vec::new();
    for _ in 0..rapidproto::MAX_DECODE_DEPTH {
        let inner = core::mem::take(&mut buf);
        put_len(&mut buf, 5, &inner);
    }
    // A debug build needs a large stack to decode this deep (see decode.rs), but not to print it.
    std::thread::Builder::new()
        .stack_size(32 * 1024 * 1024)
        .spawn(move || {
            let arena = Arena::new();
            let msg = Msg::decode(&buf, &arena).unwrap();
            let (compact, pretty) = std::thread::scope(|scope| {
                std::thread::Builder::new()
                    .spawn_scoped(scope, || (format!("{msg:?}"), format!("{msg:#?}")))
                    .expect("spawn print thread")
                    .join()
                    .expect("print thread panicked")
            });
            assert_eq!(compact.matches("self: Msg").count(), 100);
            assert_eq!(pretty.matches("self: Msg").count(), 100);
        })
        .expect("spawn deep-decode thread")
        .join()
        .expect("deep-decode thread panicked");
}
