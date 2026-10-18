//! The one canonical per-scenario checksum walk over RapidProto's generated `bench` decoders, shared
//! by every "ours" measurement: the ecosystem arm (`ours`), the criterion A/B bench (`benches/
//! decode.rs`), and the perf-stat harnesses. A full field read (wrapping `u64` sum) that defeats
//! dead-code elimination and, in the ecosystem bench, is mirrored field-for-field by each competitor
//! arm so all arms must agree. See the crate docs for the per-type contribution convention.

use rustgen_golden::bench::arena::bench::{BigSet, Dataset, ParticleSet, RecordSet, WideSet};

pub fn checksum_dataset(ds: &Dataset) -> u64 {
    let mut sum = ds.name().as_bytes().len() as u64 ^ (ds.version() as u64);
    for p in ds.people() {
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
        if let Some(a) = p.address() {
            sum = sum.wrapping_add(u64::from(a.zip()));
        }
        for &h in p.history() {
            sum = sum.wrapping_add(h as u64);
        }
    }
    sum
}

pub fn checksum_bigset(bs: &BigSet) -> u64 {
    let mut sum = 0u64;
    for it in bs.items() {
        for &n in it.numbers() {
            sum = sum.wrapping_add(n as u64);
        }
        for &r in it.reals() {
            sum = sum.wrapping_add(r.to_bits());
        }
        for &z in it.zz() {
            sum = sum.wrapping_add(z as u64);
        }
        for &k in it.kinds() {
            sum = sum.wrapping_add(k.0 as u64);
        }
    }
    sum
}

pub fn checksum_wideset(ws: &WideSet) -> u64 {
    let mut sum = 0u64;
    for it in ws.items() {
        for &v in it.a() {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in it.b() {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in it.c() {
            sum = sum.wrapping_add(v as u64);
        }
        for &v in it.d() {
            sum = sum.wrapping_add(v as u64);
        }
        for x in it.s() {
            sum = sum.wrapping_add(x.len() as u64);
        }
        for x in it.t() {
            sum = sum.wrapping_add(x.len() as u64);
        }
    }
    sum
}

pub fn checksum_recordset(rs: &RecordSet) -> u64 {
    let mut sum = 0u64;
    for r in rs.records() {
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
        for s in [r.s1(), r.s2()].into_iter().flatten() {
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

pub fn checksum_particleset(ps: &ParticleSet) -> u64 {
    let mut sum = 0u64;
    for p in ps.items() {
        sum = sum
            .wrapping_add(p.id() as u64)
            .wrapping_add(u64::from(p.color()));
        if let Some(v) = p.position() {
            sum = sum
                .wrapping_add(v.x().to_bits())
                .wrapping_add(v.y().to_bits())
                .wrapping_add(v.z().to_bits());
        }
        if let Some(v) = p.velocity() {
            sum = sum
                .wrapping_add(v.x().to_bits())
                .wrapping_add(v.y().to_bits())
                .wrapping_add(v.z().to_bits());
        }
    }
    sum
}
