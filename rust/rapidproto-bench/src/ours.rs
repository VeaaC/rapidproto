//! RapidProto's own arms (reused from the golden crate, so the comparison runs the exact code a
//! consumer compiles): the arena decoder cold -- arm 0, the baseline: a fresh arena per decode -- and
//! warm -- one arena, reset per decode (the C++ `arena-warm` arm) -- each walking every field with the
//! shared [`checksum`](crate::checksum); plus, on `dataset`, the streaming decoder (the C++ arena bench's
//! `streamgen` arm), visiting the same fields into the same sum.

use std::cell::RefCell;

use rapidproto::Arena;
use rustgen_golden::bench::arena::bench::{Big, BigSet, Dataset, ParticleSet, RecordSet, WideSet};

use crate::{checksum, Arm};

// The cold + warm arms for one message type. The warm arm's arena lives in the closure (a `RefCell`,
// since an arm is a `Fn`) and is reset before every decode.
macro_rules! arena_arms {
    ($ty:ty, $ck:path) => {{
        let warm = RefCell::new(Arena::new());
        vec![
            Arm::new("rapidproto", |b| {
                let arena = Arena::new();
                $ck(<$ty>::decode(b, &arena).expect("rapidproto decode"))
            }),
            Arm::new("rapidproto-warm", move |b| {
                let mut arena = warm.borrow_mut();
                arena.reset();
                $ck(<$ty>::decode(b, &arena).expect("rapidproto decode"))
            }),
        ]
    }};
}

pub fn dataset() -> Vec<Arm> {
    let mut arms = arena_arms!(Dataset, checksum::checksum_dataset);
    arms.push(Arm::new("rapidproto-stream", stream_dataset));
    arms
}

pub fn bigset() -> Vec<Arm> {
    arena_arms!(BigSet, checksum::checksum_bigset)
}

pub fn big() -> Vec<Arm> {
    arena_arms!(Big, checksum::checksum_big)
}

pub fn wideset() -> Vec<Arm> {
    arena_arms!(WideSet, checksum::checksum_wideset)
}

pub fn recordset() -> Vec<Arm> {
    arena_arms!(RecordSet, checksum::checksum_recordset)
}

pub fn particleset() -> Vec<Arm> {
    arena_arms!(ParticleSet, checksum::checksum_particleset)
}

// `checksum_dataset` over the streaming decoder, visiting every field the C++ arena bench's
// `streamgen` arm does (each Address and Attribute decoded whole): the people's terms accumulate in
// one sum and the header's `name length ^ version` is combined at the end (wrapping addition
// commutes, so the order the fields arrive in does not matter). A repeated field or map contributes
// its element count; the strings that convention does not count (street, city, an attribute's key
// and value) are still delivered and summed apart, into `unread`, so the arm does the C++ work.
fn stream_dataset(b: &[u8]) -> u64 {
    use rustgen_golden::bench::stream::bench::*;
    #[derive(Default)]
    struct Acc {
        name_len: u64,
        version: u64,
        people: u64,
        unread: u64,
    }
    let add = |s: &mut u64, v: u64| *s = s.wrapping_add(v);
    let mut acc = Acc::default();
    Dataset::new(b)
        .decode(dataset_stream!(&mut acc; {
            name => |a, v| a.name_len = v.len() as u64,
            version => |a, v| a.version = v as u64,
            people => |a, p| p.decode(person_stream!(&mut *a; {
                id => |a, v| add(&mut a.people, v as u64),
                name => |a, v| add(&mut a.people, v.len() as u64),
                email => |a, v| add(&mut a.people, v.len() as u64),
                active => |a, v| add(&mut a.people, u64::from(v)),
                score => |a, v| add(&mut a.people, v.to_bits()),
                created => |a, v| add(&mut a.people, v),
                address => |a, ad| ad.decode(address_stream!(&mut *a; {
                    street => |a, v| add(&mut a.unread, v.len() as u64),
                    city => |a, v| add(&mut a.unread, v.len() as u64),
                    zip => |a, v| add(&mut a.people, u64::from(v)),
                })),
                tags => |a, _v| add(&mut a.people, 1),
                history => |a, v| add(&mut a.people, v as u64),
                attributes => |a, at| {
                    add(&mut a.people, 1);
                    at.decode(attribute_stream!(&mut *a; {
                        key => |a, v| add(&mut a.unread, v.len() as u64),
                        value => |a, v| add(&mut a.unread, v.len() as u64),
                    }))
                },
                counters => |a, _e| add(&mut a.people, 1),
            })),
        }))
        .expect("rapidproto stream decode");
    std::hint::black_box(acc.unread);
    (acc.name_len ^ acc.version).wrapping_add(acc.people)
}
