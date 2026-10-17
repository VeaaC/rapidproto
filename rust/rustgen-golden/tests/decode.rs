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
use rustgen_golden::arena_modes_lean::arena::fm::holder::Grp as ModedGrp;
use rustgen_golden::arena_modes_lean::arena::fm::Holder as ModedHolder;
use rustgen_golden::arena_naming::arena::an::collide::Letters;
use rustgen_golden::arena_naming::arena::an::Collide;
use rustgen_golden::arena_unknown_tracked::arena::au::Holder as UnkHolder;
use rustgen_golden::editions2023::arena::ed23::M as Ed23M;
use rustgen_golden::editions2024::arena::ed24::M as Ed24M;
use rustgen_golden::expanded::arena::xp::Expanded;
use rustgen_golden::expanded::common::xp::expanded::Kind;
use rustgen_golden::main::arena::main::main::Choice as MainChoice;
use rustgen_golden::main::arena::main::Main;
use rustgen_golden::messageset::arena::ms::Outer as MsOuter;
use rustgen_golden::messageset_unknown::arena::ms::Outer as MsuOuter;
use rustgen_golden::proto2::arena::p2::container::{Choice, Nested};
use rustgen_golden::proto2::arena::p2::oneof_group::Pick as GroupPick;
use rustgen_golden::proto2::arena::p2::oneof_group_straddle::Pick as StraddlePick;
use rustgen_golden::proto2::arena::p2::{
    Container, GroupNumberReuse, OneofGroup, OneofGroupStraddle, RepeatedGroup, Scalars, WithGroup,
};
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
fn threaded_fields_decode_regardless_of_wire_order() {
    // The decode loop threads singular scalar/enum fields with an ascending-order straight-line
    // prologue, probing a one-byte tag for fields 1 implicit_i / 2 explicit_i / 4 state and a TWO-byte
    // tag for fields 20 ratio (fixed64) / 21 scale (fixed32). Fields written OUT OF ORDER (descending
    // here) make the prologue probes mostly miss, so the general hub loop must decode them to the same
    // result -- the one- and two-byte fast paths and the fallback cannot disagree.
    let arena = Arena::new();
    let put_f64 = |b: &mut Vec<u8>, field: u32, v: f64| {
        put_tag(b, field, 1);
        b.extend_from_slice(&v.to_bits().to_le_bytes());
    };
    let put_f32 = |b: &mut Vec<u8>, field: u32, v: f32| {
        put_tag(b, field, 5);
        b.extend_from_slice(&v.to_bits().to_le_bytes());
    };
    let mut ascending = Vec::new();
    put_var(&mut ascending, 1, 300); // implicit_i (1-byte tag)
    put_var(&mut ascending, 2, 42); // explicit_i (1-byte tag)
    put_var(&mut ascending, 4, 1); // state = ON (1-byte tag)
    put_f64(&mut ascending, 20, 2.5); // ratio (2-byte tag)
    put_f32(&mut ascending, 21, 1.5); // scale (2-byte tag)
    let mut descending = Vec::new();
    put_f32(&mut descending, 21, 1.5);
    put_f64(&mut descending, 20, 2.5);
    put_var(&mut descending, 4, 1);
    put_var(&mut descending, 2, 42);
    put_var(&mut descending, 1, 300);

    for buf in [&ascending, &descending] {
        let m = Msg::decode(buf, &arena).unwrap();
        assert_eq!(m.implicit_i(), 300);
        assert_eq!(m.explicit_i(), Some(42));
        assert_eq!(m.state(), State::ON);
        assert_eq!(m.ratio(), 2.5);
        assert_eq!(m.scale(), 1.5);
    }
}

