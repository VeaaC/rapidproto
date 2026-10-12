//! A borrowed `string` field.

use core::fmt::{self, Write as _};

/// A borrowed protobuf `string` field: the raw UTF-8 bytes, with validation left to the caller.
///
/// Decoding does not validate (matching the C++ runtime, which trusts the schema), so a `ProtoStr`
/// always borrows the input directly and costs nothing to produce. The caller picks the cost it
/// wants: [`as_bytes`](ProtoStr::as_bytes) is free, [`to_str`](ProtoStr::to_str) validates and can
/// fail, and [`to_str_lossy`](ProtoStr::to_str_lossy) (with `alloc`) never fails. `Display` writes
/// the text, substituting U+FFFD for any invalid sequence. Proto `bytes` fields are `&[u8]`
/// directly and do not use this type.
#[derive(Clone, Copy)]
pub struct ProtoStr<'a> {
    bytes: &'a [u8],
}

impl<'a> ProtoStr<'a> {
    /// Wrap raw bytes (no validation).
    #[inline]
    pub fn new(bytes: &'a [u8]) -> Self {
        Self { bytes }
    }

    /// The raw bytes, unchanged. Free.
    #[inline]
    pub fn as_bytes(&self) -> &'a [u8] {
        self.bytes
    }

    /// Length in bytes.
    #[inline]
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// Whether the field is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// Validate as UTF-8, borrowing the input on success.
    #[inline]
    pub fn to_str(&self) -> Result<&'a str, core::str::Utf8Error> {
        core::str::from_utf8(self.bytes)
    }

    /// Validate as UTF-8, substituting U+FFFD for invalid sequences. Borrows the input when it is
    /// already valid, otherwise allocates the repaired copy.
    #[cfg(feature = "alloc")]
    #[inline]
    pub fn to_str_lossy(&self) -> alloc::borrow::Cow<'a, str> {
        // `from_utf8_lossy` already borrows for the valid case (tied to `self.bytes: &'a`) and owns
        // the repaired copy otherwise, in a single scan.
        alloc::string::String::from_utf8_lossy(self.bytes)
    }
}

impl fmt::Display for ProtoStr<'_> {
    /// Writes the text, substituting U+FFFD for each invalid sequence. Allocation-free.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut rem = self.bytes;
        loop {
            match core::str::from_utf8(rem) {
                Ok(s) => return f.write_str(s),
                Err(e) => {
                    let valid = e.valid_up_to();
                    // The prefix up to `valid` is valid UTF-8 by definition of `valid_up_to`.
                    f.write_str(core::str::from_utf8(&rem[..valid]).unwrap_or(""))?;
                    f.write_char(char::REPLACEMENT_CHARACTER)?;
                    match e.error_len() {
                        Some(n) => rem = &rem[valid + n..],
                        None => return Ok(()), // invalid bytes at the very end
                    }
                }
            }
        }
    }
}

impl fmt::Debug for ProtoStr<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Valid UTF-8 → `str`'s Debug (proper escaping of quotes/newlines/control bytes); otherwise
        // show the raw bytes so invalid content is unambiguous.
        match self.to_str() {
            Ok(s) => fmt::Debug::fmt(s, f),
            Err(_) => write!(f, "ProtoStr({:?})", self.bytes),
        }
    }
}

impl PartialEq for ProtoStr<'_> {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        self.bytes == other.bytes
    }
}
impl Eq for ProtoStr<'_> {}

impl PartialEq<str> for ProtoStr<'_> {
    #[inline]
    fn eq(&self, other: &str) -> bool {
        self.bytes == other.as_bytes()
    }
}

impl PartialEq<&str> for ProtoStr<'_> {
    #[inline]
    fn eq(&self, other: &&str) -> bool {
        self.bytes == other.as_bytes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    extern crate alloc;
    use alloc::string::ToString;

    #[test]
    fn valid_utf8_round_trips() {
        let s = ProtoStr::new(b"hello");
        assert_eq!(s.to_str().unwrap(), "hello");
        assert_eq!(s.to_string(), "hello");
        assert_eq!(s, "hello");
        assert!(!s.is_empty());
        assert_eq!(s.len(), 5);
    }

    #[test]
    fn invalid_utf8_is_bytes_but_display_is_lossy() {
        let s = ProtoStr::new(b"a\xFFb");
        assert!(s.to_str().is_err());
        assert_eq!(s.as_bytes(), b"a\xFFb");
        assert_eq!(s.to_string(), "a\u{FFFD}b"); // U+FFFD for the invalid byte
    }

    #[test]
    fn empty() {
        let s = ProtoStr::new(b"");
        assert!(s.is_empty());
        assert_eq!(s.to_string(), "");
    }
}
