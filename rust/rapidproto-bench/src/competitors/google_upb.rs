//! google-upb arm (the CAPSTONE) -- Google's official upb-backed `protobuf` crate (v4), the only true
//! arena-vs-arena peer to RapidProto: a decoded message is a handle onto a upb C arena and every field
//! read returns a borrowed `*View<'_>` over it, exactly our arena-of-borrowed-views model. The decode
//! and field walk live in the sibling `rapidproto-bench-upb` crate, which owns `::protobuf::` = v4 in
//! isolation (see that crate's `Cargo.toml`); here we only wrap each scenario's walk in an [`Arm`].
//! The walks mirror the shared checksum convention, so this arm joins the agreement gate like any other.
//!
//! Gated behind the off-by-default `google-upb` feature: building it pulls Google's v4 crate (upb C
//! kernel, compiled by `cc`) and requires an exact-version `protoc` (36.2) at build time, so it never
//! touches the normal cargo-only build/CI. Supports `map` (so it covers `dataset`, unlike femtopb),
//! open enums (unknowns preserved), and sound packed doubles (copied into the upb arena -- no `‡`).

use crate::Arm;

pub fn dataset() -> Arm {
    Arm::new("google-upb", rapidproto_bench_upb::dataset)
}

pub fn bigset() -> Arm {
    Arm::new("google-upb", rapidproto_bench_upb::bigset)
}

pub fn wideset() -> Arm {
    Arm::new("google-upb", rapidproto_bench_upb::wideset)
}

pub fn recordset() -> Arm {
    Arm::new("google-upb", rapidproto_bench_upb::recordset)
}

pub fn particleset() -> Arm {
    Arm::new("google-upb", rapidproto_bench_upb::particleset)
}