#[test]
fn threaded_repeated_oneof_and_message_match_fallback() {
    // Threading covers every field kind, not just scalars: a repeated field self-loops on its own
    // element tag (nums, 6), a oneof member is threaded (pick::a, 10), and a singular sub-message is
    // threaded (self, 5). Decoding the SAME fields ascending (prologue threads them) vs scrambled
    // (prologue misses, the hub loop decodes them) must agree -- the self-loop, oneof slot, and
    // in-place message decode cannot diverge from the fallback.
    let arena = Arena::new();
    let mut nested = Vec::new();
    put_var(&mut nested, 1, 9); // self.implicit_i = 9

    let mut ascending = Vec::new();
    put_len(&mut ascending, 5, &nested); // self (message)
    put_var(&mut ascending, 6, 100); // nums[0] (repeated, unpacked)
    put_var(&mut ascending, 6, 200); // nums[1]
    put_var(&mut ascending, 6, 300); // nums[2]
    map_entry(&mut ascending, 9, &len_field(1, b"p"), &varint_field(2, 1)); // counts (map) self-loop
    map_entry(&mut ascending, 9, &len_field(1, b"q"), &varint_field(2, 2));
    put_var(&mut ascending, 10, 42); // pick::a (oneof)

    let mut scrambled = Vec::new();
    put_var(&mut scrambled, 10, 42); // oneof first -> everything after falls to the hub
    map_entry(&mut scrambled, 9, &len_field(1, b"p"), &varint_field(2, 1));
    map_entry(&mut scrambled, 9, &len_field(1, b"q"), &varint_field(2, 2));
    put_len(&mut scrambled, 5, &nested); // then the message
    put_var(&mut scrambled, 6, 100); // then the repeated run
    put_var(&mut scrambled, 6, 200);
    put_var(&mut scrambled, 6, 300);

    for buf in [&ascending, &scrambled] {
        let m = Msg::decode(buf, &arena).unwrap();
        assert_eq!(m.nums(), &[100, 200, 300]);
        assert_eq!(m.self_().expect("self present").implicit_i(), 9);
        assert!(matches!(m.pick(), Some(Pick::A(42))));
        assert_eq!(m.counts().get("p"), Some(&1));
        assert_eq!(m.counts().get("q"), Some(&2));
    }
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
    // Decoding to MAX_DECODE_DEPTH recurses that deep. In a RELEASE build the cap holds within a
    // default stack -- 100-deep fits even a 1 MiB thread stack (measured) -- so a real consumer on
    // any default thread stack gets DepthLimitExceeded, never a stack overflow: the cap does its job.
    // A DEBUG build's unoptimized `decode_into` frames are several times larger, so ~100 levels
    // exceed the default 2 MiB TEST-thread stack. Give the (debug) test thread headroom; it asserts
    // the depth CAP, not the stack (the guard stops at 100 either way -- verified with a large stack).
    std::thread::Builder::new()
        .stack_size(32 * 1024 * 1024)
        .spawn(|| {
            let arena = Arena::new();
            assert!(Msg::decode(&nest(100), &arena).is_ok());
            match Msg::decode(&nest(101), &arena) {
                Ok(_) => panic!("expected a depth error"),
                Err(e) => assert_eq!(e.kind(), ErrorKind::DepthLimitExceeded),
            }
        })
        .expect("spawn deep-decode thread")
        .join()
        .expect("deep-decode thread panicked");
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

// --- proto2 groups (delimited wire: SGROUP..matching EGROUP; nested / repeated / field-reuse) ---

#[test]
fn group_nested_decodes_through_matching_end_tags() {
    // WithGroup { MyGroup(1) { a(2)=42, Inner(3) { flag(4)=true } } }
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 3); // start MyGroup (field 1)
    put_var(&mut buf, 2, 42); //   a = 42
    put_tag(&mut buf, 3, 3); //   start Inner (field 3)
    put_var(&mut buf, 4, 1); //     flag = true
    put_tag(&mut buf, 3, 4); //   end Inner
    put_tag(&mut buf, 1, 4); // end MyGroup
    let arena = Arena::new();
    let m = WithGroup::decode(&buf, &arena).unwrap();
    let g = m.mygroup().unwrap();
    assert_eq!(g.a(), Some(42));
    assert_eq!(g.inner().unwrap().flag(), Some(true));
}

