//! quick-protobuf arm. The first NON-owned competitor: generated types borrow strings/bytes as
//! `Cow<'a, _>` over the input and inline singular sub-messages as `Option<T>` (no `Box`, unlike
//! rust-protobuf), and `reals` even borrows as `Cow<'a, [f64]>` (zero-copy packed doubles). But it
//! still materializes a non-owning object tree: every repeated field is a heap `Vec`, and `Cow<str>`
//! still UTF-8-validates (borrows, doesn't copy). So "borrowed-string, allocating-tree" -- closer to
//! our arena than the owned crates, but not a no-allocation peer. The checksum walk reads every field
//! so the tree build is paid for, not elided.
//!
//! Decode entry is `M::from_reader(&mut BytesReader, bytes)` -- the top-level (non-length-delimited)
//! read; `deserialize_from_slice` is NOT used, it reads a length prefix our bare payloads don't have.
//!
//! SOUNDNESS (UB): the zero-copy win on packed fixed-width floats is unsound. For `Big.reals`
//! (`repeated double`) pb-rs emits a `Cow<'a, [f64]>` filled by `BytesReader::read_packed_fixed`,
//! which does `slice::from_raw_parts(input_ptr as *const f64, n)` with NO alignment handling -- a
//! misaligned `&[f64]` into the align-1 input. It is correct on x86 in a release build (the alignment
//! UB-check is compiled out) but genuine UB: it aborts under debug UB-checks / Miri and would misread
//! on a big-endian host. (Our arena decoder instead memcpy's packed fixed data into an aligned arena
//! array -- sound, and the per-field `reals` checksum still agrees on x86.) So this arm is measured in
//! RELEASE ONLY: the runner drops it from the packed-double scenarios in a debug build and flags its
//! rows there in release (`has_unaligned_fixed_slice_ub`), and the `bigset` agreement test is
//! release-only. Only packed fixed-width floats hit this; strings (`Cow<str>`, align 1) and
//! varint-packed ints (`Vec`, decoded) are fine.
//!
//! FEATURE GAP: pb-rs generates CLOSED enums -- `From<i32>` maps any unknown value to the default
//! (`_ => Self::default()`), so an out-of-range proto3 open-enum value is silently lost. The shared
//! payloads only carry defined `Kind` values (0/1), so the arms agree; see the agreement test for the
//! one place this surfaces.

use quick_protobuf::{BytesReader, MessageRead};

use crate::Arm;

/// quick-protobuf's generated decoders for `bench.proto` (rendered by `build.rs` via pb-rs' pure
/// parser). Its codegen emits a `mod.rs` (`pub mod bench;`); as with the rust-protobuf arm we include
/// that attribute-free file and flatten `bench` up to `pb::` to match the other arms' paths.
pub mod pb {
    include!(concat!(env!("OUT_DIR"), "/quick_protobuf_gen/mod.rs"));
    pub use self::bench::*;
}

// Top-level decode: wrap the bytes in a reader and read the message to EOF (no length prefix).
fn decode<'a, M: MessageRead<'a>>(bytes: &'a [u8]) -> M {
    let mut reader = BytesReader::from_bytes(bytes);
    M::from_reader(&mut reader, bytes).expect("quick-protobuf decode")
}

pub fn dataset() -> Arm {
    Arm::new("quick-protobuf", |b| {
        checksum_dataset(&decode::<pb::Dataset>(b))
    })
}

pub fn bigset() -> Arm {
    Arm::new("quick-protobuf", |b| {
        checksum_bigset(&decode::<pb::BigSet>(b))
    })
}

pub fn big() -> Arm {
    Arm::new("quick-protobuf", |b| checksum_big(&decode::<pb::Big>(b)))
}

pub fn wideset() -> Arm {
    Arm::new("quick-protobuf", |b| {
        checksum_wideset(&decode::<pb::WideSet>(b))
    })
}

pub fn recordset() -> Arm {
    Arm::new("quick-protobuf", |b| {
        checksum_recordset(&decode::<pb::RecordSet>(b))
    })
}

pub fn particleset() -> Arm {
    Arm::new("quick-protobuf", |b| {
        checksum_particleset(&decode::<pb::ParticleSet>(b))
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
    for &r in it.reals.iter() {
        sum = sum.wrapping_add(r.to_bits());
    }
    for &z in &it.zz {
        sum = sum.wrapping_add(z as u64);
    }
    for &k in &it.kinds {
        sum = sum.wrapping_add(k as i32 as u64);
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
