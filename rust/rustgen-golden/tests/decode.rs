//! Decode round-trip tests for the Rust generator's emitted `decode` bodies.
//!
//! These decode the SAME global corpus schemas the C++ arena decode test (`tests/test_arena_decode.cpp`)
//! uses -- there are no Rust-local fixture protos -- and assert the same behaviors, so the two
//! languages stay in lockstep. The generated code under test is the committed goldens (included by the
//! crate's `lib.rs`); the tiny wire encoders below are test scaffolding, not the system under test.
//!
//! Coverage by message:
//!   p3::Msg          -- presence (implicit/explicit), singular message + recursion cap, repeated
//!                       scalar/enum (packed + unpacked), maps (scalar keys/values, bool key), oneof
//!                       (scalar/string/bool/double), skip + error edges.
//!   xp::Expanded     -- sint64 (zigzag), fixed64, sfixed32, bool, enum element conversions.
//!   p2::Container    -- message-valued map, enum-valued map, oneof with a message variant, repeated
//!                       message, the in-entry repeated-singular-message rejection.
//!   mr::ManyRequired -- proto2 `required`: all-present ok, a missing field -> MissingRequired.
//!   al::Layout       -- inlined small fixed-size sub-messages (bare `T` + presence bit, decoded in
//!                       place) vs pointer sub-messages (over the cutoff / not fixed-size), plus a
//!                       repeated inlined-message array.
//!   mr::ReqInline    -- a `required` small fixed-size message: inlined by value with NO resting
//!                       presence bit (always-present), present-ok + missing -> MissingRequired.

use rapidproto::{Arena, ErrorKind};

use rustgen_golden::arena_layout::arena::al::Layout;
use rustgen_golden::arena_manyreq::arena::mr::ManyRequired;
use rustgen_golden::arena_manyreq::arena::mr::ReqInline;
use rustgen_golden::expanded::arena::xp::Expanded;
use rustgen_golden::expanded::common::xp::expanded::Kind;
use rustgen_golden::proto2::arena::p2::container::{Choice, Nested};
use rustgen_golden::proto2::arena::p2::{Container, Scalars};
use rustgen_golden::proto2::common::p2::Color;
use rustgen_golden::proto3::arena::p3::msg::Pick;
use rustgen_golden::proto3::arena::p3::Msg;
use rustgen_golden::proto3::common::p3::State;
// The field-modes golden: proto3's Msg generated under `drop name`, `raw self`, `unknown p3.Msg`.
use rustgen_golden::proto3_modes::arena::p3::Msg as ModedMsg;

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
fn put_var(b: &mut Vec<u8>, field: u32, v: u64) {
    put_tag(b, field, 0);
    put_varint(b, v);
}
fn put_len(b: &mut Vec<u8>, field: u32, payload: &[u8]) {
    put_tag(b, field, 2);
    put_varint(b, payload.len() as u64);
    b.extend_from_slice(payload);
}
// A single-field payload (a map entry's key or value), as its own byte buffer.
fn varint_field(field: u32, v: u64) -> Vec<u8> {
    let mut b = Vec::new();
    put_var(&mut b, field, v);
    b
}
fn len_field(field: u32, payload: &[u8]) -> Vec<u8> {
    let mut b = Vec::new();
    put_len(&mut b, field, payload);
    b
}
// Append a map entry (field 1 = key, field 2 = value) as a length-delimited sub-message of `field`.
fn map_entry(buf: &mut Vec<u8>, field: u32, key: &[u8], value: &[u8]) {
    let mut e = Vec::new();
    e.extend_from_slice(key);
    e.extend_from_slice(value);
    put_len(buf, field, &e);
}