#[test]
fn group_end_scan_matches_field_number_not_inner_reuse() {
    // GroupNumberReuse { G(2) { two(2)=7 } } -- the end scan must pair EGROUP(2) with the group, not
    // let the inner field-2 value be mistaken for the end (the field number recurs inside the body).
    let mut buf = Vec::new();
    put_tag(&mut buf, 2, 3); // start G (field 2)
    put_var(&mut buf, 2, 7); //   two = 7 (field number 2 reused inside)
    put_tag(&mut buf, 2, 4); // end G
    let arena = Arena::new();
    let m = GroupNumberReuse::decode(&buf, &arena).unwrap();
    assert_eq!(m.g().unwrap().two(), Some(7));
}

#[test]
fn repeated_group_accumulates_elements() {
    // RepeatedGroup { Item(1){v(2)=10} Item(1){v(2)=20} tail(3)=99 } -- the google_message2 shape.
    let mut buf = Vec::new();
    for v in [10u64, 20] {
        put_tag(&mut buf, 1, 3); // start Item
        put_var(&mut buf, 2, v); //   v
        put_tag(&mut buf, 1, 4); // end Item
    }
    put_var(&mut buf, 3, 99); // tail
    let arena = Arena::new();
    let m = RepeatedGroup::decode(&buf, &arena).unwrap();
    let items = m.item();
    assert_eq!(items.len(), 2);
    assert_eq!(items[0].v(), Some(10));
    assert_eq!(items[1].v(), Some(20));
    assert_eq!(m.tail(), Some(99));
}

#[test]
fn oneof_group_member_decodes() {
    // OneofGroup { pick = G(2){ x(3)=5, s(4)="hi" }, after(5)=1 }
    let mut buf = Vec::new();
    put_tag(&mut buf, 2, 3); // start G (oneof member, field 2)
    put_var(&mut buf, 3, 5); //   x = 5
    put_len(&mut buf, 4, b"hi"); //   s = "hi"
    put_tag(&mut buf, 2, 4); // end G
    put_var(&mut buf, 5, 1); // after = 1
    let arena = Arena::new();
    let m = OneofGroup::decode(&buf, &arena).unwrap();
    assert_eq!(m.after(), Some(1));
    match m.pick() {
        Some(GroupPick::G(g)) => {
            assert_eq!(g.x(), Some(5));
            assert_eq!(g.s().unwrap().to_str().unwrap(), "hi");
        }
        _ => panic!("expected the G oneof member"),
    }
}

#[test]
fn group_unterminated_errors_without_panicking() {
    // A start-group tag with no matching end must error cleanly (never crash / loop forever).
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 3); // start MyGroup, then no matching end
    put_var(&mut buf, 2, 42);
    let arena = Arena::new();
    assert!(WithGroup::decode(&buf, &arena).is_err());
}

#[test]
fn oneof_group_straddle_decodes_either_member() {
    // OneofGroupStraddle mixes an (unthreadable) group member Head(2){x(3)} with an int member tail(5)
    // in one oneof, plus a plain mid(4). Both oneof routes must decode, and the plain field alongside.
    // Group member: Head { x = 7 }, mid = 4.
    let mut g = Vec::new();
    put_tag(&mut g, 2, 3); // start Head (field 2)
    put_var(&mut g, 3, 7); //   x = 7
    put_tag(&mut g, 2, 4); // end Head
    put_var(&mut g, 4, 4); // mid = 4
    let arena = Arena::new();
    let m = OneofGroupStraddle::decode(&g, &arena).unwrap();
    assert_eq!(m.mid(), Some(4));
    match m.pick() {
        Some(StraddlePick::Head(h)) => assert_eq!(h.x(), Some(7)),
        _ => panic!("expected the Head group member"),
    }
    // Int member: tail(5) = 9 (reached past the plain field 4 probe).
    let mut i = Vec::new();
    put_var(&mut i, 5, 9);
    let arena2 = Arena::new();
    let m2 = OneofGroupStraddle::decode(&i, &arena2).unwrap();
    assert!(matches!(m2.pick(), Some(StraddlePick::Tail(9))));
}

