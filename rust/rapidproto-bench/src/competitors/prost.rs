//! prost arm. prost materializes the message into owned `Vec`/`String`/`HashMap` (a different model
//! from our borrowed arena views) and validates UTF-8 on strings; the checksum walk reads every field
//! so that owned allocation is paid for, not elided. `decode` is prost's only (and fastest) entry --
//! there is no borrowed/zero-copy mode. The walks are field-for-field identical to `ours`, so the
//! arms agree.

use ::prost::Message;

use crate::Arm;

/// prost's generated decoders for `bench.proto` (rendered by `build.rs`; package `bench` -> top-level
/// items here).
pub mod pb {
    include!(concat!(env!("OUT_DIR"), "/bench.rs"));
}

pub fn dataset() -> Arm {
    Arm::new("prost", |b| {
        checksum_dataset(&pb::Dataset::decode(b).expect("prost decode"))
    })
}

pub fn bigset() -> Arm {
    Arm::new("prost", |b| {
        checksum_bigset(&pb::BigSet::decode(b).expect("prost decode"))
    })
}

pub fn big() -> Arm {
    Arm::new("prost", |b| {
        checksum_big(&pb::Big::decode(b).expect("prost decode"))
    })
}

pub fn wideset() -> Arm {
    Arm::new("prost", |b| {
        checksum_wideset(&pb::WideSet::decode(b).expect("prost decode"))
    })
}

pub fn recordset() -> Arm {
    Arm::new("prost", |b| {
        checksum_recordset(&pb::RecordSet::decode(b).expect("prost decode"))
    })
}

pub fn particleset() -> Arm {
    Arm::new("prost", |b| {
        checksum_particleset(&pb::ParticleSet::decode(b).expect("prost decode"))
    })
}

fn checksum_dataset(ds: &pb::Dataset) -> u64 {
    let mut sum = ds.name.len() as u64 ^ (ds.version as u64);
    for p in &ds.people {
        sum = sum
            .wrapping_add(p.id as u64)
            .wrapping_add(p.name.len() as u64)
            .wrapping_add(p.email.len() as u64)
            .wrapping_add(u64::from(p.active))
            .wrapping_add(p.score.to_bits())
            .wrapping_add(p.created)
            .wrapping_add(p.tags.len() as u64)
            .wrapping_add(p.attributes.len() as u64)
            .wrapping_add(p.counters.len() as u64);
        if let Some(a) = &p.address {
            sum = sum.wrapping_add(u64::from(a.zip));
        }
        for &h in &p.history {
            sum = sum.wrapping_add(h as u64);
        }
    }
    sum
}

fn checksum_bigset(bs: &pb::BigSet) -> u64 {
    bs.items
        .iter()
        .fold(0u64, |sum, it| sum.wrapping_add(checksum_big(it)))
}

fn checksum_big(it: &pb::Big) -> u64 {
    let mut sum = 0u64;
    for &n in &it.numbers {
        sum = sum.wrapping_add(n as u64);
    }
    for &r in &it.reals {
        sum = sum.wrapping_add(r.to_bits());
    }
    for &z in &it.zz {
        sum = sum.wrapping_add(z as u64);
    }
    for &k in &it.kinds {
        sum = sum.wrapping_add(k as u64);
    }
    sum
}

fn checksum_wideset(ws: &pb::WideSet) -> u64 {
    let mut sum = 0u64;
    for it in &ws.items {
        for &v in &it.a {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in &it.b {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in &it.c {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in &it.d {
            sum = sum.wrapping_add(v as u64);
        }
        for x in &it.s {
            sum = sum.wrapping_add(x.len() as u64);
        }
        for x in &it.t {
            sum = sum.wrapping_add(x.len() as u64);
        }
    }
    sum
}

fn checksum_recordset(rs: &pb::RecordSet) -> u64 {
    let mut sum = 0u64;
    for r in &rs.records {
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
    for p in &ps.items {
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