#[test]
fn scalars_presence_and_defaults() {
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 300); // implicit_i (i32)
    put_var(&mut buf, 2, 42); // explicit_i (optional i32)
    put_len(&mut buf, 3, b"hi"); // name (string)
    put_var(&mut buf, 4, 1); // state = ON (enum)
    put_tag(&mut buf, 20, 1);
    buf.extend_from_slice(&(-0.0f64).to_bits().to_le_bytes()); // ratio: -0.0 bit pattern preserved
    put_tag(&mut buf, 21, 5);
    buf.extend_from_slice(&1.5f32.to_bits().to_le_bytes()); // scale

    let arena = Arena::new();
    let m = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(m.implicit_i(), 300);
    assert_eq!(m.explicit_i(), Some(42));
    assert_eq!(m.name().as_bytes(), b"hi");
    assert_eq!(m.state(), State::ON);
    assert_eq!(m.ratio().to_bits(), (-0.0f64).to_bits());
    assert_eq!(m.scale(), 1.5);

    // Absent: implicit zero defaults, explicit None, empty string, enum zero.
    let d = Msg::decode(&[], &arena).unwrap();
    assert_eq!(d.implicit_i(), 0);
    assert_eq!(d.explicit_i(), None);
    assert_eq!(d.name().as_bytes(), b"");
    assert_eq!(d.state(), State::UNKNOWN);
    assert_eq!(d.ratio(), 0.0);
    assert_eq!(d.scale(), 0.0);
    assert!(m.pick().is_none());
}

#[test]
fn singular_scalar_last_occurrence_wins() {
    // A singular scalar repeated on the wire takes its last value (unlike a singular message).
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 1);
    put_var(&mut buf, 1, 2);
    let arena = Arena::new();
    assert_eq!(Msg::decode(&buf, &arena).unwrap().implicit_i(), 2);
}

#[test]
fn element_conversions_packed_and_unpacked() {
    // Unpacked: one tag per element.
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 199); // uzz: zigzag(-100) == 199 (sint64)
    put_var(&mut buf, 1, 13); // uzz: zigzag(-7) == 13
    put_var(&mut buf, 2, 1); // ub: true
    put_var(&mut buf, 2, 0); // ub: false
    put_var(&mut buf, 3, 5); // uk: Kind::C
    put_tag(&mut buf, 4, 1);
    buf.extend_from_slice(&0x0102_0304_0506_0708u64.to_le_bytes()); // ufx (fixed64)
    put_tag(&mut buf, 5, 5);
    buf.extend_from_slice(&(-2i32).to_le_bytes()); // usf (sfixed32)

    let arena = Arena::new();
    let e = Expanded::decode(&buf, &arena).unwrap();
    assert_eq!(e.uzz(), &[-100, -7]);
    assert_eq!(e.ub(), &[true, false]);
    assert_eq!(e.uk(), &[Kind::C]);
    assert_eq!(e.ufx(), &[0x0102_0304_0506_0708]);
    assert_eq!(e.usf(), &[-2]);

    // Packed: a single length-delimited run per field (the decoder accepts both forms).
    let mut zz = Vec::new();
    put_varint(&mut zz, 199);
    put_varint(&mut zz, 13);
    let mut buf2 = Vec::new();
    put_len(&mut buf2, 1, &zz);
    let e2 = Expanded::decode(&buf2, &arena).unwrap();
    assert_eq!(e2.uzz(), &[-100, -7]);
}