#[test]
fn repeated_group_rejects_a_malformed_element() {
    // A good first element then a second whose group is never closed must error cleanly, not hang or
    // silently keep the first (the never-crash invariant on adversarial input).
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 3); // start Item
    put_var(&mut buf, 2, 10); //   v = 10
    put_tag(&mut buf, 1, 4); // end Item
    put_tag(&mut buf, 1, 3); // start a SECOND Item with no matching end
    put_var(&mut buf, 2, 20);
    let arena = Arena::new();
    assert!(RepeatedGroup::decode(&buf, &arena).is_err());
}

#[test]
fn group_with_mismatched_nested_end_errors() {
    // A nested group (field 3) closed by an end tag for a DIFFERENT field number must be rejected while
    // scanning the outer group's body -- not swallowed -- so the outer group never finds its own end.
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 3); // start MyGroup (field 1)
    put_tag(&mut buf, 3, 3); //   start Inner (field 3)
    put_tag(&mut buf, 9, 4); //   end-group for field 9 (mismatched) -> malformed
    put_tag(&mut buf, 1, 4); // (unreached) end MyGroup
    let arena = Arena::new();
    assert!(WithGroup::decode(&buf, &arena).is_err());
}

#[test]
fn raw_group_field_stores_its_bare_body_undecoded() {
    // Holder under the `lean` field-modes profile (mirrors the C++ arena_modes golden): `grp` is a
    // proto2 GROUP field (14) kept RAW, and `req_blob` (13) a raw message field -- both stored as
    // borrowed, undecoded payload bytes. A raw group's payload is its BARE body (SGROUP/EGROUP framing
    // stripped by read_group), the same view the C++ `test_arena_decode` raw-payloads test asserts.
    let mut blob = Vec::new();
    put_len(&mut blob, 1, b"r"); // Blob { payload = "r" }
    let mut body = Vec::new();
    put_var(&mut body, 1, 5); // the group body: g (field 1) = 5

    let mut buf = Vec::new();
    put_var(&mut buf, 5, 7); // must = 7 (required)
    put_len(&mut buf, 13, &blob); // req_blob (required, raw) -> stores the Blob payload bytes
    put_tag(&mut buf, 14, 3); // grp: start group (field 14)
    buf.extend_from_slice(&body); //   group body
    put_tag(&mut buf, 14, 4); // grp: end group

    let arena = Arena::new();
    let h = ModedHolder::decode(&buf, &arena).unwrap();
    assert_eq!(h.must(), 7);
    assert_eq!(h.req_blob(), Some(&blob[..])); // raw message: the length-delimited payload
    assert_eq!(h.grp(), Some(&body[..])); // raw group: the bare body, framing stripped
                                          // The stored bare body is a valid group payload: it re-decodes through the group's own decoder.
    let g = ModedGrp::decode(h.grp().unwrap(), &arena).unwrap();
    assert_eq!(g.g(), Some(5));
}

#[test]
fn raw_required_group_bearing_holder_rejects_missing_required() {
    // The same message with the required `must`/`req_blob` omitted still errors cleanly (never panics),
    // so the raw-group path composes with required validation.
    let mut buf = Vec::new();
    put_tag(&mut buf, 14, 3);
    put_var(&mut buf, 1, 5);
    put_tag(&mut buf, 14, 4);
    let arena = Arena::new();
    assert!(matches!(
        ModedHolder::decode(&buf, &arena),
        Err(e) if e.kind() == ErrorKind::MissingRequired
    ));
}

// --- parity pendants for tests/test_arena_decode.cpp cases with no prior Rust coverage ------------

#[test]
fn a_capacity_limited_arena_surfaces_arena_exhausted() {
    // A tiny arena cannot even allocate the root message -> decode returns ArenaExhausted, never
    // panics (the never-crash invariant under allocation failure). Mirrors C++ "capacity-limited
    // arena surfaces OutOfMemory".
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 1); // implicit_i = 1
    let arena = Arena::new();
    arena.set_capacity_limit(8); // smaller than a Msg
    assert!(matches!(
        Msg::decode(&buf, &arena),
        Err(e) if e.kind() == ErrorKind::ArenaExhausted
    ));
}

