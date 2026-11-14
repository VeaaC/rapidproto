//! Runtime for the Protobuf decoders that [`rapidproto-build`] generates.
//!
//! RapidProto only decodes. `rapidproto-build` generates decoders from your schema at build time, and
//! the generated code uses this crate. Add `rapidproto` to your dependencies and `rapidproto-build` to
//! your build dependencies, both pinned to the same version with `=`. The [`example`] module shows a
//! schema, the build script and the generated code.
//!
//! # Arena and stream decoders
//!
//! An arena decoder decodes the whole message. `Msg::decode(input, &arena)` returns a `&Msg` with an
//! accessor for each field. `string` and `bytes` values borrow from `input`, and everything else is
//! stored in the [`Arena`].
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # #[cfg(feature = "alloc")]
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x14\x0a\x06Zurich\x18\x01\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41";
//! use example::arena::weather::Report;
//! use rapidproto::Arena;
//!
//! let arena = Arena::new();
//! let report = Report::decode(input, &arena)?;
//! assert_eq!(report.stations()[0].name(), "Zurich");
//! # Ok(())
//! # }
//! # #[cfg(not(feature = "alloc"))]
//! # fn main() {}
//! ```
//!
//! `Msg::decode_owned(input)` returns an [`Owned`], which holds the input and the arena, so you do not
//! have to keep them alive yourself.
//!
//! A stream decoder builds no message. It passes the fields you select to your handlers and skips the
//! rest, so it suits reading a few fields of a large message. `rapidproto-build` generates arena
//! decoders by default, and stream decoders when you turn on [`Config::stream`]. See [`stream`].
//!
//! # Decoding rules
//!
//! A decoder skips unknown fields, which the schema does not declare, and declared fields that arrive
//! with the wrong wire type. It does not check that strings are valid UTF-8. [`ProtoStr::to_str`] does
//! that when you read one.
//!
//! An arena decoder fails if a `required` field is missing, or if a singular message field appears
//! twice, where Protobuf would merge the two. A stream decoder checks neither.
//!
//! # Untrusted input
//!
//! An arena decoder fails if sub-messages nest deeper than [`MAX_DECODE_DEPTH`]. Its arena can grow to
//! many times the size of the input, so decode into your own arena and limit it with
//! [`Arena::set_capacity_limit`]. A stream decoder does not allocate, and it decodes a sub-message
//! only when your handler calls `decode` on it.
//!
//! # Features
//!
//! - `alloc`, on by default, lets an [`Arena`] grow on the heap and adds [`Owned`]. Without it, give
//!   the arena a buffer with [`Arena::with_buffer`].
//! - `serde` adds `serde::Serialize` impls. See [Serde](#serde).
//!
//! The crate is `no_std`, and without `serde` it has no dependencies.
//!
//! # Debug output
//!
//! Decoded messages implement `Debug`, which prints the fields that are set under their names in the
//! schema, such as `Station { name: "Zurich", unit: Celsius, temperatures: [12.5, 13.0] }`. A field
//! with presence appears when it is set, even at its default value. That covers `optional` fields,
//! message fields and groups, and editions fields with explicit presence. A field without presence,
//! such as a proto3 scalar with no `optional`, appears when its value is not the default. `required`
//! fields always appear. `repeated` and `map` fields appear when they are not empty, and a `oneof`
//! appears as its set member. A field a decode profile drops does not appear, and a `raw` field shows
//! its bytes.
//!
//! An enum prints its `classify()` variant, such as `Celsius`, or `Unknown(7)` for a number the schema
//! does not name. `bytes` print as a byte string, such as `b"\xff"`, and a `string` with invalid UTF-8
//! as `ProtoStr(b"\xff")`. A stream struct prints the length of its encoded message, such as
//! `Station { len: 20 }`.
//!
//! # Serde
//!
//! With `rapidproto-build`'s [`Config::serde`] and this crate's `serde` feature, decoded messages
//! implement `serde::Serialize`. There is no `Deserialize`.
//!
//! A message serializes as a map from field name to value, with the fields `Debug` prints. An enum
//! serializes as its `i32`, and a `raw` field as bytes.
//!
//! `bytes` goes through `serialize_bytes`, so the format decides how it looks. `serde_json` writes an
//! array of numbers. A `string` with invalid UTF-8 serializes as bytes too. A `map` key repeated on the
//! wire appears more than once. A message gives the serializer no length up front, so a format that
//! needs one cannot serialize it.
//!
//! With `serde_json`, a non-finite `float` or `double` becomes `null`, and a `map` fails if a `string`
//! key has invalid UTF-8.
//!
//! [`rapidproto-build`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/
//! [`Config::stream`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.stream
//! [`Config::serde`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.serde
// Without `alloc` there is no `Owned`, so its links go to the feature list, which says why.
#![cfg_attr(not(feature = "alloc"), doc = "[`Owned`]: #features")]
// The test harness needs `std`, which also lets the no-allocator path be unit-tested with
// `--no-default-features`.
#![cfg_attr(not(test), no_std)]
#![cfg_attr(docsrs, feature(doc_cfg))]
// `ProtoBytes` shows `[u8]`'s methods, and a few would carry a standard library label.
#![cfg_attr(docsrs, doc(auto_cfg(hide(no_global_oom_handling))))]
#![forbid(unsafe_op_in_unsafe_fn)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![warn(missing_docs)]

#[cfg(feature = "alloc")]
extern crate alloc;

mod arena;
// Rustdoc checks these as `compile_fail` doctests.
#[cfg(all(doctest, feature = "alloc"))]
mod compile_fail;
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
pub use map::{MapIter, MapView};
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
                input: &'a [u8],
                arena: &'a $crate::Arena<'a>,
            ) -> ::core::result::Result<&'a $msg<'a>, $crate::DecodeError> {
                $msg::decode(input, arena)
            }
        }

        impl<'a> $msg<'a> {
            // No links: this expands in the user's crate, whose `cargo doc --no-deps` cannot reach
            // this crate's pages, and rustdoc then writes a link's raw path as its address.
            /// Decodes `input` into an `Owned`, which holds the input and the arena.
            ///
            /// Pass a `Vec<u8>` or a `Box<[u8]>` to hand over the buffer. A `&[u8]` is copied.
            /// Read the message with `Owned::get`. Needs the `alloc` feature of `rapidproto`.
            pub fn decode_owned(
                input: impl ::core::convert::Into<$crate::Box<[u8]>>,
            ) -> ::core::result::Result<$crate::Owned<$msg<'static>>, $crate::DecodeError> {
                $crate::decode_owned::<$msg<'static>>(input)
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

/// How deep sub-messages and groups can nest in an arena decoder.
///
/// Deeper nesting fails with [`ErrorKind::DepthLimitExceeded`], which bounds the stack a decode uses.
/// In an unoptimized build stack frames are larger, and a decode this deep can overflow the default
/// 2 MiB stack of a spawned thread.
pub const MAX_DECODE_DEPTH: u32 = 100;
