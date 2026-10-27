//! RapidProto — decode-only Protobuf runtime for generated Rust decoders.
//!
//! Generated decoders borrow from the input bytes and from an [`Arena`], both tied to one
//! lifetime, and never copy. This crate provides the pieces they build on:
//!
//! - [`Arena`] — a bump arena that serves three tiers from one API: `std` (default), `no_std` +
//!   `alloc`, and `no_std` with no allocator (a caller-seeded fixed buffer). Allocation is
//!   fallible and never panics.
//! - [`ProtoStr`] — a borrowed `string` field; the caller chooses whether/how to validate UTF-8.
//! - [`PackedSlice`] / [`PackedStr`] — the 12-byte cells decoded messages store slices and
//!   strings in; they deref to `[T]` and [`ProtoStr`].
//! - [`MapView`] — a borrowed view over a `map` field's entries.
//! - [`DecodeError`] / [`ErrorKind`] — the error returned by generated `decode` entry points.
//! - [`wire`] — the wire-decode primitives (varint, fixed, length-delimited, tag, skip) generated
//!   `decode` bodies read fields with.
//! - [`stream`] — the primitives a generated *streaming* decoder builds on (a visitor that reads only
//!   the fields it selects, never materializing the message); emitted when `rapidproto-build`'s
//!   `Config::stream` is enabled.
//!
//! This is an early stage: the generated code that uses these types is still being built out.
//!
//! With the optional `serde` feature (off by default), generated decoders implement
//! `serde::Serialize` — serialize-only; RapidProto stays decode-only, so there is no
//! `Deserialize`. A default build pulls in no serde. See the `serde_support` module (compiled only
//! with that feature) for what the runtime types serialize to.
//!
//! `no_std` outside of tests whenever the `std` feature is off; the test harness keeps `std` so
//! the no-allocator path can still be unit-tested with `--no-default-features`.
#![cfg_attr(not(any(feature = "std", test)), no_std)]
#![forbid(unsafe_op_in_unsafe_fn)]
#![deny(clippy::undocumented_unsafe_blocks)]

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
mod proto_str;
#[cfg(feature = "serde")]
pub mod serde_support;
pub mod stream;
pub mod wire;

/// Re-export of `serde` (only with the `serde` feature), so generated `Serialize` impls can name
/// `::rapidproto::serde` and a consumer need only enable `rapidproto/serde`, not add serde directly.
#[cfg(feature = "serde")]
pub use serde;

pub use arena::{Arena, ArenaExhausted};
pub use decode_slot::{decode_value, Init, Slot};
pub use decode_vec::DecodeVec;
pub use error::{DecodeError, ErrorKind};
pub use map::MapView;
#[cfg(feature = "alloc")]
pub use owned::{decode_owned, ArenaDecode, Owned};
pub use packed_slice::PackedSlice;
pub use proto_str::{PackedStr, ProtoStr};

/// Re-export of the standard `Box` (only with `alloc`), so a generated `decode_owned` can name
/// `::rapidproto::Box<[u8]>` without the consumer crate spelling an `alloc`/`std` path itself.
/// Internal to the generated glue, not part of the public API.
#[cfg(feature = "alloc")]
#[doc(hidden)]
pub use alloc::boxed::Box;

/// Implement the owning-handle API for a generated message type `$msg`: the [`ArenaDecode`] tag impl
/// on `$msg<'static>` plus the `$msg::decode_owned` convenience over [`decode_owned`]. Generated code
/// invokes this once per message instead of expanding the boilerplate into every file; `$msg`
/// resolves at the call site. Internal to the generated glue, not a consumer-facing macro.
///
/// The emitted items are themselves `#[cfg(feature = "alloc")]`-gated (on the *including* crate's
/// feature), so the invocation needs no gate: without `alloc` it expands to nothing. Defined here at
/// the crate root (not in the `alloc`-only `owned` module) so the macro resolves even in a no-`alloc`
/// build, where it expands away.
///
/// # Safety of the emitted `unsafe impl`
/// A generated message borrows the input/arena only through shared `&'a` references (scalars,
/// `PackedStr`, `PackedSlice`, `MapView`, by-value or `&` sub-messages): it is covariant in `'a`,
/// layout-identical at every lifetime, free of interior mutability over the borrow, and holds no
/// reference to the `Arena` value itself — exactly [`ArenaDecode`]'s contract. Only invoke this for
/// a type with those properties (every `rustgen` message has them).
#[macro_export]
#[doc(hidden)]
macro_rules! impl_decode_owned {
    ($msg:ident) => {
        #[cfg(feature = "alloc")]
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

        #[cfg(feature = "alloc")]
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

/// The maximum nesting a generated `decode` descends through length-delimited sub-messages before
/// giving up with [`ErrorKind::DepthLimitExceeded`], so pathological nesting cannot exhaust the stack.
/// Matches the C++ decoder's `kMaxDecodeDepth`.
pub const MAX_DECODE_DEPTH: u32 = 100;
