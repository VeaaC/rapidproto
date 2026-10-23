//! Streaming-decode primitives: the runtime types a generated streaming decoder builds on. Streaming
//! visits fields as they appear on the wire and never materializes the message, so a consumer pays
//! only for the fields it selects; unselected fields are skipped on the wire with no dispatch.
//!
//! The generator emits, per message `Msg`: a reader handle `Msg<'a>` (`Msg::new(bytes).decode(visitor)`),
//! a `MsgVisitor<'a>` trait (one `const WANT_<field>: bool = false` plus a defaulted
//! `on_<field>(&mut self, V) -> Result<Flow, DecodeError>` method per field), and a per-message
//! `msg_stream! { field => closure, .. }` macro that builds a visitor whose closures capture local
//! state. The macro also takes an optional shared-state form,
//! `msg_stream!(&mut state; { field => |state, value| .. })`, where the driver lends one `&mut state`
//! to every handler as its first argument instead of each closure capturing it — the way to aggregate
//! many fields into one accumulator (or parse into one struct), which exclusive-`&mut` captures cannot
//! express. The decode driver gates each field on `V::WANT_<field>` (an associated `const`, so a release
//! build folds the branch and drops the decode+dispatch for every unselected field, leaving only the
//! wire-skip). A handler returns `()`/`Flow`/`Result` (see [`HandlerRet`]): `Flow::Stop` ends the
//! decode early as a clean `Ok(())`, and an `Err(DecodeError)` propagates out of `decode` — so a handler
//! that decodes a sub-message with `child.decode(..)?` surfaces a malformed-child error naturally.
//!
//! The macro expands one recursion step per selected field, and calls nested inside handlers share
//! rustc's macro recursion budget: selecting more than about 120 fields in one call (fewer across nested
//! calls) needs a raised `#![recursion_limit = "256"]` in the calling crate.
//!
//! Streaming is a raw field visit, not a materialized message, so the consumer owns anything the arena
//! model would resolve: a handler fires once per wire occurrence of its field (a repeated field, or a
//! singular one duplicated by a non-canonical encoder, calls the handler more than once), and the
//! members of a `oneof` are independent handlers, not collapsed to last-wins — overwrite in the closure
//! if that is what you want. A sub-message or group field hands the consumer the child's reader handle
//! (`child.decode(child_visitor)`); an enum is delivered as its generated newtype; a map field delivers
//! each entry as a `(key, value)` pair (a message value is the child's reader handle). There is no
//! catch-all ("every known field") handler: the per-field visitor is the selection mechanism. For
//! fields a schema does not declare, select the per-message `unknown` handler (it receives an
//! [`UnknownField`]).
//!
//! `decode` itself never recurses: a nested message/group/map value is handed over as a child reader
//! handle (and anything unselected is skipped iteratively), so adversarial input alone cannot overflow
//! the stack through the decoder. A consumer that walks a self-referential schema by calling
//! `child.decode(..)` inside a handler owns that recursion depth and must bound it if the input is
//! untrusted — the arena model's decode-depth cap does not apply to consumer-driven recursion.
//!
//! # Hand-implementing a visitor
//!
//! The `msg_stream!` macro is the usual way to build a visitor, but the generated `<Msg>Visitor` trait
//! can be implemented by hand (e.g. for a reusable visitor type). A field is selected by overriding
//! **both** its `WANT_<field>` const **and** its `on_<field>` method: the macro always emits the pair,
//! but by hand it is easy to write `on_<field>` and forget `WANT_<field> = true`, leaving the field at
//! its default `WANT_<field> = false` so the driver never dispatches it — the method silently never
//! fires. The generated trait is shaped like the toy one below:
//!
//! ```
//! use rapidproto::stream::Flow;
//! use rapidproto::DecodeError;
//!
//! // Shaped like a generated `<Msg>Visitor` (one `WANT_`/`on_` pair per field).
//! trait ExampleVisitor<'a> {
//!     #[allow(non_upper_case_globals)]
//!     const WANT_value: bool = false;
//!     fn on_value(&mut self, _v: i64) -> Result<Flow, DecodeError> { Ok(Flow::Continue) }
//! }
//!
//! struct Collect(Vec<i64>);
//! impl<'a> ExampleVisitor<'a> for Collect {
//!     #[allow(non_upper_case_globals)]
//!     const WANT_value: bool = true; // required, or `on_value` is never dispatched
//!     fn on_value(&mut self, v: i64) -> Result<Flow, DecodeError> {
//!         self.0.push(v);
//!         Ok(Flow::Continue)
//!     }
//! }
//! # let _ = Collect(Vec::new());
//! ```

