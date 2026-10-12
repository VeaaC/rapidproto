//! Compile check for the Rust generator's emitted output (see this crate's Cargo.toml).
//!
//! Each golden is generated code, not hand-written, so it isn't linted — the point is only that it
//! is valid, compilable Rust, including the `super::`-relative paths from `arena` into `common`.
//! Each golden is included under its own module so those two trees are siblings, matching how a
//! consumer includes a schema's generated files under one parent module.
//!
//! INTERIM: this is a small, hand-picked set of schemas while the emitter is still being built out.
//! Once the emitter covers the full feature surface, replace it with the comprehensive golden set
//! (per-schema goldens over the shared schema corpus) and retire this stopgap crate.
#![cfg_attr(not(feature = "std"), no_std)]
#![allow(clippy::all)]
// Generated code: `unused` covers dead_code and the unused params of the stubbed `decode` (they are
// used once decode is implemented).
#![allow(unused)]

pub mod scalars_enums {
    include!("../goldens/scalars_enums.rs");
}

pub mod nopackage {
    include!("../goldens/nopackage.rs");
}
