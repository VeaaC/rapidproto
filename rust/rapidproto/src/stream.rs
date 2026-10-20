//! Streaming-decode primitives: the runtime types a generated streaming decoder builds on. Streaming
//! visits fields as they appear on the wire and never materializes the message, so a consumer pays
//! only for the fields it selects; unselected fields are skipped on the wire with no dispatch.
//!
//! The generator emits, per message `Msg`: a reader handle `Msg<'a>` (`Msg::new(bytes).decode(visitor)`),
//! a `MsgVisitor<'a>` trait (one `const WANT_<field>: bool = false` plus a defaulted
//! `on_<field>(&mut self, V) -> Result<Flow, DecodeError>` method per field), and a per-message
//! `msg_stream! { field => closure, .. }` macro that builds a visitor whose closures capture local
//! state. The decode driver gates each field on `V::WANT_<field>` (an associated `const`, so a release
//! build folds the branch and drops the decode+dispatch for every unselected field, leaving only the
//! wire-skip). A handler returns `()`/`Flow`/`Result` (see [`HandlerRet`]): `Flow::Stop` ends the
//! decode early as a clean `Ok(())`, and an `Err(DecodeError)` propagates out of `decode` — so a handler
//! that decodes a sub-message with `child.decode(..)?` surfaces a malformed-child error naturally.
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
