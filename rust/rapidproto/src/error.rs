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
    /// Nesting exceeded a depth limit. In an arena decoder, [`MAX_DECODE_DEPTH`](crate::MAX_DECODE_DEPTH)
    /// limits sub-messages and groups. Groups have an equal limit in every decoder, whether the field is
    /// read or skipped.
    DepthLimitExceeded,
    /// The arena could not allocate. Its buffer is full and it cannot grow, it reached the limit from
    /// [`Arena::set_capacity_limit`](crate::Arena::set_capacity_limit), or the global allocator failed.
    ArenaExhausted,
    /// A `required` (proto2) field was absent from the message. The error carries the missing field's
    /// number (see [`DecodeError::field_number`]).
    MissingRequired,
    /// A singular (non-repeated) message field appeared more than once. RapidProto rejects this rather
    /// than merging the occurrences (matching the C++ decoder); singular scalar fields keep the last.
    RepeatedSingularMessage,
    /// The input is larger than the decoder supports (`> u32::MAX` bytes).
    InputTooLarge,
    /// A `string` is not valid UTF-8. A decoder never returns this, since it does not check strings.
    /// It comes from `From<Utf8Error>`, so `?` on [`ProtoStr::to_str`](crate::ProtoStr::to_str) works
    /// in a stream handler. The offset is the position of the first invalid byte in the string, not in
    /// the input.
    InvalidUtf8,
}

/// A decode failure: a matchable [`ErrorKind`], a byte offset, and the field number it concerns when
/// one applies.
///
/// The offset counts from the start of the message that failed. Inside a sub-message or group, that is
/// the start of its body, not of the whole input, so the offset alone does not locate the error in
/// the input. A value that is cut short reports where the value starts. An
/// [`ArenaExhausted`](ErrorKind::ArenaExhausted) error always has offset 0.
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
    #[doc(hidden)]
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

    /// The byte offset of the failure, counted from the start of the message that failed (see
    /// [`DecodeError`]).
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
            ErrorKind::InvalidUtf8 => {
                return write!(f, "invalid UTF-8 at byte {} of a string", self.offset);
            }
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

impl From<core::str::Utf8Error> for DecodeError {
    /// An [`InvalidUtf8`](ErrorKind::InvalidUtf8) error at the first invalid byte of the string.
    #[inline]
    fn from(error: core::str::Utf8Error) -> Self {
        DecodeError::new(ErrorKind::InvalidUtf8, error.valid_up_to())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn invalid_utf8_reports_its_position_in_the_string() {
        let to_str = |bytes: &[u8]| -> Result<(), DecodeError> {
            core::str::from_utf8(bytes)?;
            Ok(())
        };
        let error = to_str(b"ab\xffc").unwrap_err();
        assert_eq!((error.kind(), error.offset()), (ErrorKind::InvalidUtf8, 2));
        assert_eq!(error.to_string(), "invalid UTF-8 at byte 2 of a string");
    }
}
