//! Types for stream decoders.
//!
//! A stream decoder reads a message's fields in wire order and passes the ones you select to your
//! handlers. It builds no message and does not allocate. `rapidproto-build` generates stream decoders
//! when you turn on its [`Config::stream`]. The [`example`](crate::example) module shows the generated
//! code.
//!
//! For each message `Msg`, the generator emits a stream struct `Msg<'a>`, a visitor trait
//! `MsgVisitor<'a>`, and a macro `msg_stream!` that builds a visitor from handlers. Decode with
//! `Msg::new(input)?.decode(visitor)`.
//!
//! # Handlers
//!
//! With `msg_stream! { field => |value| .. }`, each handler is a closure that captures its own state.
//! Two closures cannot both capture the same `&mut` variable, so to share state, pass it first:
//! `msg_stream!(&mut state; { field => |state, value| .. })`.
//!
//! A handler returns `()`, a [`Flow`], or a `Result` of either with [`DecodeError`]. An error ends the
//! decode, and `decode` returns it. `Flow::Stop` ends the decode, and `decode` returns
//! `Ok(Flow::Stop)`. A decode that reaches the end of its message returns `Ok(Flow::Continue)`.
//!
//! A handler for a sub-message chooses how far a stop reaches. `|sub| sub.decode(..)` returns the
//! result, so a stop inside the sub-message ends the outer `decode` too.
//! `|sub| { sub.decode(..)?; Ok(()) }` discards the `Flow`, so it ends only the sub-message. Both pass
//! an error on.
//!
//! # What a handler receives
//!
//! - A scalar, `string` or `bytes` field: its value. A `repeated` field calls the handler once per
//!   element, packed or not, and a singular field that appears twice on the wire calls it twice.
//! - An enum field: its generated enum type, which holds any `i32`.
//! - A sub-message or group: its stream struct. Call `decode` on it, or ignore it.
//! - A `map` field: one `(key, value)` pair per entry. A message value is its stream struct.
//! - A `oneof`: each member has its own handler, and a later member does not replace an earlier one.
//! - Unknown fields: the `unknown` handler receives each one as an [`UnknownField`].
//!
//! A declared field that arrives with the wrong wire type is skipped, and does not reach `unknown`.
//!
//! # Untrusted input
//!
//! `decode` does not decode sub-messages itself. It hands them to your handlers. If your handlers
//! follow a recursive schema on untrusted input, limit the depth yourself, because
//! [`MAX_DECODE_DEPTH`](crate::MAX_DECODE_DEPTH) applies only to arena decoders. Groups are the
//! exception: nesting them deeper than `MAX_DECODE_DEPTH` fails, even when they are skipped.
//!
//! # Many fields
//!
//! The macro takes one recursion step per selected field, and macro calls nested in handlers share
//! rustc's recursion limit. Above about 120 selected fields, add `#![recursion_limit = "256"]` to your
//! crate.
//!
//! # Writing a visitor by hand
//!
//! The `msg_stream!` macro is the usual way to build a visitor, but you can also implement the
//! generated `MsgVisitor` trait yourself, for example for a reusable visitor type. To select a field,
//! set its `WANT_<field>` to `true` and override its `on_<field>` method. The macro does both, but by
//! hand it is easy to forget `WANT_<field>`, and then `on_<field>` is never called.
//!
//! `decode` takes the visitor by value, so implement the trait for `&mut` your type to keep its state
//! after the decode. A handler for a sub-message passes the same state on with
//! `station.decode(&mut **self)`. This counts the stations and temperatures in `input`, an encoded
//! `Report` from the [`example`](crate::example) schema:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x14\x0a\x06Zurich\x18\x01\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41";
//! use example::stream::weather::{Report, ReportVisitor, Station, StationVisitor};
//! use rapidproto::stream::Flow;
//! use rapidproto::DecodeError;
//!
//! #[derive(Default)]
//! struct Count {
//!     stations: usize,
//!     temperatures: usize,
//! }
//!
//! impl<'a> ReportVisitor<'a> for &mut Count {
//!     const WANT_stations: bool = true;
//!     fn on_stations(&mut self, station: Station<'a>) -> Result<Flow, DecodeError> {
//!         self.stations += 1;
//!         station.decode(&mut **self)
//!     }
//! }
//!
//! impl<'a> StationVisitor<'a> for &mut Count {
//!     const WANT_temperatures: bool = true; // without it, `on_temperatures` is never called
//!     fn on_temperatures(&mut self, _: f32) -> Result<Flow, DecodeError> {
//!         self.temperatures += 1;
//!         Ok(Flow::Continue)
//!     }
//! }
//!
//! let mut count = Count::default();
//! Report::new(input)?.decode(&mut count)?;
//! assert_eq!((count.stations, count.temperatures), (1, 2));
//! # Ok(())
//! # }
//! ```
//!
//! [`Config::stream`]: https://docs.rs/rapidproto-build/latest/rapidproto_build/struct.Config.html#method.stream