#[test]
fn repeated_scalars_and_enums() {
    let mut buf = Vec::new();
    // nums (6): packed-by-default int32, sent unpacked here.
    for v in [5u64, 6, 7] {
        put_var(&mut buf, 6, v);
    }
    // unpacked (7): declared [packed=false].
    put_var(&mut buf, 7, 11);
    // states (8): repeated enum.
    put_var(&mut buf, 8, 1);
    put_var(&mut buf, 8, 0);
    // reals (12): packed f64 (bulk path).
    let mut reals = Vec::new();
    reals.extend_from_slice(&1.5f64.to_bits().to_le_bytes());
    reals.extend_from_slice(&2.25f64.to_bits().to_le_bytes());
    put_len(&mut buf, 12, &reals);
    // codes (13): packed fixed32 -> u32.
    let mut codes = Vec::new();
    codes.extend_from_slice(&7u32.to_le_bytes());
    put_len(&mut buf, 13, &codes);
    // bools (17): repeated bool.
    put_var(&mut buf, 17, 1);

    let arena = Arena::new();
    let m = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(m.nums(), &[5, 6, 7]);
    assert_eq!(m.unpacked(), &[11]);
    assert_eq!(m.states(), &[State::ON, State::UNKNOWN]);
    assert_eq!(m.reals(), &[1.5, 2.25]);
    assert_eq!(m.codes(), &[7]);
    assert_eq!(m.bools(), &[true]);
    assert!(m.nums().len() == 3 && Msg::decode(&[], &arena).unwrap().nums().is_empty());
}

