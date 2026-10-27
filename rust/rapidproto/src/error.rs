//! The error type returned by generated `decode` entry points.

use crate::arena::ArenaExhausted;

/// Why a decode failed. `#[non_exhaustive]`: variants may be added without a breaking change, so
/// callers must include a `_` arm when matching.
#[non_exhaustive]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ErrorKind {
    /// The input ended in the middle of a field, length-delimited payload, or varint.
    Truncated,
    /// A field tag was malformed: a field number of 0 or above the Protobuf maximum (2^29 - 1), or a
    /// reserved wire type (6 or 7).
    InvalidTag,
    /// A base-128 varint did not fit 64 bits: more than ten bytes, or a tenth byte with bits above
    /// bit 0. Unlike [`InvalidTag`](Self::InvalidTag) this is context-neutral -- it is returned
    /// wherever a varint is read (a field value or a length prefix, not only a tag).
    VarintOverflow,
    /// A field's wire type is not one the decoder accepts for that position.
    InvalidWireType,
    /// Nesting exceeded a depth cap: [`MAX_DECODE_DEPTH`](crate::MAX_DECODE_DEPTH) for decoded
    /// sub-messages and groups, [`MAX_GROUP_DEPTH`](crate::wire::MAX_GROUP_DEPTH) for groups the
    /// wire reader skips or reads.
    DepthLimitExceeded,
    /// The arena could not satisfy an allocation (no allocator, or the host allocator failed).
    ArenaExhausted,
    /// A `required` (proto2) field was absent from the message. The error carries the missing field's
    /// number (see [`DecodeError::field_number`]).
    MissingRequired,
    /// A singular (non-repeated) message field appeared more than once. RapidProto rejects this rather
    /// than merging the occurrences (matching the C++ decoder); singular scalar fields keep the last.
    RepeatedSingularMessage,
    /// The input is larger than the decoder supports (`> u32::MAX` bytes).
    InputTooLarge,
}

/// A decode failure: a matchable [`ErrorKind`], the byte offset into the input where it was detected,
/// and the field number it concerns when one applies. The offset is relative to the message body being
/// decoded -- for a failure inside a nested length-delimited sub-message it is relative to that
/// sub-message, not the top-level input (matching the C++ decoder). Offset and field number are uniform
/// accessors rather than per-variant fields, so matching on the kind stays clean.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct DecodeError {
    kind: ErrorKind,
    offset: usize,
    field: Option<u32>,
}

impl DecodeError {
    /// Construct an error of `kind` detected at `offset` bytes into the message body, with no field
    /// number attached.
    #[inline]
    pub fn new(kind: ErrorKind, offset: usize) -> Self {
        Self {
            kind,
            offset,
            field: None,
        }
    }

    /// Attach the field number this error concerns (e.g. a missing `required` field).
    #[inline]
    #[must_use]
    pub fn with_field(mut self, field: u32) -> Self {
        self.field = Some(field);
        self
    }

    /// The kind of failure.
    #[inline]
    pub fn kind(&self) -> ErrorKind {
        self.kind
    }

    /// The byte offset into the message body where the failure was detected.
    #[inline]
    pub fn offset(&self) -> usize {
        self.offset
    }

    /// The field number this error concerns, when one applies (e.g. a missing `required` field or a
    /// repeated singular message); `None` otherwise.
    #[inline]
    pub fn field_number(&self) -> Option<u32> {
        self.field
    }
}

impl core::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let what = match self.kind {
            ErrorKind::Truncated => "truncated input",
            ErrorKind::InvalidTag => "invalid field tag",
            ErrorKind::VarintOverflow => "varint overflow",
            ErrorKind::InvalidWireType => "invalid wire type",
            ErrorKind::DepthLimitExceeded => "depth limit exceeded",
            ErrorKind::ArenaExhausted => "arena exhausted",
            ErrorKind::MissingRequired => "missing required field",
            ErrorKind::RepeatedSingularMessage => "repeated singular message field",
            ErrorKind::InputTooLarge => "input too large",
        };
        match self.field {
            Some(field) => write!(f, "{what} (field {field}) at offset {}", self.offset),
            None => write!(f, "{what} at offset {}", self.offset),
        }
    }
}

// `core::error::Error` works in `no_std` too, so no feature gate is needed.
impl core::error::Error for DecodeError {}

impl From<ArenaExhausted> for DecodeError {
    /// The arena does not know the wire offset, so this yields offset 0; the decode layer that
    /// knows its position should prefer [`DecodeError::new`] with the real offset.
    #[inline]
    fn from(_: ArenaExhausted) -> Self {
        DecodeError::new(ErrorKind::ArenaExhausted, 0)
    }
}