#[test]
fn packed_varint_fill_empty_regrow_and_mixed_forms() {
    // nums (field 6, packed int32): (a) an empty packed span yields an empty array; (b) two packed
    // occurrences of the same field accumulate (the second re-grows after the first's trim); (c) a
    // packed run followed by a bare expanded element of the same field accumulate. Mirrors C++
    // "packed-varint fill (pre-size / trim / re-grow / mixed forms)".
    let mut empty = Vec::new();
    put_len(&mut empty, 6, &[]); // zero-length packed span
    let arena0 = Arena::new();
    assert!(Msg::decode(&empty, &arena0).unwrap().nums().is_empty());

    let mut buf = Vec::new();
    let mut run1 = Vec::new();
    for v in [1u64, 2, 3] {
        put_varint(&mut run1, v);
    }
    put_len(&mut buf, 6, &run1); // first packed run
    let mut run2 = Vec::new();
    for v in [4u64, 5] {
        put_varint(&mut run2, v);
    }
    put_len(&mut buf, 6, &run2); // second packed run -> re-grow
    put_var(&mut buf, 6, 6); // a bare (expanded) element of the same field
    let arena = Arena::new();
    let m = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(m.nums(), &[1, 2, 3, 4, 5, 6]);
}

#[test]
fn non_minimal_tag_decodes_identically_to_the_canonical_form() {
    // An over-long (non-minimal) varint tag decodes the same field as its canonical one-byte form --
    // the general (multi-byte) tag path accepts it, like the C++ reader. name's canonical tag is 0x1a
    // = (3<<3)|2; encode it over-long as 0x9a 0x00. Mirrors C++ "non-minimal tags decode identically".
    let mut buf = Vec::new();
    buf.extend_from_slice(&[0x9a, 0x00]); // (3<<3)|2, two-byte non-minimal encoding
    put_varint(&mut buf, 2);
    buf.extend_from_slice(b"hi"); // the LEN payload
    let arena = Arena::new();
    let m = Msg::decode(&buf, &arena).unwrap();
    assert_eq!(m.name().as_bytes(), b"hi");

    // ...and across a different field kind -- a varint field (implicit_i, field 1): canonical tag
    // 0x08, encoded over-long as 0x88 0x00.
    let mut vbuf = Vec::new();
    vbuf.extend_from_slice(&[0x88, 0x00]); // (1<<3)|0, two-byte non-minimal
    put_varint(&mut vbuf, 42);
    let va = Arena::new();
    assert_eq!(Msg::decode(&vbuf, &va).unwrap().implicit_i(), 42);
}

#[test]
fn strings_borrow_the_input_buffer_zero_copy() {
    // A decoded string view aliases the input bytes (zero-copy borrow), not an arena copy. Mirrors
    // C++ "strings borrow the input buffer (no copy)".
    let mut buf = Vec::new();
    put_len(&mut buf, 3, b"borrowed"); // name
    let arena = Arena::new();
    let m = Msg::decode(&buf, &arena).unwrap();
    let name = m.name().as_bytes();
    assert_eq!(name, b"borrowed");
    let base = buf.as_ptr() as usize;
    let p = name.as_ptr() as usize;
    assert!(p >= base && p + name.len() <= base + buf.len()); // the view points into the input
}

// A minimal valid arena_modes_lean Holder (must + req_blob present) that further fields extend.
fn moded_holder_base(buf: &mut Vec<u8>) {
    put_var(buf, 5, 1); // must (required)
    let mut rb = Vec::new();
    put_len(&mut rb, 1, b"r"); // Blob { payload = "r" }
    put_len(buf, 13, &rb); // req_blob (required, raw)
}

#[test]
fn raw_message_present_empty_is_distinct_from_absent() {
    // blob (field 7, raw singular message): present with a zero-length payload is a non-null empty
    // view, distinct from absent (None). Mirrors C++ "raw absence and present-empty payloads".
    let mut absent = Vec::new();
    moded_holder_base(&mut absent);
    let arena = Arena::new();
    assert_eq!(ModedHolder::decode(&absent, &arena).unwrap().blob(), None);

    let mut present = Vec::new();
    moded_holder_base(&mut present);
    put_len(&mut present, 7, &[]); // blob present, empty payload
    let arena2 = Arena::new();
    let b = ModedHolder::decode(&present, &arena2).unwrap().blob();
    assert_eq!(b, Some(&[][..])); // present-and-empty (not None)
}