use core::fmt;

use crate::error::DecodeError;
pub use crate::wire::WireType;

/// Whether a stream decode continues or stops.
///
/// A handler returns it to choose, and `decode` returns it to say which happened.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Flow {
    /// A handler returns it to keep decoding, as returning `()` does. `decode` returns it when it read
    /// the message to its end.
    Continue,
    /// A handler returns it to end this message's `decode`, and `decode` then returns it too.
    Stop,
}

/// What a field-handler closure may return, normalized to the driver's `Result<Flow, DecodeError>`:
/// `()` continues, a [`Flow`] continues or stops, and a `Result<_, DecodeError>` lets a handler
/// propagate a failure (e.g. `child.decode(..)?` on a malformed sub-message) out of the outer
/// `decode`. Implemented for `()`, `Flow`, `Result<(), DecodeError>`, and `Result<Flow, DecodeError>`.
#[doc(hidden)]
pub trait HandlerRet {
    /// Normalize this return value into the driver's outcome.
    fn into_outcome(self) -> Result<Flow, DecodeError>;
}

impl HandlerRet for () {
    #[inline(always)]
    fn into_outcome(self) -> Result<Flow, DecodeError> {
        Ok(Flow::Continue)
    }
}

impl HandlerRet for Flow {
    #[inline(always)]
    fn into_outcome(self) -> Result<Flow, DecodeError> {
        Ok(self)
    }
}

impl HandlerRet for Result<(), DecodeError> {
    #[inline(always)]
    fn into_outcome(self) -> Result<Flow, DecodeError> {
        self.map(|()| Flow::Continue)
    }
}

impl HandlerRet for Result<Flow, DecodeError> {
    #[inline(always)]
    fn into_outcome(self) -> Result<Flow, DecodeError> {
        self
    }
}

/// The glue that lets a field handler be any `FnMut(V)` returning a [`HandlerRet`]. The generated
/// `msg_stream!` macro bounds each selected field's closure as `Handler<V>` and calls
/// [`call`](Handler::call); not implemented by consumers (the blanket impl covers every suitable
/// closure).
#[doc(hidden)]
pub trait Handler<V> {
    /// Deliver one occurrence of the field's value, returning the driver's outcome.
    fn call(&mut self, value: V) -> Result<Flow, DecodeError>;
}

impl<V, R: HandlerRet, F: FnMut(V) -> R> Handler<V> for F {
    #[inline(always)]
    fn call(&mut self, value: V) -> Result<Flow, DecodeError> {
        (self)(value).into_outcome()
    }
}

/// Fix a field handler's closure parameter to the field's value type `V`, so the consumer need not
/// annotate it even when the body alone does not pin the type (e.g. `|s| s.0`, `|sub| sub.decode(..)`).
/// The generated `msg_stream!` macro wraps each closure with the field's `V` in the turbofish; the
/// returned closure is unchanged. Internal to the generated glue, not called by consumers directly.
#[doc(hidden)]
#[inline(always)]
pub fn typed<V, R, F: FnMut(V) -> R>(f: F) -> F {
    f
}

/// The glue for the shared-state macro form: a field handler that takes the lent `&mut S` as its first
/// argument and the field's value as its second. The generated `msg_stream!(&mut state; { .. })` form
/// bounds each selected field's closure as `Handler2<S, V>` and calls [`call`](Handler2::call),
/// passing one reborrow of the visitor's `&mut state` per occurrence; because the state is passed, not
/// captured, every field's handler can touch it without the aliasing an `FnMut(V)` capture would need.
/// Not implemented by consumers (the blanket impl covers every suitable closure).
#[doc(hidden)]
pub trait Handler2<S: ?Sized, V> {
    /// Deliver one occurrence of the field's value together with the lent state, returning the outcome.
    fn call(&mut self, state: &mut S, value: V) -> Result<Flow, DecodeError>;
}

impl<S: ?Sized, V, R: HandlerRet, F: FnMut(&mut S, V) -> R> Handler2<S, V> for F {
    #[inline(always)]
    fn call(&mut self, state: &mut S, value: V) -> Result<Flow, DecodeError> {
        (self)(state, value).into_outcome()
    }
}

/// Pin a shared-state handler's two parameter types so the consumer annotates neither: the state type
/// `S` is fixed from the `&*state` hint the generated macro passes, and the value type `V` from the
/// turbofish. The `_state_hint` reference is a transient borrow only used to infer `S`; the returned
/// closure is unchanged. Internal to the generated glue, not called by consumers directly.
#[doc(hidden)]
#[inline(always)]
pub fn typed2<S: ?Sized, V, R, F: FnMut(&mut S, V) -> R>(_state_hint: &S, f: F) -> F {
    f
}