#[test]
fn singular_message_and_recursion_cap() {
    // self (field 5): a nested Msg with implicit_i set.
    let mut inner = Vec::new();
    put_var(&mut inner, 1, 9);
    let mut buf = Vec::new();
    put_len(&mut buf, 5, &inner);
    let arena = Arena::new();
    let m = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(m.self_().expect("self present").implicit_i(), 9);
    assert!(Msg::decode(&[], &arena).unwrap().self_().is_none());

    // Wrap `levels` self (field 5) layers; the deepest decode_into runs at depth == levels, and the
    // cap rejects at depth > MAX_DECODE_DEPTH (100).
    fn nest(levels: u32) -> Vec<u8> {
        let mut buf = Vec::new();
        for _ in 0..levels {
            let inner = core::mem::take(&mut buf);
            put_len(&mut buf, 5, &inner);
        }
        buf
    }
    assert!(Msg::decode(&nest(100), &arena).is_ok());
    let err = match Msg::decode(&nest(101), &arena) {
        Ok(_) => panic!("expected a depth error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::DepthLimitExceeded);
}

#[test]
fn repeated_singular_message_is_rejected() {
    let mut inner = Vec::new();
    put_var(&mut inner, 1, 1);
    let mut buf = Vec::new();
    put_len(&mut buf, 5, &inner); // self
    put_len(&mut buf, 5, &inner); // self AGAIN -> error (matches C++, not protobuf merge)
    let arena = Arena::new();
    let err = match Msg::decode(&buf, &arena) {
        Ok(_) => panic!("expected a decode error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::RepeatedSingularMessage);
    assert_eq!(err.field_number(), Some(5));
}

#[test]
fn maps_with_scalar_keys_and_values() {
    let arena = Arena::new();
    let mut buf = Vec::new();
    // counts (9): map<string, int32>. Four entries exercising duplicate-key last-wins, an absent key
    // (defaults to ""), and an unknown field inside an entry (skipped, entry still lands).
    map_entry(&mut buf, 9, &len_field(1, b"x"), &varint_field(2, 1));
    map_entry(&mut buf, 9, &len_field(1, b"x"), &varint_field(2, 9));
    map_entry(&mut buf, 9, &len_field(1, b"y"), &varint_field(2, 2));
    map_entry(&mut buf, 9, &varint_field(1, 0), &varint_field(2, 5)); // no key field -> key = ""
                                                                      // An entry with an unknown field 3 between key and value; it must be skipped, the entry kept.
    let mut odd = Vec::new();
    odd.extend_from_slice(&len_field(1, b"w"));
    odd.extend_from_slice(&varint_field(3, 123)); // unknown-in-entry
    odd.extend_from_slice(&varint_field(2, 4));
    put_len(&mut buf, 9, &odd);
    // flags (14): map<bool, string> -- the bool key.
    map_entry(&mut buf, 14, &varint_field(1, 1), &len_field(2, b"on"));
    // toggles (18): map<string, bool>.
    map_entry(&mut buf, 18, &len_field(1, b"t"), &varint_field(2, 1));

    let m = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(m.counts().get("x"), Some(&9)); // last-wins
    assert_eq!(m.counts().get("y"), Some(&2));
    assert_eq!(m.counts().get(""), Some(&5)); // absent key defaulted to ""
    assert_eq!(m.counts().get("w"), Some(&4)); // unknown field in the entry was skipped
    assert_eq!(m.counts().get("z"), None);
    assert_eq!(m.flags().get(&true).map(|s| s.as_bytes()), Some(&b"on"[..]));
    assert_eq!(m.toggles().get("t"), Some(&true));
    assert!(Msg::decode(&[], &arena).unwrap().counts().is_empty());
}

#[test]
fn oneof_last_wins_and_unset() {
    let arena = Arena::new();
    // a (10, int32) then b (11, string): last tag wins.
    let mut buf = Vec::new();
    put_var(&mut buf, 10, 7);
    put_len(&mut buf, 11, b"txt");
    let m = Msg::decode(&buf, &arena).unwrap();
    match m.pick() {
        Some(Pick::B(s)) => assert_eq!(s.as_bytes(), b"txt"),
        other => panic!("expected B, got {}", other.is_some()),
    }
    // c (15, bool) and d (16, double) variants.
    let mut cbuf = Vec::new();
    put_var(&mut cbuf, 15, 1);
    assert!(matches!(
        Msg::decode(&cbuf, &arena).unwrap().pick(),
        Some(Pick::C(true))
    ));
    let mut dbuf = Vec::new();
    put_tag(&mut dbuf, 16, 1);
    dbuf.extend_from_slice(&2.5f64.to_bits().to_le_bytes());
    match Msg::decode(&dbuf, &arena).unwrap().pick() {
        Some(Pick::D(x)) => assert_eq!(*x, 2.5),
        _ => panic!("expected D"),
    }
    // unset.
    assert!(Msg::decode(&[], &arena).unwrap().pick().is_none());
}

#[test]
fn message_valued_map_oneof_and_repeated_message() {
    let arena = Arena::new();
    let nested = |x: i64| {
        let mut n = Vec::new();
        put_var(&mut n, 1, x as u64);
        n
    };

    let mut buf = Vec::new();
    // by_name (1): map<string, Nested>.
    map_entry(&mut buf, 1, &len_field(1, b"a"), &len_field(2, &nested(5)));
    // by_name with an ABSENT value -> the default (empty) Nested.
    map_entry(&mut buf, 1, &len_field(1, b"empty"), &[]);
    // by_id (2): map<int32, Color>.
    map_entry(&mut buf, 2, &varint_field(1, 7), &varint_field(2, 1));
    // choice (oneof): cn (4) = Nested.
    put_len(&mut buf, 4, &nested(42));
    // items (5): repeated Nested.
    put_len(&mut buf, 5, &nested(1));
    put_len(&mut buf, 5, &nested(2));

    let c = Container::decode(&buf, &arena).unwrap();
    assert_eq!(c.by_name().get("a").map(Nested::x), Some(Some(5)));
    assert_eq!(c.by_name().get("empty").map(Nested::x), Some(None)); // default Nested: x absent
    assert_eq!(c.by_id().get(&7), Some(&Color::RED));
    match c.choice() {
        Some(Choice::Cn(n)) => assert_eq!(n.x(), Some(42)),
        _ => panic!("expected Cn"),
    }
    assert_eq!(c.items().len(), 2);
    assert_eq!(c.items()[0].x(), Some(1));
    assert_eq!(c.items()[1].x(), Some(2));

    // A map message value appearing twice IN ONE ENTRY -> RepeatedSingularMessage.
    let mut dup = Vec::new();
    let dup_entry = {
        let mut e = Vec::new();
        put_len(&mut e, 1, b"k"); // key
        put_len(&mut e, 2, &nested(1)); // value
        put_len(&mut e, 2, &nested(2)); // value AGAIN
        e
    };
    put_len(&mut dup, 1, &dup_entry);
    let err = match Container::decode(&dup, &arena) {
        Ok(_) => panic!("expected a decode error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::RepeatedSingularMessage);
    assert_eq!(err.field_number(), Some(1));
}

#[test]
fn required_all_present_ok_and_missing_rejected() {
    let arena = Arena::new();
    // All 65 required fields present.
    let mut buf = Vec::new();
    for f in 1..=65u32 {
        put_var(&mut buf, f, u64::from(f));
    }
    let m = ManyRequired::decode(&buf, &arena).unwrap();
    assert_eq!(m.f1(), 1);
    assert_eq!(m.f65(), 65);

    // Omit field 33: MissingRequired, keyed to that field.
    let mut hole = Vec::new();
    for f in 1..=65u32 {
        if f != 33 {
            put_var(&mut hole, f, u64::from(f));
        }
    }
    let err = match ManyRequired::decode(&hole, &arena) {
        Ok(_) => panic!("expected MissingRequired"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::MissingRequired);
    assert_eq!(err.field_number(), Some(33));
}

#[test]
fn unknown_and_wrong_wire_fields_are_skipped() {
    let mut buf = Vec::new();
    put_var(&mut buf, 99, 7); // unknown varint field
    put_len(&mut buf, 100, b"junk"); // unknown length-delimited field
    put_len(&mut buf, 1, b"xyz"); // field 1 (implicit_i) as LEN not Varint: wrong wire -> skipped
    put_var(&mut buf, 1, 5); // the real field after the junk
    let arena = Arena::new();
    let m = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(
        m.implicit_i(),
        5,
        "the real field after skipped junk still decodes"
    );
}

#[test]
fn truncated_value_errors_at_a_body_relative_offset() {
    // field 2 (explicit_i) tag (1 byte) then a lone continuation byte: the varint is truncated.
    let buf = [0x10u8, 0x80];
    let arena = Arena::new();
    let err = match Msg::decode(&buf, &arena) {
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
fn oneof_repeated_message_member_is_rejected() {
    let arena = Arena::new();
    let nested = |x: i64| {
        let mut n = Vec::new();
        put_var(&mut n, 1, x as u64);
        n
    };
    // The same message member twice while the oneof still holds it -> rejected (protobuf would merge
    // the two occurrences; a read-only tree cannot, and last-wins would differ from protobuf).
    let mut buf = Vec::new();
    put_len(&mut buf, 4, &nested(7)); // choice.cn = Nested{x=7}
    put_len(&mut buf, 4, &[]); // cn again while cn is held
    let err = match Container::decode(&buf, &arena) {
        Ok(_) => panic!("expected RepeatedSingularMessage"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::RepeatedSingularMessage);
    assert_eq!(err.field_number(), Some(4));

    // An intervening different member (ci) clears the slot, so the later cn starts fresh and decodes.
    let mut buf2 = Vec::new();
    put_len(&mut buf2, 4, &nested(7)); // cn
    put_var(&mut buf2, 3, 5); // ci -> clears cn
    put_len(&mut buf2, 4, &[]); // cn again, now fresh (empty)
    let c = Container::decode(&buf2, &arena).unwrap();
    match c.choice() {
        Some(Choice::Cn(n)) => assert_eq!(n.x(), None), // the fresh empty cn
        other => panic!("expected a fresh empty Cn, got present={}", other.is_some()),
    }

    // A scalar oneof member repeating is plain last-wins (never rejected).
    let mut buf3 = Vec::new();
    put_var(&mut buf3, 3, 1); // ci = 1
    put_var(&mut buf3, 3, 2); // ci = 2 (wins)
    let c3 = Container::decode(&buf3, &arena).unwrap();
    assert!(matches!(c3.choice(), Some(Choice::Ci(2))));
}

#[test]
fn proto2_every_scalar_type_set_on_the_wire() {
    // p2.Scalars carries every scalar type (with proto2 defaults); set each explicitly here so the
    // per-type wire read + conversion is exercised end to end. Field 1 (i32) is `required`.
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 300); // i32
    put_var(&mut buf, 2, (-5i64) as u64); // i64 (negative -> 10-byte varint)
    put_var(&mut buf, 3, 4_000_000_000); // u32 (> i32::MAX)
    put_var(&mut buf, 4, 1_000_000_000_000_000_000); // u64
    put_var(&mut buf, 5, 13); // s32: zigzag(-7) == 13
    put_var(&mut buf, 6, 199); // s64: zigzag(-100) == 199
    put_tag(&mut buf, 7, 5);
    buf.extend_from_slice(&0xDEAD_BEEFu32.to_le_bytes()); // f32 (fixed32)
    put_tag(&mut buf, 8, 1);
    buf.extend_from_slice(&0x0102_0304_0506_0708u64.to_le_bytes()); // f64 (fixed64)
    put_tag(&mut buf, 9, 5);
    buf.extend_from_slice(&(-2i32).to_le_bytes()); // sf32
    put_tag(&mut buf, 10, 1);
    buf.extend_from_slice(&(-3i64).to_le_bytes()); // sf64
    put_var(&mut buf, 11, 0); // b = false (overrides the `default = true`)
    put_len(&mut buf, 12, b"xy"); // s
    put_len(&mut buf, 13, &[0xDE, 0xAD]); // by
    put_tag(&mut buf, 14, 5);
    buf.extend_from_slice(&1.5f32.to_bits().to_le_bytes()); // fl
    put_tag(&mut buf, 15, 1);
    buf.extend_from_slice(&2.25f64.to_bits().to_le_bytes()); // db
    put_var(&mut buf, 16, 1); // color = COLOR_RED

    let arena = Arena::new();
    let m = Scalars::decode(&buf, &arena).unwrap();
    assert_eq!(m.i32(), 300); // required -> bare
    assert_eq!(m.i64(), Some(-5));
    assert_eq!(m.u32(), Some(4_000_000_000));
    assert_eq!(m.u64(), Some(1_000_000_000_000_000_000));
    assert_eq!(m.s32(), Some(-7));
    assert_eq!(m.s64(), Some(-100));
    assert_eq!(m.f32(), Some(0xDEAD_BEEF));
    assert_eq!(m.f64(), Some(0x0102_0304_0506_0708));
    assert_eq!(m.sf32(), Some(-2));
    assert_eq!(m.sf64(), Some(-3));
    assert_eq!(m.b(), Some(false));
    assert_eq!(m.s().map(|s| s.as_bytes()), Some(&b"xy"[..]));
    assert_eq!(m.by(), Some(&b"\xDE\xAD"[..]));
    assert_eq!(m.fl(), Some(1.5));
    assert_eq!(m.db(), Some(2.25));
    assert_eq!(m.color(), Some(Color::RED));
}

#[test]
fn proto2_absent_fields_are_none_defaults_not_materialized() {
    // Only the `required` field 1 present. Every absent optional reads None -- a proto2 `[default=...]`
    // is NOT read through the decoder (matching the C++ decoder; a consumer applies it via unwrap_or).
    let arena = Arena::new();
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 7); // i32 (required) must be present
    let m = Scalars::decode(&buf, &arena).unwrap();
    assert_eq!(m.i32(), 7);
    // Defaulted fields absent -> None (the default -7/42/true/"hi.."/1.5/-2.25/RED is NOT applied here).
    assert_eq!(m.i64(), None);
    assert_eq!(m.b(), None);
    assert_eq!(m.s(), None);
    assert_eq!(m.by(), None);
    assert_eq!(m.fl(), None);
    assert_eq!(m.db(), None);
    assert_eq!(m.color(), None);
    // Optionals without a default -> also None.
    assert_eq!(m.u32(), None);
    assert_eq!(m.s32(), None);
    assert_eq!(m.f32(), None);
    assert_eq!(m.sf64(), None);

    // A missing required field is still an error.
    let err = match Scalars::decode(&[], &arena) {
        Ok(_) => panic!("expected MissingRequired"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::MissingRequired);
    assert_eq!(err.field_number(), Some(1));
}

#[test]
fn field_modes_drop_raw_unknown() {
    let arena = Arena::new();
    // A payload for the `raw` self field (field 5): a small Msg body, kept undecoded.
    let mut inner = Vec::new();
    put_var(&mut inner, 1, 9);

    let mut buf = Vec::new();
    put_var(&mut buf, 1, 300); // implicit_i (kept)
    put_len(&mut buf, 3, b"x"); // name (DROPPED: skip-validated, ignored, not "unknown")
    put_len(&mut buf, 5, &inner); // self (RAW: payload stored undecoded)
    put_var(&mut buf, 99, 7); // a genuinely unknown field -> trips has_unknown_fields()

    let m = ModedMsg::decode(&buf, &arena).unwrap();
    assert_eq!(m.implicit_i(), 300);
    assert_eq!(m.self_(), Some(&inner[..])); // raw: the exact payload bytes, not a decoded Msg
    assert!(m.has_unknown_fields());

    // A dropped field present on the wire is a KNOWN field, so it must not trip the unknown bit; and
    // an absent raw field reads None.
    let mut buf2 = Vec::new();
    put_var(&mut buf2, 1, 1);
    put_len(&mut buf2, 3, b"still dropped");
    let m2 = ModedMsg::decode(&buf2, &arena).unwrap();
    assert_eq!(m2.implicit_i(), 1);
    assert_eq!(m2.self_(), None);
    assert!(!m2.has_unknown_fields());

    // A KNOWN field number arriving with the WRONG wire type is skipped as known (not "unknown") --
    // so it must NOT trip has_unknown_fields() (matching C++, which flags only unrecognized numbers).
    let mut buf3 = Vec::new();
    put_var(&mut buf3, 1, 5); // implicit_i (correct wire)
    put_len(&mut buf3, 1, b"wrong wire for field 1"); // field 1 as LEN -> skipped as known
    let m3 = ModedMsg::decode(&buf3, &arena).unwrap();
    assert_eq!(m3.implicit_i(), 5);
    assert!(!m3.has_unknown_fields());
}

#[test]
fn malformed_tag_is_an_error() {
    let arena = Arena::new();
    // A malformed ONE-byte tag (field number 0: 0x00) is InvalidTag. The decode loop's peek hub takes
    // a one-byte tag without validating it and only rejects it when the catch-all's `skip_field` sees
    // the bad field/wire, so the diagnostic offset is one byte past the tag (1), not the tag start.
    let err = match Msg::decode(&[0x00u8], &arena) {
        Ok(_) => panic!("expected a decode error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::InvalidTag);
    assert_eq!(err.offset(), 1);
    // A malformed MULTI-byte tag still goes through `read_tag`, which validates before advancing, so it
    // is reported at the tag start (0). Field 2047 with reserved wire type 7: (2047 << 3 | 7) = 0x3fff
    // -> 2-byte varint 0xff 0x7f.
    let err = match Msg::decode(&[0xffu8, 0x7f], &arena) {
        Ok(_) => panic!("expected a decode error"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::InvalidTag);
    assert_eq!(err.offset(), 0);
}

#[test]
fn inlined_small_messages_decode_in_place() {
    // Layout inlines small fixed-size sub-messages BY VALUE with a presence bit (pt: Point 8B,
    // flag: BoolWrap) and decodes them in place (decode_value, no arena alloc); Big (24B > 16B cutoff)
    // and HasString (not fixed-size) stay behind arena pointers. Exercises the inlined singular-message
    // decode path + its `Option<&T>` presence getter, the repeated inlined-element array, and that an
    // absent inlined field reads `None` (its `Default::default()` storage never surfaces).
    let mut pt = Vec::new();
    put_var(&mut pt, 1, 10); // Point.x
    put_var(&mut pt, 2, 20); // Point.y
    let mut flag = Vec::new();
    put_var(&mut flag, 1, 1); // BoolWrap.value = true
    let mut big = Vec::new();
    put_tag(&mut big, 1, 1); // Big.a (fixed64, wire type I64)
    big.extend_from_slice(&100u64.to_le_bytes());

    let mut buf = Vec::new();
    put_var(&mut buf, 1, 1); // b1 = true (implicit bool -> value bit)
    put_var(&mut buf, 4, 42); // small (implicit i32)
    put_var(&mut buf, 5, 7); // opt (explicit i32 -> presence bit)
    put_len(&mut buf, 9, &pt); // pt (inlined Point)
    put_len(&mut buf, 10, &big); // bg (pointer Big)
    put_len(&mut buf, 12, &flag); // flag (inlined BoolWrap)

    let arena = Arena::new();
    let m = Layout::decode(&buf, &arena).unwrap();
    assert!(m.b1());
    assert_eq!(m.small(), 42);
    assert_eq!(m.opt(), Some(7));
    let p = m.pt().expect("pt present");
    assert_eq!(p.x(), 10);
    assert_eq!(p.y(), 20);
    assert_eq!(m.bg().expect("bg present").a(), 100);
    assert!(m.flag().expect("flag present").value());

    // Repeated inlined message: an array of bare `Point` decoded per element.
    let mut pt2 = Vec::new();
    put_var(&mut pt2, 1, 30); // Point.x (y defaults 0)
    let mut buf2 = Vec::new();
    put_len(&mut buf2, 14, &pt); // points[0]
    put_len(&mut buf2, 14, &pt2); // points[1]
    let m2 = Layout::decode(&buf2, &arena).unwrap();
    assert_eq!(m2.points().len(), 2);
    assert_eq!(m2.points()[0].x(), 10);
    assert_eq!(m2.points()[0].y(), 20);
    assert_eq!(m2.points()[1].x(), 30);
    assert_eq!(m2.points()[1].y(), 0);

    // Absent: inlined-message presence bits clear -> None; pointer sub-message null -> None.
    let d = Layout::decode(&[], &arena).unwrap();
    assert!(d.pt().is_none());
    assert!(d.flag().is_none());
    assert!(d.bg().is_none());
    assert_eq!(d.opt(), None);
    assert!(!d.b1());
    assert!(d.points().is_empty());
}

#[test]
fn required_inlined_message_present_ok_and_missing_rejected() {
    // A `required` small fixed-size message inlines by value with NO resting presence bit (required is
    // always-present once decode succeeds); the getter returns `Some(&..)` unconditionally and a
    // duplicate is rejected via the transient required flag. Present: decodes in place; absent: the
    // post-loop required check fires MissingRequired. (Regression: this shape emitted `1 << -1`.)
    let mut small = Vec::new();
    put_var(&mut small, 1, 5); // Small.x
    let mut buf = Vec::new();
    put_len(&mut buf, 1, &small); // required Small s
    put_var(&mut buf, 2, 9); // optional int32 tag

    let arena = Arena::new();
    let m = ReqInline::decode(&buf, &arena).unwrap();
    assert_eq!(m.s().expect("required s present").x(), Some(5));
    assert_eq!(m.tag(), Some(9));

    // Missing the required message -> MissingRequired on field 1.
    let err = match ReqInline::decode(&[], &arena) {
        Ok(_) => panic!("expected MissingRequired"),
        Err(e) => e,
    };
    assert_eq!(err.kind(), ErrorKind::MissingRequired);
}
