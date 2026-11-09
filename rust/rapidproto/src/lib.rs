//! RapidProto — decode-only Protobuf runtime for generated Rust decoders.
//!
//! Generated decoders borrow from the input bytes and from an [`Arena`], both tied to one
//! lifetime, and never copy. This crate provides the pieces they build on:
//!
//! - [`Arena`] — a bump arena that serves three tiers from one API: `std` (default), `no_std` +
//!   `alloc`, and `no_std` with no allocator (a caller-seeded fixed buffer). Allocation is
//!   fallible and never panics.
//! - [`ProtoStr`] — a borrowed `string` value, validated as UTF-8 only when the caller asks.
//! - [`ProtoBytes`] — a borrowed `bytes` value; it derefs to `[u8]`.
//! - [`MapView`] — a borrowed view over a `map` field's entries.
//! - [`DecodeError`] / [`ErrorKind`] — the error returned by generated `decode` entry points.
//! - [`stream`] — the primitives a generated *streaming* decoder builds on (a visitor that reads only
//!   the fields it selects, never materializing the message); emitted when `rapidproto-build`'s
//!   `Config::stream` is enabled.
//!
//! This is an early stage: the generated code that uses these types is still being built out.
//!
//! Decoded messages implement `Debug`, which prints the fields that are set under their proto names,
//! such as `Station { name: "Zurich", unit: Celsius, temperatures: [12.5, 13.0] }`. A field with
//! presence appears when it is set, even at its default value. That covers `optional` fields, message
//! fields and groups, and editions fields with explicit presence. A field without presence, such as a
//! proto3 scalar with no `optional`, appears when its value is not the default. `required` fields
//! always appear. `repeated` and `map` fields appear when they are not empty, and a `oneof` appears as
//! its set member. A field a decode profile drops does not appear, and a `raw` field shows its bytes.
//!
//! An enum prints its value's name, or `Unknown(7)` for a number the schema does not name. `bytes`
//! print as a byte string, such as `b"\xff"`, and a `string` with invalid UTF-8 as
//! `ProtoStr(b"\xff")`. A stream handle prints only its length.
//!
//! Decoded messages can also implement `serde::Serialize`. Ask for it with `rapidproto-build`'s
//! `Config::serde`, and enable this crate's `serde` feature, which is off by default. RapidProto only
//! decodes, so there is no `Deserialize`.
//!
//! A message serializes as a map from proto field name to value, with the fields `Debug` prints. An
//! enum serializes as its `i32`, and a `raw` field as bytes.
//!
//! `bytes` goes through `serialize_bytes`, so the format decides how it looks. `serde_json` writes an
//! array of numbers. A `string` with invalid UTF-8 serializes as bytes too. A `map` key repeated on the
//! wire appears more than once. A message gives the serializer no length up front, so a format that
//! needs one cannot serialize it.
//!
//! With `serde_json`, a non-finite `float` or `double` becomes `null`, and a `map` fails if a `string`
//! key has invalid UTF-8.
//!
//! `no_std` outside of tests whenever the `std` feature is off; the test harness keeps `std` so
//! the no-allocator path can still be unit-tested with `--no-default-features`.
#![cfg_attr(not(any(feature = "std", test)), no_std)]
#![cfg_attr(docsrs, feature(doc_cfg))]
// `ProtoBytes` shows `[u8]`'s methods, and a few would carry a standard library label.
#![cfg_attr(docsrs, doc(auto_cfg(hide(no_global_oom_handling))))]
#![forbid(unsafe_op_in_unsafe_fn)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![warn(missing_docs)]

#[cfg(feature = "alloc")]
extern crate alloc;

mod arena;
mod decode_slot;
mod decode_vec;
mod error;
mod map;
#[cfg(feature = "alloc")]
mod owned;
mod packed_slice;
mod packed_varint;
mod proto_bytes;
mod proto_str;
#[cfg(feature = "serde")]
mod serde_support;
pub mod stream;
// Generated code and benches read the wire through this. Reading it directly is not a user feature.
#[doc(hidden)]
pub mod wire;

// The generated code in `example` names this crate `::rapidproto`, as it does in a user's crate.
#[cfg(doc)]
extern crate self as rapidproto;
// One doc example calls `Arena::new`, so without `alloc` the module stays out of the doctests.
#[cfg(doc)]
#[cfg_attr(not(feature = "alloc"), cfg(not(doctest)))]
pub mod example;