#[test]
fn raw_singular_message_rejects_a_second_occurrence() {
    // blob (field 7): a second occurrence -> RepeatedSingularMessage (even when the first was
    // present-empty); a wrong-wire-type field-7 occurrence is skip-validated, not counted as a dup.
    let mut dup = Vec::new();
    moded_holder_base(&mut dup);
    put_len(&mut dup, 7, &[]); // first (empty) -> "seen"
    put_len(&mut dup, 7, b"x"); // second -> reject
    let arena = Arena::new();
    assert!(matches!(
        ModedHolder::decode(&dup, &arena),
        Err(e) if e.kind() == ErrorKind::RepeatedSingularMessage
    ));

    let mut wrong = Vec::new();
    moded_holder_base(&mut wrong);
    put_var(&mut wrong, 7, 42); // field 7 as varint (wrong wire) -> skipped, not stored
    let arena2 = Arena::new();
    assert_eq!(ModedHolder::decode(&wrong, &arena2).unwrap().blob(), None);
}

#[test]
fn dropped_field_record_is_still_wire_validated() {
    // old_ids (field 4) is DROPPED, but its records are still wire-validated: a truncated field-4
    // varint fails with a wire error. Mirrors C++ "dropped fields keep decode guarantees".
    let mut buf = Vec::new();
    moded_holder_base(&mut buf);
    put_tag(&mut buf, 4, 0); // field 4 varint tag...
    buf.push(0x80); // ...truncated varint (continuation, no terminator)
    let arena = Arena::new();
    assert!(matches!(
        ModedHolder::decode(&buf, &arena),
        Err(e) if e.kind() == ErrorKind::Truncated
    ));
}

#[test]
fn cross_file_imported_submessages_decode_through_the_forwarder() {
    // Main's fields are imported dep.Dep types -- singular (d), repeated (ds), map value (dm), oneof
    // member (od). They decode through the cross-file decode_into forwarder. Mirrors C++ "imported
    // (cross-file) sub-messages decode through the forwarder".
    let dep = |v: u64| {
        let mut d = Vec::new();
        put_var(&mut d, 1, v); // dep.Dep { v }
        d
    };
    let mut buf = Vec::new();
    put_len(&mut buf, 1, &dep(10)); // d
    put_len(&mut buf, 5, &dep(20)); // ds[0]
    put_len(&mut buf, 5, &dep(21)); // ds[1]
    let mut entry = Vec::new();
    put_var(&mut entry, 1, 7); // dm key (int32)
    put_len(&mut entry, 2, &dep(30)); // dm value (Dep)
    put_len(&mut buf, 6, &entry); // dm[7] = Dep{30}
    put_len(&mut buf, 7, &dep(40)); // choice = od (Dep)
    let arena = Arena::new();
    let m = Main::decode(&buf, &arena).unwrap();
    assert_eq!(m.d().unwrap().v(), Some(10));
    assert_eq!(m.ds().len(), 2);
    assert_eq!(m.ds()[1].v(), Some(21));
    assert_eq!(m.dm().get(&7).unwrap().v(), Some(30));
    match m.choice() {
        Some(MainChoice::Od(d)) => assert_eq!(d.v(), Some(40)),
        _ => panic!("expected the od (Dep) oneof member"),
    }
}

