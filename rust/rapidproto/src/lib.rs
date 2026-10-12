//! RapidProto — decode-only Protobuf runtime for generated Rust decoders.
//!
//! Generated decoders borrow from the input bytes and from an [`Arena`], both tied to one
//! lifetime, and never copy. This crate provides the pieces they build on:
//!
//! - [`Arena`] — a bump arena that serves three tiers from one API: `std` (default), `no_std` +
//!   `alloc`, and `no_std` with no allocator (a caller-seeded fixed buffer). Allocation is
//!   fallible and never panics.
//! - [`ProtoStr`] — a borrowed `string` field; the caller chooses whether/how to validate UTF-8.
//! - [`MapView`] — a borrowed view over a `map` field's entries.
//! - [`DecodeError`] / [`ErrorKind`] — the error returned by generated `decode` entry points.
//!
//! This is an early stage: the wire-decode primitives and the generated code that uses these
//! types are still being built out.
//!
//! `no_std` outside of tests whenever the `std` feature is off; the test harness keeps `std` so
//! the no-allocator path can still be unit-tested with `--no-default-features`.
#![cfg_attr(not(any(feature = "std", test)), no_std)]
#![forbid(unsafe_op_in_unsafe_fn)]

#[cfg(feature = "alloc")]
extern crate alloc;

mod arena;
mod error;
mod map;
mod proto_str;

pub use arena::{Arena, ArenaExhausted};
pub use error::{DecodeError, ErrorKind};
pub use map::MapView;
pub use proto_str::ProtoStr;
