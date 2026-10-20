//! Streaming-decode primitives: the handler types a generated per-message *stream builder* is built
//! from. Streaming visits fields as they appear on the wire and never materializes the message, so a
//! consumer pays only for the fields it selects; unselected fields are skipped on the wire with no
//! dispatch.
//!
//! A generated `MsgStream` carries one type parameter per field, each a [`FieldHandler`]: [`Skip`]
//! (the default — the field is not selected) or [`On`] (a selected field's closure). The builder's
//! `.on_<field>(..)` methods swap a slot from `Skip` to `On`, and the generated decode loop gates
//! each field on [`FieldHandler::WANTED`]; since that is an associated `const`, a release build folds
//! the branch and drops the decode+dispatch for every unselected field, leaving only the skip.

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

/// A per-field handler slot on a generated stream builder. Implemented by exactly two types: [`Skip`]
/// (unselected, [`WANTED`](FieldHandler::WANTED) `= false`) and [`On`] (selected, `= true`). The
/// generated decode loop reads `WANTED` as a `const` to decide decode-and-dispatch vs. wire-skip.
pub trait FieldHandler<V> {
    /// `true` iff this field is selected. A `const` so the generated loop folds it at monomorphization.
    const WANTED: bool;
    /// Deliver one occurrence of the field's value. Never called when `WANTED` is `false`.
    fn handle(&mut self, value: V) -> Flow;
}

/// An unselected field: zero-sized, never invoked (the generated loop skips the field on the wire).
pub struct Skip;

impl<V> FieldHandler<V> for Skip {
    const WANTED: bool = false;
    #[inline(always)]
    fn handle(&mut self, _value: V) -> Flow {
        Flow::Continue
    }
}

/// A selected field: wraps the consumer's closure, called once per occurrence.
pub struct On<F>(pub F);

impl<V, R: Into<Flow>, F: FnMut(V) -> R> FieldHandler<V> for On<F> {
    const WANTED: bool = true;
    #[inline(always)]
    fn handle(&mut self, value: V) -> Flow {
        (self.0)(value).into()
    }
}