// Generated `Serialize` impls name this, so a crate using them needs no serde dependency of its own.
#[cfg(feature = "serde")]
#[doc(hidden)]
pub use serde;

pub use arena::Arena;
pub use error::{DecodeError, ErrorKind};
pub use map::MapView;
#[cfg(feature = "alloc")]
pub use owned::{ArenaDecode, Owned};
pub use proto_bytes::ProtoBytes;
pub use proto_str::ProtoStr;

// Generated code, which expands in the user's crate, and tests outside this crate build on these. So
// they stay `pub`, but users never touch them.
#[doc(hidden)]
pub use arena::ArenaExhausted;
#[doc(hidden)]
pub use decode_slot::{decode_value, Init, Slot};
#[doc(hidden)]
pub use decode_vec::DecodeVec;
#[cfg(feature = "alloc")]
#[doc(hidden)]
pub use owned::decode_owned;
#[doc(hidden)]
pub use packed_slice::PackedSlice;

/// Re-export of the standard `Box` (only with `alloc`), so a generated `decode_owned` can name
/// `::rapidproto::Box<[u8]>` without the consumer crate spelling an `alloc`/`std` path itself.
/// Internal to the generated glue, not part of the public API.
#[cfg(feature = "alloc")]
#[doc(hidden)]
pub use alloc::boxed::Box;

// This crate's public items, under one module. Generated stream code re-exports it, so a stream
// macro can name the runtime from a crate that does not depend on it directly. It cannot re-export the
// crate itself: that does not compile in a crate that writes `extern crate rapidproto;`.
#[doc(hidden)]
pub mod rp_rt {
    pub use crate::*;
}

/// Implement the owning-handle API for a generated message type `$msg`: the [`ArenaDecode`] tag impl
/// on `$msg<'static>` plus the `$msg::decode_owned` convenience over [`decode_owned`]. Generated code
/// invokes this once per message instead of expanding the boilerplate into every file; `$msg`
/// resolves at the call site. Internal to the generated glue, not a consumer-facing macro.
///
/// # Safety of the emitted `unsafe impl`
/// A generated message borrows the input/arena only through shared `&'a` references (scalars,
/// `ProtoStr`, `ProtoBytes`, `PackedSlice`, `MapView`, by-value or `&` sub-messages): it is covariant
/// in `'a`, layout-identical at every lifetime, free of interior mutability over the borrow, and points
/// only into the input, arena-allocated memory or `'static` data, never at the `Arena` value — exactly
/// [`ArenaDecode`]'s contract. Only invoke this for a type with those properties (every `rustgen`
/// message has them).
#[cfg(feature = "alloc")]
#[macro_export]
#[doc(hidden)]
macro_rules! impl_decode_owned {
    ($msg:ident) => {
        // SAFETY: `$msg` is a generated message, which meets the contract (see the macro's docs).
        unsafe impl $crate::ArenaDecode for $msg<'static> {
            type Message<'a> = $msg<'a>;
            fn decode_borrowed<'a>(
                rp_input: &'a [u8],
                rp_arena: &'a $crate::Arena<'a>,
            ) -> ::core::result::Result<&'a $msg<'a>, $crate::DecodeError> {
                $msg::decode(rp_input, rp_arena)
            }
        }

        impl<'a> $msg<'a> {
            /// Decode `input` into a self-contained handle that owns the bytes and the arena; read
            /// the message with `Owned::get`. No external lifetime to manage.
            pub fn decode_owned(
                rp_input: impl ::core::convert::Into<$crate::Box<[u8]>>,
            ) -> ::core::result::Result<$crate::Owned<$msg<'static>>, $crate::DecodeError> {
                $crate::decode_owned::<$msg<'static>>(rp_input)
            }
        }
    };
}

// Without `alloc` there is no owning handle: generated code invokes the macro on every tier, and it
// expands to nothing here.
#[cfg(not(feature = "alloc"))]
#[macro_export]
#[doc(hidden)]
macro_rules! impl_decode_owned {
    ($msg:ident) => {};
}

/// How deep a generated `decode` lets sub-messages and groups nest. Deeper nesting fails with
/// [`ErrorKind::DepthLimitExceeded`], so untrusted input cannot exhaust the stack. Matches the C++
/// decoder's `kMaxDecodeDepth`.
pub const MAX_DECODE_DEPTH: u32 = 100;