/// Builds the anonymous visitor a generated `<msg>_stream!` macro expands to, from the per-field rows
/// that macro collected: `$Tr` is the message's visitor trait at the impl lifetime `'rp_a`. A row is
/// `WANT_<f> on_<f>: V = closure;` -- the trait's const and method names and the field's value type at
/// that lifetime. `on_<f>` doubles as the struct field and its closure's type parameter (separate
/// namespaces), and each `V` gets a
/// block-local alias named by its const (`type WANT_<f><'rp_a> = V`) so the constructor can name it at
/// `'_` to pin the closure's parameter types. A leading `[]` selects the plain form, `[state]` the
/// shared-state form that lends one `&mut state` to every handler. Internal to the generated glue.
/// It expands at the caller's call site, so it names every outside item by an absolute path.
#[macro_export]
#[doc(hidden)]
macro_rules! stream_visitor {
    ($Tr:path [[] $($W:ident $on:ident: $V:ty = $c:expr;)*]) => {{
        $(#[allow(nonstandard_style)] type $W<'rp_a> = $V;)*
        #[allow(nonstandard_style)]
        struct RpVis<$($on),*> { $($on: $on),* }
        #[allow(nonstandard_style)]
        impl<'rp_a, $($on: $crate::stream::Handler<$W<'rp_a>>),*> $Tr
            for RpVis<$($on),*>
        {
            $(
                const $W: ::core::primitive::bool = true;
                #[inline(always)]
                fn $on(&mut self, rp_x: $W<'rp_a>)
                    -> ::core::result::Result<$crate::stream::Flow, $crate::DecodeError> {
                    $crate::stream::Handler::call(&mut self.$on, rp_x)
                }
            )*
        }
        RpVis { $($on: $crate::stream::typed::<$W<'_>, _, _>($c)),* }
    }};
    ($Tr:path [[$state:expr] $($W:ident $on:ident: $V:ty = $c:expr;)*]) => {{
        $(#[allow(nonstandard_style)] type $W<'rp_a> = $V;)*
        // `dead_code`: a zero-field selection leaves `rp_state` read by no handler.
        #[allow(nonstandard_style, dead_code)]
        struct RpVis<'rp_s, RpS: ?::core::marker::Sized, $($on),*> {
            $($on: $on,)*
            rp_state: &'rp_s mut RpS,
        }
        #[allow(nonstandard_style)]
        impl<'rp_a, 'rp_s, RpS: ?::core::marker::Sized, $($on: $crate::stream::Handler2<RpS, $W<'rp_a>>),*>
            $Tr for RpVis<'rp_s, RpS, $($on),*>
        {
            $(
                const $W: ::core::primitive::bool = true;
                #[inline(always)]
                fn $on(&mut self, rp_x: $W<'rp_a>)
                    -> ::core::result::Result<$crate::stream::Flow, $crate::DecodeError> {
                    $crate::stream::Handler2::call(&mut self.$on, &mut *self.rp_state, rp_x)
                }
            )*
        }
        // Evaluated once, before the handlers: their `&*rp_state` hints pin the state type, and the
        // `&mut` moves into the struct last.
        let rp_state = $state;
        RpVis { $($on: $crate::stream::typed2::<_, $W<'_>, _, _>(&*rp_state, $c),)* rp_state }
    }};
}

/// A field the schema does not declare, passed to the `unknown` handler.
#[derive(Clone, Copy)]
pub struct UnknownField<'a> {
    key: u32,
    bytes: &'a [u8],
}

/// Prints the field number, the wire type and the value bytes as a byte string.
impl fmt::Debug for UnknownField<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("UnknownField")
            .field("field_number", &self.field_number())
            .field("wire_type", &self.wire_type())
            .field("bytes", &format_args!("b\"{}\"", self.bytes.escape_ascii()))
            .finish()
    }
}

impl<'a> UnknownField<'a> {
    /// Construct from a raw tag key (`field << 3 | wire`) and the field's value bytes. Called by
    /// generated decoders, which reject the reserved wire types 6 and 7 before they get here.
    #[doc(hidden)]
    #[inline(always)]
    pub fn new(key: u32, bytes: &'a [u8]) -> Self {
        UnknownField { key, bytes }
    }

    /// Returns the field number.
    #[inline(always)]
    pub fn field_number(&self) -> u32 {
        self.key >> 3
    }

    /// Returns the field's wire type.
    #[inline(always)]
    pub fn wire_type(&self) -> WireType {
        // Only a misuse of the hidden `new` can pass a reserved wire type.
        WireType::from_bits(self.key & 0b111).unwrap_or(WireType::Varint)
    }

    /// Returns the field's encoded value, the bytes after its tag.
    ///
    /// A length-delimited value includes its length prefix, and a group includes its end-group tag.
    #[inline(always)]
    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unknown_field_debug() {
        let field = UnknownField::new(5 << 3 | 2, b"\x02hi");
        assert_eq!(
            format!("{field:?}"),
            r#"UnknownField { field_number: 5, wire_type: Len, bytes: b"\x02hi" }"#
        );
    }
}
