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
    /// Nested-message or group recursion exceeded the decoder's depth cap.
    DepthLimitExceeded,
    /// The arena could not satisfy an allocation (no allocator, or the host allocator failed).
    ArenaExhausted,
}

/// A decode failure: a matchable [`ErrorKind`] plus the byte offset into the input where it was
/// detected. The offset is a uniform accessor rather than a per-variant field, so matching on the
/// kind stays clean.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct DecodeError {
    kind: ErrorKind,
    offset: usize,
}

impl DecodeError {
    /// Construct an error of `kind` detected at `offset` bytes into the input.
    #[inline]
    pub fn new(kind: ErrorKind, offset: usize) -> Self {
        Self { kind, offset }
    }

    /// The kind of failure.
    #[inline]
    pub fn kind(&self) -> ErrorKind {
        self.kind
    }

    /// The byte offset into the input where the failure was detected.
    #[inline]
    pub fn offset(&self) -> usize {
        self.offset
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
        };
        write!(f, "{what} at offset {}", self.offset)
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