#[test]
fn editions2023_presence_and_delimited_encoding() {
    // ed23 M: file-level IMPLICIT presence (implicit_scalar bare), a per-field EXPLICIT override
    // (explicit_scalar -> Option, present-zero != absent), and a DELIMITED (group-framed) message
    // field (delim). Mirrors C++ "editions 2023 presence features and DELIMITED encoding".
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 9); // implicit_scalar
    put_var(&mut buf, 2, 0); // explicit_scalar present = 0
    put_tag(&mut buf, 6, 3); // delim: start group (DELIMITED message field 6)
    put_var(&mut buf, 1, 5); //   child's implicit_scalar = 5
    put_tag(&mut buf, 6, 4); // delim: end group
    let arena = Arena::new();
    let m = Ed23M::decode(&buf, &arena).unwrap();
    assert_eq!(m.implicit_scalar(), 9);
    assert_eq!(m.explicit_scalar(), Some(0)); // present-and-zero, not None
    assert_eq!(m.delim().unwrap().implicit_scalar(), 5);
    let empty = Arena::new();
    assert_eq!(Ed23M::decode(&[], &empty).unwrap().explicit_scalar(), None);
}

#[test]
fn editions2024_explicit_presence_and_packed_defaults() {
    // ed24 M: edition-2024 defaults -> `a` is EXPLICIT (Option), `b` is PACKED-by-default repeated.
    // Mirrors C++ "editions 2024 defaults (EXPLICIT presence, PACKED repeated)".
    let mut packed = Vec::new();
    for v in [1u64, 2, 3] {
        put_varint(&mut packed, v);
    }
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 7); // a
    put_len(&mut buf, 2, &packed); // b (packed)
    let arena = Arena::new();
    let m = Ed24M::decode(&buf, &arena).unwrap();
    assert_eq!(m.a(), Some(7));
    assert_eq!(m.b(), &[1, 2, 3]);
    let empty = Arena::new();
    assert_eq!(Ed24M::decode(&[], &empty).unwrap().a(), None); // EXPLICIT: absent -> None
}

#[test]
fn messageset_payload_is_skipped_as_unknown_and_ordinary_decodes() {
    // ms Outer: an ordinary field decodes normally; a Container carrying a MessageSet group item is
    // accepted and the group skipped (not materialized, not rejected). A truncated group still fails
    // with a wire error. Mirrors C++ "a MessageSet payload decodes as unknown fields".
    let mut container = Vec::new();
    put_tag(&mut container, 1, 3); // messageset item: start group
    put_var(&mut container, 2, 1234); //   type_id
    put_len(&mut container, 3, b"\x0a\x01s"); //   message payload (opaque)
    put_tag(&mut container, 1, 4); // end group
    let mut buf = Vec::new();
    put_var(&mut buf, 1, 99); // ordinary
    put_len(&mut buf, 2, &container); // container
    let arena = Arena::new();
    let m = MsOuter::decode(&buf, &arena).unwrap();
    assert_eq!(m.ordinary(), Some(99));
    assert!(m.container().is_some()); // decoded; the MessageSet group was skipped

    let mut bad = Vec::new();
    let mut badc = Vec::new();
    put_tag(&mut badc, 1, 3); // start group, never closed
    put_var(&mut badc, 2, 1);
    put_len(&mut bad, 2, &badc);
    let arena2 = Arena::new();
    assert!(MsOuter::decode(&bad, &arena2).is_err());
}

#[test]
fn same_typed_oneof_members_route_by_tag() {
    // Collide.letters: same-typed members a(7) / A(8) route to distinct Letters variants by tag
    // (A for `a`, A_ for the colliding `A`). Mirrors C++ "oneof routes same-typed members by tag".
    let mut buf = Vec::new();
    put_var(&mut buf, 7, 11); // a
    let arena = Arena::new();
    assert!(matches!(
        Collide::decode(&buf, &arena).unwrap().letters(),
        Some(Letters::A(11))
    ));
    let mut buf2 = Vec::new();
    put_var(&mut buf2, 8, 22); // A
    let arena2 = Arena::new();
    assert!(matches!(
        Collide::decode(&buf2, &arena2).unwrap().letters(),
        Some(Letters::A_(22))
    ));
}

