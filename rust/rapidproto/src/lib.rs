//! The runtime that RapidProto's generated Protobuf decoders call.
//!
//! [`rapidproto-build`] generates the decoders from your schemas at build time. Decoded messages
//! are read-only; RapidProto has no encoder and no reflection. Your crate lists `rapidproto` as a
//! dependency and `rapidproto-build` as a build dependency, and its build script is written as
//! `rapidproto-build`'s docs show. Generated code calls the runtime of its own release, so pin both
//! crates to one version with `=`. Both are test releases, and the API can change from one to the
//! next. For a complete case, the [`example`] module has a schema, its `build.rs` and everything
//! generated from it.
//!
//! # Arena and streaming decoders
//!
//! An arena decoder decodes the whole message into an [`Arena`]. `Msg::decode(input, &arena)`
//! returns a `&Msg` with an [accessor](example#accessors) for each field, or fails with a
//! [`DecodeError`]:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # #[cfg(feature = "alloc")]
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x14\x0a\x06Zurich\x18\x01\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41";
//! use example::arena::weather::Report;
//! use rapidproto::Arena;
//!
//! // `input` holds an encoded `Report` of the example schema.
//! let arena = Arena::new();
//! let report = Report::decode(input, &arena)?;
//! assert_eq!(report.stations()[0].name(), "Zurich");
//! # Ok(())
//! # }
//! # #[cfg(not(feature = "alloc"))]
//! # fn main() {}
//! ```
//!
//! `report` borrows from the input as well as the arena. Its `string` and `bytes` fields point into
//! `input`, and so does any sub-message field that a [decode profile] keeps `raw`. Everything else
//! lives in `arena`. The compiler won't let `report` outlive either of them. If you need a message
//! that stands on its own, `Report::decode_owned(input)` returns an [`Owned`], which keeps the
//! input and the arena together with the message.
//!
//! The [streaming decoder](stream) calls your handler for each field you select, in the order the
//! fields appear in the input, and skips the rest without building a message. Use it when you need
//! a few fields of a large message. The arena decoder would decode all of it. `rapidproto-build`
//! generates arena decoders by default and adds streaming ones when you turn on
//! [`Config::stream`].
//!
//! # Decoding rules
//!
//! Both decoders skip unknown fields, and declared fields that arrive with the wrong wire type. A
//! streaming decoder can hand unknown fields to an `unknown` handler instead, and
//! [`Config::unknown_present`] lets an arena message record that it had some. Neither decoder
//! checks that a `string` is valid UTF-8; [`ProtoStr::to_str`] does when you read it.
//!
//! A missing `required` field fails an arena decode with [`ErrorKind::MissingRequired`]. A singular
//! message or group field that appears twice fails it with [`ErrorKind::RepeatedSingularMessage`],
//! where Protobuf would merge the two. Either way, [`DecodeError::field_number`] names the field. A
//! singular field of any other type just keeps its last value. The streaming decoder checks neither
//! rule and calls the field's handler once per occurrence.
//!
//! The rest of the rules both decoders follow are in [docs/semantics.md]. Read them if a producer
//! may concatenate messages, or if your schema uses extensions or closed enums, since RapidProto
//! decodes each of those differently from Protobuf.
//!
//! # Untrusted input
//!
//! On malformed input, `decode` returns a `DecodeError` instead of panicking. CI fuzzes the arena
//! decoders under AddressSanitizer on every pull request. An arena decode fails with
//! [`ErrorKind::DepthLimitExceeded`] when sub-messages or groups nest deeper than
//! [`MAX_DECODE_DEPTH`].
//!
//! A decoded message can take many times its input size in the arena. With `alloc`, the arena keeps
//! growing unless you set a limit. An empty [`Station`](example::arena::weather::Station) in a
//! `Report` takes two bytes of input and at least a whole `Station` struct in the arena, more while
//! the array of stations grows. Decode untrusted input with `decode` into an arena you create and
//! limit with [`Arena::set_capacity_limit`]. You can't limit the arena that `decode_owned` makes
//! for itself.
//!
//! The limit counts the bytes the arena holds, not the input's. To pick one, decode the inputs you
//! accept that need the most arena, read the arena's [`bytes_reserved`](Arena::bytes_reserved), and
//! allow a few times the most you see. Those are the inputs with many small sub-messages or
//! repeated values; long strings cost little, since they borrow from the input. Past the limit, the
//! decode fails with [`ErrorKind::ArenaExhausted`].
//!
//! The streaming decoder allocates nothing, though your handlers may. It decodes a sub-message only
//! when a handler calls `decode` on it, so a handler that follows a recursive schema has to
//! [limit the depth itself](stream#untrusted-input).
//!
//! # Features
//!
//! - `alloc`, on by default, lets the arena grow on the heap and adds `Arena::new`, `decode_owned`
//!   and `Owned`. Without it, the arena lives in a buffer you pass to [`Arena::with_buffer`], and a
//!   decode that needs more space fails.
//! - `serde` lets the decoded messages implement `serde::Serialize`. See [Serde](#serde).
//!
//! The crate is always `no_std`, and without `serde` it has no dependencies.
//!
//! # Debug output
//!
//! Every decoded message implements `Debug` and prints its fields under their names in the schema.
//! The first station in the example's `report` prints as:
//!
//! ```text
//! Station { name: "Zurich", unit: Celsius, temperatures: [12.5, 13.0] }
//! ```
//!
//! Which fields it prints depends on their kind:
//!
//! | Field | Printed |
//! |---|---|
//! | With presence, such as `optional`, a message or a group | When set, even to its default |
//! | Without presence, such as a plain proto3 scalar | When it differs from the default (`-0.0` counts as different) |
//! | `required` | Always |
//! | `repeated` or `map` | When not empty |
//! | `oneof` | Its set member, under the member's name |
//! | Dropped by a decode profile | Never |
//!
//! An enum prints as its `classify()` variant, such as `Celsius`, or as `Unknown(7)` for a number
//! the schema doesn't name. `bytes` and `raw` fields print as byte strings like `b"\xff"`, and a
//! `string` that isn't valid UTF-8 as `ProtoStr(b"\xff")`. A [stream struct](stream) prints only
//! the length of its encoded message, as in `Station { len: 20 }`.
//!
//! # Serde
//!
//! With [`Config::serde`] in your build script and this crate's `serde` feature on, the arena
//! messages and the schema's enums implement `serde::Serialize`. Stream structs don't, and nothing
//! implements `Deserialize`. You add the format crate, such as `serde_json`, but not `serde`
//! itself.
//!
//! A message serializes as a map from field name to value, holding the fields
//! [`Debug` prints](#debug-output). An enum serializes as its `i32`. `bytes` and `raw` fields, and
//! any `string` that isn't valid UTF-8, go through `serialize_bytes`, which `serde_json` writes as
//! an array of numbers. A `map` key that appears twice on the wire appears twice in the output.
//! A message doesn't tell `serialize_map` its length up front, so a format that needs the length
//! can't serialize it.
//!
//! With `serde_json`, a `float` or `double` that is NaN or infinite becomes `null`, and a `map`
//! whose `string` key isn't valid UTF-8 fails to serialize.
//!
//! [`rapidproto-build`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/
//! [decode profile]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.field_modes
//! [`Config::stream`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.stream
//! [`Config::unknown_present`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.unknown_present
//! [`Config::serde`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.serde
//! [docs/semantics.md]: https://github.com/VeaaC/rapidproto/blob/main/docs/semantics.md
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

// The version check of generated code. Each generated file calls this with its generator's number,
// `kGeneratedAbi` in src/rustgen/generator.cpp. Any other input fails the build, whatever its shape,
// so a later generator may change the call. rustc reports macro errors before name and type errors,
// so a user who mixes versions of rapidproto and rapidproto-build sees this message first, not dozens
// of errors about this crate's internals.
// Bump both numbers when generated code needs an item that earlier versions lack, or when an item it
// uses changes or goes away. This holds for public items as well as hidden ones. A change in
// behavior counts too, even if everything still compiles. The goldens catch a bump on only one side.
// Nothing catches a missing bump, so each release decides whether one is needed.
#[doc(hidden)]
#[macro_export]
macro_rules! rp_abi {
    (1) => {};
    ($($other:tt)*) => {
        ::core::compile_error!("use the same version of rapidproto and rapidproto-build");
    };
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
            /// Pass a `Vec<u8>` or a `Box<[u8]>` to hand over the buffer. A `&[u8]` is copied, and a
            /// `Vec` with spare capacity may be too. Read the message with `Owned::get`. Needs the `alloc` feature of `rapidproto`.
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
