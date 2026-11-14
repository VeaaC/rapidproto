//! The error type returned by generated `decode` entry points.

use crate::arena::ArenaExhausted;

/// Why a decode failed.
#[non_exhaustive]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ErrorKind {
    /// The input ended in the middle of a field.
    Truncated,
    /// A field tag has field number 0, a field number above 2^29 - 1, or one of the reserved wire
    /// types 6 and 7.
    InvalidTag,
    /// A varint is longer than ten bytes or does not fit in 64 bits.
    VarintOverflow,
    /// An end-group tag has no matching start-group tag.
    InvalidWireType,
    /// Sub-messages or groups nest too deep.
    ///
    /// An arena decoder allows [`MAX_DECODE_DEPTH`](crate::MAX_DECODE_DEPTH) levels. Every decoder
    /// limits groups to that depth, whether it reads or skips them.
    DepthLimitExceeded,
    /// The arena could not allocate.
    ///
    /// Its buffer is full and it cannot grow, it reached the limit from
    /// [`Arena::set_capacity_limit`](crate::Arena::set_capacity_limit), or the global allocator failed.
    ArenaExhausted,
    /// An arena decoder found no value for a `required` field.
    ///
    /// [`DecodeError::field_number`] gives the field's number.
    MissingRequired,
    /// A singular message field appears more than once.
    ///
    /// An arena decoder returns this where Protobuf would merge the two. A singular field of another
    /// type keeps its last value. [`DecodeError::field_number`] gives the field's number.
    RepeatedSingularMessage,
    /// The input is longer than `u32::MAX` bytes.
    InputTooLarge,
    /// A `string` is not valid UTF-8.
    ///
    /// A decoder never returns this, since it does not check strings. It comes from `From<Utf8Error>`,
    /// so `?` on [`ProtoStr::to_str`](crate::ProtoStr::to_str) works in a stream handler. The offset is
    /// the position of the first invalid byte in the string, not in the input.
    InvalidUtf8,
}

/// The error a decoder returns.
///
/// It holds an [`ErrorKind`], a byte offset and, for some kinds, a field number.
///
/// The offset counts from the start of the message that failed. For a sub-message or a map entry, that
/// is the first byte after its length prefix, and for a group the first byte after its start-group tag.
/// It is not the start of the whole input, so the offset alone does not locate the error in the input.
/// A value that is cut short, or a malformed group, reports where its value starts, just after its
/// tag. A decoder reports an [`ArenaExhausted`](ErrorKind::ArenaExhausted) error at offset 0.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct DecodeError {
    kind: ErrorKind,
    offset: usize,
    field: Option<u32>,
}

impl DecodeError {
    /// Creates an error of `kind` at `offset`, with no field number.
    ///
    /// A stream handler can return one to end the decode.
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

    /// Returns the kind of failure.
    #[inline]
    pub fn kind(&self) -> ErrorKind {
        self.kind
    }

    /// Returns the byte offset of the failure, counted from the start of the message that failed.
    #[inline]
    pub fn offset(&self) -> usize {
        self.offset
    }

    /// Returns the number of the field the error is about, such as a missing `required` field.
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
    /// An [`ArenaExhausted`](ErrorKind::ArenaExhausted) error at offset 0, since the arena does not
    /// know a position in the input.
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
