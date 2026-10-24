//! femtopb arm -- the no-alloc BORROWED peer, the nearest ecosystem analog to our arena-of-borrowed-
//! views. Generated types borrow strings as `&'a str`, inline singular sub-messages as `Option<T>`,
//! and decode repeated/packed fields LAZILY: `Repeated<'a, T, _>` / `Packed<'a, T, _>` are iterators
//! that decode each element on demand (no backing `Vec`), yielding `Result<T>`. The checksum walk
//! reads every element, so that lazy work is forced, not skipped.
//!
//! Unlike quick-protobuf, femtopb's packed fixed-width path is SOUND: `Packed<f64>` decodes each
//! `double` per element (no `slice::from_raw_parts` over the unaligned input), so there is no
//! misaligned-slice UB -- this arm runs and is tested in debug like the others, and carries no `‡`.
//! Enums are open: `EnumValue<Kind>::to_raw()` returns the raw i32 for both known and unknown values,
//! so no open-enum value is lost.
//!
//! FEATURE GAP: femtopb does NOT support proto3 `map` fields -- its codegen emits a `compile_error!`.
//! The only map in the schema is `Person.counters`, so `build.rs` strips that field before running
//! femtopb-build (so the rest of the schema compiles), and the femtopb arm reports `dataset` (the only
//! scenario using `Person`) as UNSUPPORTED -- `dataset()` returns `Arm::unsupported`, so the gap is
//! listed and flagged in the table rather than silently skipped. The other seven scenarios have no
//! maps and are decoded normally.
//!
//! Decode entry is `M::decode(bytes)` (the top-level, non-length-delimited read).

use ::femtopb::Message;

use crate::Arm;

/// femtopb's generated decoders for `bench.proto` (rendered by `build.rs` via femtopb-build's pure
/// parser; a flat prost-style module -> items at the top level, included directly). `dead_code` is
/// allowed because the `dataset`-family types (`Dataset`/`Person`/`Address`/`Attribute`) are generated
/// but unused -- the femtopb arm skips the `dataset` scenario (its `map` field is unsupported).
#[allow(dead_code)]
pub mod pb {
    include!(concat!(env!("OUT_DIR"), "/femtopb_gen/bench.rs"));
}

fn decode<'a, M: Message<'a>>(bytes: &'a [u8]) -> M {
    M::decode(bytes).expect("femtopb decode")
}

// femtopb cannot decode `dataset`: `Person` has a `map` field (`counters`) and femtopb rejects maps,
// so this arm is UNSUPPORTED for that scenario -- listed and flagged in the table, not dropped.
pub fn dataset() -> Arm {
    Arm::unsupported("femtopb", "no map<> support")
}

pub fn bigset() -> Arm {
    Arm::new("femtopb", |b| checksum_bigset(&decode::<pb::BigSet>(b)))
}

pub fn big() -> Arm {
    Arm::new("femtopb", |b| checksum_big(&decode::<pb::Big>(b)))
}

pub fn wideset() -> Arm {
    Arm::new("femtopb", |b| checksum_wideset(&decode::<pb::WideSet>(b)))
}

pub fn recordset() -> Arm {
    Arm::new("femtopb", |b| {
        checksum_recordset(&decode::<pb::RecordSet>(b))
    })
}

pub fn particleset() -> Arm {
    Arm::new("femtopb", |b| {
        checksum_particleset(&decode::<pb::ParticleSet>(b))
    })
}

// Each lazy element is a `Result` (decode can fail mid-walk) and is reached via `.iter()` (only the
// owning `Packed`/`Repeated` and `.iter()` are iterable, not `&field`). The shared payloads are
// well-formed, so `expect` is the agreement-gate contract -- a decode error would be a real mismatch,
// not silently dropped.
fn checksum_bigset(bs: &pb::BigSet) -> u64 {
    bs.items.iter().fold(0u64, |sum, it| {
        sum.wrapping_add(checksum_big(&it.expect("femtopb big")))
    })
}

fn checksum_big(it: &pb::Big) -> u64 {
    let mut sum = 0u64;
    for n in it.numbers.iter() {
        sum = sum.wrapping_add(n.expect("femtopb number") as u64);
    }
    for r in it.reals.iter() {
        sum = sum.wrapping_add(r.expect("femtopb real").to_bits());
    }
    for z in it.zz.iter() {
        sum = sum.wrapping_add(z.expect("femtopb zz") as u64);
    }
    for k in it.kinds.iter() {
        sum = sum.wrapping_add(k.expect("femtopb kind").to_raw() as u64);
    }
    sum
}

fn checksum_wideset(ws: &pb::WideSet) -> u64 {
    let mut sum = 0u64;
    for it in ws.items.iter() {
        let it = it.expect("femtopb wide");
        for v in it.a.iter() {
            sum = sum.wrapping_add(v.expect("femtopb a") as u64);
        }
        for v in it.b.iter() {
            sum = sum.wrapping_add(v.expect("femtopb b") as u64);
        }
        for v in it.c.iter() {
            sum = sum.wrapping_add(v.expect("femtopb c") as u64);
        }
        for v in it.d.iter() {
            sum = sum.wrapping_add(v.expect("femtopb d") as u64);
        }
        for x in it.s.iter() {
            sum = sum.wrapping_add(x.expect("femtopb s").len() as u64);
        }
        for x in it.t.iter() {
            sum = sum.wrapping_add(x.expect("femtopb t").len() as u64);
        }
    }
    sum
}

fn checksum_recordset(rs: &pb::RecordSet) -> u64 {
    let mut sum = 0u64;
    for r in rs.records.iter() {
        let r = r.expect("femtopb record");
        sum = sum
            .wrapping_add(r.f1 as u64)
            .wrapping_add(r.f2 as u64)
            .wrapping_add(r.f3 as u64)
            .wrapping_add(r.f4 as u64)
            .wrapping_add(u64::from(r.f5))
            .wrapping_add(r.f6 as u64)
            .wrapping_add(u64::from(r.f7))
            .wrapping_add(r.f8)
            .wrapping_add(r.f9 as u64)
            .wrapping_add(r.f10 as u64)
            .wrapping_add(r.f11 as u64)
            .wrapping_add(r.f12 as u64)
            .wrapping_add(r.f13 as u64)
            .wrapping_add(r.f14 as u64)
            .wrapping_add(r.f15 as u64)
            .wrapping_add(r.f18 as u64)
            .wrapping_add(r.f19 as u64)
            .wrapping_add(r.f20 as u64);
        for s in [r.s1.as_ref(), r.s2.as_ref()].into_iter().flatten() {
            sum = sum
                .wrapping_add(s.a as u64)
                .wrapping_add(s.b as u64)
                .wrapping_add(s.c as u64)
                .wrapping_add(s.d as u64)
                .wrapping_add(u64::from(s.e))
                .wrapping_add(u64::from(s.f));
        }
    }
    sum
}

fn checksum_particleset(ps: &pb::ParticleSet) -> u64 {
    let mut sum = 0u64;
    for p in ps.items.iter() {
        let p = p.expect("femtopb particle");
        sum = sum
            .wrapping_add(p.id as u64)
            .wrapping_add(u64::from(p.color));
        if let Some(v) = &p.position {
            sum = sum
                .wrapping_add(v.x.to_bits())
                .wrapping_add(v.y.to_bits())
                .wrapping_add(v.z.to_bits());
        }
        if let Some(v) = &p.velocity {
            sum = sum
                .wrapping_add(v.x.to_bits())
                .wrapping_add(v.y.to_bits())
                .wrapping_add(v.z.to_bits());
        }
    }
    sum
}