#[test]
fn inlined_wrapper_tracks_its_own_unknown_fields_separately_from_the_parent() {
    // Holder (unknown-tracked) has an inlined Flag wrapper (field 1, itself unknown-tracked) + n
    // (field 2). An unknown field INSIDE the Flag trips the Flag's mask, not the Holder's; an unknown
    // field at the Holder level trips the Holder's, not the Flag's. Mirrors C++ "a bool-wrapper field
    // reports has_unknown_fields (--unknown-present)".
    let mut flag_body = Vec::new();
    put_var(&mut flag_body, 1, 1); // Flag.value = true
    put_var(&mut flag_body, 5, 7); // an unknown field INSIDE Flag
    let mut buf = Vec::new();
    put_len(&mut buf, 1, &flag_body); // flag
    put_var(&mut buf, 2, 3); // n
    let arena = Arena::new();
    let h = UnkHolder::decode(&buf, &arena).unwrap();
    assert_eq!(h.n(), 3);
    let f = h.flag().unwrap();
    assert!(f.value());
    assert!(f.has_unknown_fields()); // the Flag saw the unknown
    assert!(!h.has_unknown_fields()); // ...the Holder did not

    let mut flag2 = Vec::new();
    put_var(&mut flag2, 1, 1);
    let mut buf2 = Vec::new();
    put_len(&mut buf2, 1, &flag2);
    put_var(&mut buf2, 9, 1); // an unknown field at the HOLDER level
    let arena2 = Arena::new();
    let h2 = UnkHolder::decode(&buf2, &arena2).unwrap();
    assert!(h2.has_unknown_fields()); // the Holder saw the unknown
    assert!(!h2.flag().unwrap().has_unknown_fields()); // ...the Flag did not

    // A field at the wrapper's OWN known number (1 = value) but the WRONG wire type is skipped as a
    // known field, NOT counted as unknown (the mask keys on field number, not the full tag).
    let mut flag3 = Vec::new();
    put_len(&mut flag3, 1, b"x"); // Flag.value (field 1) as LEN -- wrong wire (value is a bool/varint)
    let mut buf3 = Vec::new();
    put_len(&mut buf3, 1, &flag3);
    let arena3 = Arena::new();
    assert!(!UnkHolder::decode(&buf3, &arena3)
        .unwrap()
        .flag()
        .unwrap()
        .has_unknown_fields());
}

#[test]
fn unknown_present_observes_messageset_carried_content() {
    // Under --unknown-present, Container::has_unknown_fields() is the only way to observe that a
    // MessageSet carried content (skipped, not materialized); false when empty. Mirrors C++
    // "--unknown-present observes that a MessageSet carried content".
    let mut c = Vec::new();
    put_tag(&mut c, 1, 3); // messageset item group
    put_var(&mut c, 2, 1234);
    put_len(&mut c, 3, b"\x0a\x01s");
    put_tag(&mut c, 1, 4);
    let mut buf = Vec::new();
    put_len(&mut buf, 2, &c); // container
    let arena = Arena::new();
    assert!(MsuOuter::decode(&buf, &arena)
        .unwrap()
        .container()
        .unwrap()
        .has_unknown_fields()); // content observed

    let mut empty = Vec::new();
    put_len(&mut empty, 2, &[]); // empty container
    let arena2 = Arena::new();
    assert!(!MsuOuter::decode(&empty, &arena2)
        .unwrap()
        .container()
        .unwrap()
        .has_unknown_fields());
}

#[test]
fn packed_fixed_span_with_a_partial_element_is_rejected() {
    // codes (field 13, packed fixed32 = 4 bytes/elt): a span whose length is not a whole multiple of
    // the element width is malformed -> decode errors (never silently truncates or panics).
    let mut buf = Vec::new();
    put_len(&mut buf, 13, &[1, 2, 3, 4, 5, 6]); // 6 bytes: one whole fixed32 + a 2-byte remainder
    let arena = Arena::new();
    assert!(Msg::decode(&buf, &arena).is_err());
}

#[test]
fn a_stray_end_group_tag_is_rejected_by_the_decode_loop() {
    // An end-group tag (wire type 4) with no matching start, seen as a top-level field, is malformed
    // -> the decode loop's skip rejects it with a wire error, never panicking.
    let mut buf = Vec::new();
    put_tag(&mut buf, 1, 4); // stray EGROUP for field 1
    let arena = Arena::new();
    assert!(Msg::decode(&buf, &arena).is_err());
}