use crate::error::DecodeError;
use crate::wire::WireType;

/// Whether a streaming handler wants decoding to continue after a field, or stop early with the
/// message decoded so far. A handler may return `()` ([`Flow::Continue`]) or a `Flow`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Flow {
    /// Keep decoding.
    Continue,
    /// Stop now; `decode` returns `Ok(())`.
    Stop,
}

/// What a field-handler closure may return, normalized to the driver's `Result<Flow, DecodeError>`:
/// `()` continues, a [`Flow`] continues or stops, and a `Result<_, DecodeError>` lets a handler
/// propagate a failure (e.g. `child.decode(..)?` on a malformed sub-message) out of the outer
/// `decode`. Implemented for `()`, `Flow`, `Result<(), DecodeError>`, and `Result<Flow, DecodeError>`.
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
/// that macro collected: `$scope` is the message's stream module (re-exported under a per-message
/// name) and `$Tr` its visitor trait. A row is `WANT_<f> on_<f>: V = closure;` -- the trait's const and
/// method names and the field's value type at the impl lifetime `'rp_a`. `on_<f>` doubles as the
/// struct field and its closure's type parameter (separate namespaces), and each `V` gets a
/// block-local alias named by its const (`type WANT_<f><'rp_a> = V`) so the constructor can name it at
/// `'_` to pin the closure's parameter types. A leading `[]` selects the plain form, `[state]` the
/// shared-state form that lends one `&mut state` to every handler. Internal to the generated glue.
/// It expands at the caller's call site, so it names every outside item by an absolute path.
#[macro_export]
#[doc(hidden)]
macro_rules! stream_visitor {
    ($scope:ident $Tr:ident [[] $($W:ident $on:ident: $V:ty = $c:expr;)*]) => {{
        $(#[allow(nonstandard_style)] type $W<'rp_a> = $V;)*
        #[allow(nonstandard_style)]
        struct RpVis<$($on),*> { $($on: $on),* }
        #[allow(nonstandard_style)]
        impl<'rp_a, $($on: $crate::stream::Handler<$W<'rp_a>>),*> $scope::$Tr<'rp_a>
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
    ($scope:ident $Tr:ident [[$state:expr] $($W:ident $on:ident: $V:ty = $c:expr;)*]) => {{
        $(#[allow(nonstandard_style)] type $W<'rp_a> = $V;)*
        // `dead_code`: a zero-field selection leaves `rp_state` read by no handler.
        #[allow(nonstandard_style, dead_code)]
        struct RpVis<'rp_s, RpS: ?::core::marker::Sized, $($on),*> {
            $($on: $on,)*
            rp_state: &'rp_s mut RpS,
        }
        #[allow(nonstandard_style)]
        impl<'rp_a, 'rp_s, RpS: ?::core::marker::Sized, $($on: $crate::stream::Handler2<RpS, $W<'rp_a>>),*>
            $scope::$Tr<'rp_a> for RpVis<'rp_s, RpS, $($on),*>
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

/// A field on the wire that the message's schema does not declare, delivered to a message's `unknown`
/// handler (when selected). Carries the field number, its wire type, and the field's raw value bytes:
/// the bytes after the tag, exactly as they appear on the wire — for a length-delimited field this
/// includes the length prefix, and for a group it is the body plus the trailing end-group tag.
#[derive(Clone, Copy)]
pub struct UnknownField<'a> {
    key: u32,
    bytes: &'a [u8],
}

impl<'a> UnknownField<'a> {
    /// Construct from a raw tag key (`field << 3 | wire`) and the field's value bytes. Called by
    /// generated decoders; not part of the stable surface.
    #[doc(hidden)]
    #[inline(always)]
    pub fn new(key: u32, bytes: &'a [u8]) -> Self {
        UnknownField { key, bytes }
    }

    /// The field number.
    #[inline(always)]
    pub fn field_number(&self) -> u32 {
        self.key >> 3
    }

    /// The field's wire type (`None` only for the reserved wire codes 6/7, which a decoded field
    /// never carries).
    #[inline(always)]
    pub fn wire_type(&self) -> Option<WireType> {
        WireType::from_bits(self.key & 0b111)
    }

    /// The field's raw value bytes (after the tag).
    #[inline(always)]
    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }
}
