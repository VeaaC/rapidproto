//! Streaming-decode primitives: the handful of runtime types a generated streaming decoder builds on.
//! Streaming visits fields as they appear on the wire and never materializes the message, so a
//! consumer pays only for the fields it selects; unselected fields are skipped on the wire with no
//! dispatch.
//!
//! The generator emits, per message, a `MsgVisitor` trait (one `const WANT_<field>: bool = false` plus
//! a defaulted `on_<field>` method per field) and a `Msg::decode` driver generic over any visitor. The
//! driver gates each field on `V::WANT_<field>`; since that is an associated `const`, a release build
//! folds the branch and drops the decode+dispatch for every unselected field, leaving only the skip. A
//! consumer implements the trait directly, or uses the generated per-message `msg_stream! { field =>
//! closure, .. }` macro, which builds an anonymous visitor whose closures capture local state.

/// Whether a streaming handler wants decoding to continue after a field, or stop early with the
/// message decoded so far. A handler closure may return `()` ([`Flow::Continue`]) or a `Flow`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Flow {
    /// Keep decoding.
    Continue,
    /// Stop now; `decode` returns `Ok(())`.
    Stop,
}

impl From<()> for Flow {
    #[inline(always)]
    fn from(_: ()) -> Flow {
        Flow::Continue
    }
}

/// The glue that lets a field handler be any `FnMut(V)` returning `()` or [`Flow`]. The generated
/// `msg_stream!` macro bounds each selected field's closure as `Handler<V>` and calls [`call`](Handler::call),
/// so a handler can end with a plain expression (`Flow::Continue`) or return `Flow::Stop` to halt. Not
/// implemented by consumers; the blanket impl covers every suitable closure.
pub trait Handler<V> {
    /// Deliver one occurrence of the field's value to the handler.
    fn call(&mut self, value: V) -> Flow;
}

impl<V, R: Into<Flow>, F: FnMut(V) -> R> Handler<V> for F {
    #[inline(always)]
    fn call(&mut self, value: V) -> Flow {
        (self)(value).into()
    }
}
