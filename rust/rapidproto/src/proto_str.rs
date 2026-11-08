//! A protobuf `string` value.

use core::cmp::Ordering;
use core::fmt::{self, Write as _};
use core::hash::{Hash, Hasher};

use crate::{PackedSlice, ProtoBytes};

/// A protobuf `string`: its raw bytes, borrowed for `'a`, with UTF-8 validation left to the caller.
///
/// Decoding does not validate, so a `ProtoStr` costs nothing to produce. The caller picks the cost it
/// wants: [`as_bytes`](ProtoStr::as_bytes) is free, [`to_str`](ProtoStr::to_str) validates and can
/// fail, and `to_str_lossy` (with `alloc`) never fails. The methods take the `ProtoStr` by value, and
/// what they return borrows for the full `'a`.
///
/// It compares with `str`, as in `station.name() == "Zurich"`. `Display` writes the text with U+FFFD
/// for each invalid sequence. `Default` is the empty string.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct ProtoStr<'a>(PackedSlice<'a, u8>);

impl<'a> ProtoStr<'a> {
    /// View `bytes` as a string, without copying or validating them. Use it for a `ProtoStr` that does
    /// not come from decoding, such as a lookup key or a test value.
    ///
    /// # Panics
    ///
    /// If `bytes` is longer than `u32::MAX`, the most a `ProtoStr` holds.
    #[inline]
    pub fn new(bytes: &'a [u8]) -> Self {
        assert!(
            u32::try_from(bytes.len()).is_ok(),
            "a ProtoStr holds at most u32::MAX bytes"
        );
        Self::new_truncating(bytes)
    }

    // A decode never reads more than `u32::MAX` bytes, since it rejects a longer input, so generated
    // code skips the check of `new`.
    #[doc(hidden)]
    #[inline]
    pub fn new_truncating(bytes: &'a [u8]) -> Self {
        Self(PackedSlice::new(bytes))
    }

    /// The raw bytes, unchanged. Free.
    #[inline]
    pub fn as_bytes(self) -> &'a [u8] {
        self.0.as_slice()
    }

    /// Length in bytes.
    #[inline]
    pub fn len(self) -> usize {
        self.as_bytes().len()
    }

    /// Whether the string is empty.
    #[inline]
    pub fn is_empty(self) -> bool {
        self.as_bytes().is_empty()
    }

    /// Validate as UTF-8.
    #[inline]
    pub fn to_str(self) -> Result<&'a str, core::str::Utf8Error> {
        core::str::from_utf8(self.as_bytes())
    }

    /// Validate as UTF-8, substituting U+FFFD for each invalid sequence. Borrows when the bytes are
    /// valid, and allocates the repaired copy otherwise.
    #[cfg(feature = "alloc")]
    #[inline]
    pub fn to_str_lossy(self) -> alloc::borrow::Cow<'a, str> {
        alloc::string::String::from_utf8_lossy(self.as_bytes())
    }
}

impl Default for ProtoStr<'_> {
    #[inline]
    fn default() -> Self {
        Self::new_truncating(&[])
    }
}

impl fmt::Display for ProtoStr<'_> {
    /// Writes the text, substituting U+FFFD for each invalid sequence. Allocation-free.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for chunk in self.as_bytes().utf8_chunks() {
            f.write_str(chunk.valid())?;
            if !chunk.invalid().is_empty() {
                f.write_char(char::REPLACEMENT_CHARACTER)?;
            }
        }
        Ok(())
    }
}

impl fmt::Debug for ProtoStr<'_> {
    /// Valid UTF-8 prints as a `str` does. Otherwise the bytes print as `ProtoStr(b"a\xffb")`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.to_str() {
            Ok(s) => fmt::Debug::fmt(s, f),
            Err(_) => write!(
                f,
                "ProtoStr({:?})",
                ProtoBytes::new_truncating(self.as_bytes())
            ),
        }
    }
}

impl AsRef<[u8]> for ProtoStr<'_> {
    #[inline]
    fn as_ref(&self) -> &[u8] {
        self.as_bytes()
    }
}

// Comparison, ordering and hashing are the bytes'.
impl PartialEq for ProtoStr<'_> {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}
impl Eq for ProtoStr<'_> {}

impl PartialOrd for ProtoStr<'_> {
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for ProtoStr<'_> {
    #[inline]
    fn cmp(&self, other: &Self) -> Ordering {
        self.as_bytes().cmp(other.as_bytes())
    }
}

impl Hash for ProtoStr<'_> {
    #[inline]
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.as_bytes().hash(state);
    }
}

impl PartialEq<str> for ProtoStr<'_> {
    #[inline]
    fn eq(&self, other: &str) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}
impl PartialEq<&str> for ProtoStr<'_> {
    #[inline]
    fn eq(&self, other: &&str) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}
impl PartialEq<ProtoStr<'_>> for str {
    #[inline]
    fn eq(&self, other: &ProtoStr<'_>) -> bool {
        other == self
    }
}
impl PartialEq<ProtoStr<'_>> for &str {
    #[inline]
    fn eq(&self, other: &ProtoStr<'_>) -> bool {
        other == self
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    extern crate alloc;
    use alloc::format;
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
        let s = ProtoStr::new(b"a\xFFb\xE2\x82");
        assert!(s.to_str().is_err());
        assert_eq!(s.as_bytes(), b"a\xFFb\xE2\x82");
        // One U+FFFD per invalid sequence, as `String::from_utf8_lossy` writes it.
        assert_eq!(s.to_string(), "a\u{FFFD}b\u{FFFD}");
        #[cfg(feature = "alloc")]
        assert_eq!(s.to_string(), s.to_str_lossy());
        assert_eq!(format!("{s:?}"), r#"ProtoStr(b"a\xffb\xe2\x82")"#);
    }

    #[test]
    fn empty() {
        let s = ProtoStr::new(b"");
        assert!(s.is_empty());
        assert_eq!(s.to_string(), "");
        assert_eq!(ProtoStr::default(), s);
    }

    #[test]
    fn compares_with_str_in_either_order() {
        let (a, b) = (ProtoStr::new(b"a\xFFz"), ProtoStr::new(b"b"));
        assert_eq!(b, "b");
        assert_eq!("b", b);
        assert_eq!(*"b", b);
        assert_eq!(b, *"b");
        assert_ne!(a, b);
        assert!(a < b);
        assert_eq!(format!("{b:?}"), "\"b\"");
    }

    // Covariant like `&'a str`: `Owned`'s safety contract relies on decoded messages being covariant.
    fn _covariant<'a: 'b, 'b>(s: ProtoStr<'a>) -> ProtoStr<'b> {
        s
    }

    // A copy hands out borrows for the full `'a`.
    fn _outlives_the_copy<'a>(strings: &[ProtoStr<'a>]) -> &'a str {
        strings[0].to_str().unwrap_or("")
    }
}
