//! The google-upb capstone arm's decoders + checksum walks, kept in this isolated crate so Google's
//! upb-backed `protobuf` v4 owns `::protobuf::` here without clashing with stepancheg's v3 in
//! `rapidproto-bench` (see Cargo.toml). This is the only true arena-vs-arena peer to RapidProto: a
//! decoded message is a handle onto a upb arena, and every field read returns a borrowed `*View<'_>`
//! over it -- exactly our arena-of-borrowed-views model, which is what makes it the capstone.
//!
//! Each `pub fn <scenario>(bytes) -> u64` decodes with `Message::parse` and walks every field with the
//! SAME convention as [`rapidproto_bench::checksum`] (string/bytes -> byte length; repeated string /
//! repeated message / map -> element COUNT; bool -> 0/1; double -> `to_bits`; enum -> raw i32;
//! present-fields-only), so this arm agrees with every other arm or the shared gate aborts. Reads go
//! through `.as_view()` to exercise the borrowed-view path a upb consumer actually uses. Enums are open
//! (`i32::from(Kind)` preserves unknowns); packed doubles are copied into the upb arena, so -- unlike
//! quick-protobuf -- there is no unaligned-slice UB and no `‡`.

use protobuf::prelude::*;

// Google's generated upb decoders for `bench.proto` (rendered by `build.rs`). The emitted
// `generated.rs` declares `pub mod bench_proto; pub use bench_proto::*;`; nesting it in a private
// module both scopes `clippy::all` to the generated code (it trips many style lints) and re-exports
// the message types into this crate for the walks below.
#[allow(clippy::all)]
mod generated {
    include!(concat!(env!("OUT_DIR"), "/generated.rs"));
}
use generated::*;

pub fn dataset(bytes: &[u8]) -> u64 {
    let m = Dataset::parse(bytes).expect("google-upb decode");
    let v = m.as_view();
    let mut sum = v.name().as_bytes().len() as u64 ^ (v.version() as u64);
    for p in v.people().iter() {
        sum = sum
            .wrapping_add(p.id() as u64)
            .wrapping_add(p.name().as_bytes().len() as u64)
            .wrapping_add(p.email().as_bytes().len() as u64)
            .wrapping_add(u64::from(p.active()))
            .wrapping_add(p.score().to_bits())
            .wrapping_add(p.created())
            .wrapping_add(p.tags().len() as u64)
            .wrapping_add(p.attributes().len() as u64)
            .wrapping_add(p.counters().len() as u64);
        if let Some(a) = p.address_opt() {
            sum = sum.wrapping_add(u64::from(a.zip()));
        }
        for h in p.history().iter() {
            sum = sum.wrapping_add(h as u64);
        }
    }
    sum
}

pub fn bigset(bytes: &[u8]) -> u64 {
    let m = BigSet::parse(bytes).expect("google-upb decode");
    let v = m.as_view();
    let mut sum = 0u64;
    for it in v.items().iter() {
        for n in it.numbers().iter() {
            sum = sum.wrapping_add(n as u64);
        }
        for r in it.reals().iter() {
            sum = sum.wrapping_add(r.to_bits());
        }
        for z in it.zz().iter() {
            sum = sum.wrapping_add(z as u64);
        }
        for k in it.kinds().iter() {
            sum = sum.wrapping_add(i32::from(k) as u64);
        }
    }
    sum
}

pub fn wideset(bytes: &[u8]) -> u64 {
    let m = WideSet::parse(bytes).expect("google-upb decode");
    let v = m.as_view();
    let mut sum = 0u64;
    for it in v.items().iter() {
        for x in it.a().iter() {
            sum = sum.wrapping_add(x as u64);
        }
        for x in it.b().iter() {
            sum = sum.wrapping_add(x as u64);
        }
        for x in it.c().iter() {
            sum = sum.wrapping_add(x as u64);
        }
        for x in it.d().iter() {
            sum = sum.wrapping_add(x as u64);
        }
        for x in it.s().iter() {
            sum = sum.wrapping_add(x.as_bytes().len() as u64);
        }
        for x in it.t().iter() {
            sum = sum.wrapping_add(x.as_bytes().len() as u64);
        }
    }
    sum
}

pub fn recordset(bytes: &[u8]) -> u64 {
    let m = RecordSet::parse(bytes).expect("google-upb decode");
    let v = m.as_view();
    let mut sum = 0u64;
    for r in v.records().iter() {
        sum = sum
            .wrapping_add(r.f1() as u64)
            .wrapping_add(r.f2() as u64)
            .wrapping_add(r.f3() as u64)
            .wrapping_add(r.f4() as u64)
            .wrapping_add(u64::from(r.f5()))
            .wrapping_add(r.f6() as u64)
            .wrapping_add(u64::from(r.f7()))
            .wrapping_add(r.f8())
            .wrapping_add(r.f9() as u64)
            .wrapping_add(r.f10() as u64)
            .wrapping_add(r.f11() as u64)
            .wrapping_add(r.f12() as u64)
            .wrapping_add(r.f13() as u64)
            .wrapping_add(r.f14() as u64)
            .wrapping_add(r.f15() as u64)
            .wrapping_add(r.f18() as u64)
            .wrapping_add(r.f19() as u64)
            .wrapping_add(r.f20() as u64);
        for s in [r.s1_opt(), r.s2_opt()].into_iter().flatten() {
            sum = sum
                .wrapping_add(s.a() as u64)
                .wrapping_add(s.b() as u64)
                .wrapping_add(s.c() as u64)
                .wrapping_add(s.d() as u64)
                .wrapping_add(u64::from(s.e()))
                .wrapping_add(u64::from(s.f()));
        }
    }
    sum
}

pub fn particleset(bytes: &[u8]) -> u64 {
    let m = ParticleSet::parse(bytes).expect("google-upb decode");
    let v = m.as_view();
    let mut sum = 0u64;
    for p in v.items().iter() {
        sum = sum
            .wrapping_add(p.id() as u64)
            .wrapping_add(u64::from(p.color()));
        if let Some(pos) = p.position_opt() {
            sum = sum
                .wrapping_add(pos.x().to_bits())
                .wrapping_add(pos.y().to_bits())
                .wrapping_add(pos.z().to_bits());
        }
        if let Some(vel) = p.velocity_opt() {
            sum = sum
                .wrapping_add(vel.x().to_bits())
                .wrapping_add(vel.y().to_bits())
                .wrapping_add(vel.z().to_bits());
        }
    }
    sum
}
