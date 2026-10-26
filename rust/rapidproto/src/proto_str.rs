//! A borrowed `string` field, and the 12-byte cell decoded messages store one in.

use core::borrow::Borrow;
use core::cmp::Ordering;
use core::fmt::{self, Write as _};
use core::hash::{Hash, Hasher};
use core::ops::Deref;

use crate::PackedSlice;

/// A protobuf `string`: its raw UTF-8 bytes, with validation left to the caller. Used as
/// `&'a ProtoStr`, as `str` is used as `&str`.
///
/// Decoding does not validate (matching the C++ runtime, which trusts the schema), so a `&ProtoStr`
/// always borrows the input directly and costs nothing to produce. The caller picks the cost it
/// wants: [`as_bytes`](ProtoStr::as_bytes) is free, [`to_str`](ProtoStr::to_str) validates and can
/// fail, and [`to_str_lossy`](ProtoStr::to_str_lossy) (with `alloc`) never fails. `Display` writes
/// the text, substituting U+FFFD for any invalid sequence. Proto `bytes` fields are `&[u8]`
/// directly and do not use this type.
#[derive(PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct ProtoStr([u8]);

impl ProtoStr {
    /// View raw bytes as a string (no validation).
    #[inline]
    pub fn new(bytes: &[u8]) -> &ProtoStr {
        // SAFETY: `ProtoStr` is a `repr(transparent)` wrapper of `[u8]`, so both references have
        // the same layout and length, and the borrow carries over.
        unsafe { &*(bytes as *const [u8] as *const ProtoStr) }
    }

    /// The raw bytes, unchanged. Free.
    #[inline]
    pub fn as_bytes(&self) -> &[u8] {
        &self.0
    }

    /// Length in bytes.
    #[inline]
    pub fn len(&self) -> usize {
        self.0.len()
    }

    /// Whether the field is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// Validate as UTF-8, borrowing the input on success.
    #[inline]
    pub fn to_str(&self) -> Result<&str, core::str::Utf8Error> {
        core::str::from_utf8(&self.0)
    }

    /// Validate as UTF-8, substituting U+FFFD for invalid sequences. Borrows the input when it is
    /// already valid, otherwise allocates the repaired copy.
    #[cfg(feature = "alloc")]
    #[inline]
    pub fn to_str_lossy(&self) -> alloc::borrow::Cow<'_, str> {
        // `from_utf8_lossy` already borrows for the valid case and owns the repaired copy
        // otherwise, in a single scan.
        alloc::string::String::from_utf8_lossy(&self.0)
    }
}

impl fmt::Display for ProtoStr {
    /// Writes the text, substituting U+FFFD for each invalid sequence. Allocation-free.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut rem = &self.0;
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

impl fmt::Debug for ProtoStr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Valid UTF-8 → `str`'s Debug (proper escaping of quotes/newlines/control bytes); otherwise
        // show the raw bytes so invalid content is unambiguous.
        match self.to_str() {
            Ok(s) => fmt::Debug::fmt(s, f),
            Err(_) => write!(f, "ProtoStr({:?})", &self.0),
        }
    }
}

impl PartialEq<str> for ProtoStr {
    #[inline]
    fn eq(&self, other: &str) -> bool {
        self.0 == *other.as_bytes()
    }
}

impl PartialEq<ProtoStr> for str {
    #[inline]
    fn eq(&self, other: &ProtoStr) -> bool {
        *self.as_bytes() == other.0
    }
}

impl AsRef<[u8]> for ProtoStr {
    #[inline]
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

impl AsRef<ProtoStr> for ProtoStr {
    #[inline]
    fn as_ref(&self) -> &ProtoStr {
        self
    }
}

/// A `string` as decoded messages store it, in 12 bytes (see [`PackedSlice`]). A repeated `string`
/// field hands out its elements as `PackedStr`, as do `string` map keys and values and `string`
/// oneof members.
///
/// It derefs to [`ProtoStr`]. Deref borrows the cell, so a string reached through it lives only as
/// long as that borrow; [`get`](PackedStr::get) returns it for the full `'a`.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct PackedStr<'a>(PackedSlice<'a, u8>);

impl<'a> PackedStr<'a> {
    /// Store raw bytes (no validation). The length truncates to 32 bits, as in [`PackedSlice`].
    #[doc(hidden)]
    #[inline]
    pub fn new(bytes: &'a [u8]) -> Self {
        Self(PackedSlice::new(bytes))
    }

    /// The stored string, for the full `'a`.
    #[inline]
    pub fn get(self) -> &'a ProtoStr {
        ProtoStr::new(self.0.as_slice())
    }
}

impl Deref for PackedStr<'_> {
    type Target = ProtoStr;
    #[inline]
    fn deref(&self) -> &ProtoStr {
        (*self).get()
    }
}

impl AsRef<ProtoStr> for PackedStr<'_> {
    #[inline]
    fn as_ref(&self) -> &ProtoStr {
        self
    }
}

impl AsRef<[u8]> for PackedStr<'_> {
    #[inline]
    fn as_ref(&self) -> &[u8] {
        self.as_bytes()
    }
}

impl Borrow<ProtoStr> for PackedStr<'_> {
    #[inline]
    fn borrow(&self) -> &ProtoStr {
        self
    }
}

impl fmt::Display for PackedStr<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&**self, f)
    }
}

impl fmt::Debug for PackedStr<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(&**self, f)
    }
}

// Comparison, ordering and hashing are the `ProtoStr`'s, so `Borrow<ProtoStr>` keys look up
// consistently.
impl PartialEq for PackedStr<'_> {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        **self == **other
    }
}
impl Eq for PackedStr<'_> {}

impl PartialOrd for PackedStr<'_> {
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for PackedStr<'_> {
    #[inline]
    fn cmp(&self, other: &Self) -> Ordering {
        (**self).cmp(&**other)
    }
}

impl Hash for PackedStr<'_> {
    #[inline]
    fn hash<H: Hasher>(&self, state: &mut H) {
        (**self).hash(state);
    }
}

impl PartialEq<ProtoStr> for PackedStr<'_> {
    #[inline]
    fn eq(&self, other: &ProtoStr) -> bool {
        **self == *other
    }
}

impl PartialEq<str> for PackedStr<'_> {
    #[inline]
    fn eq(&self, other: &str) -> bool {
        **self == *other
    }
}

impl PartialEq<&str> for PackedStr<'_> {
    #[inline]
    fn eq(&self, other: &&str) -> bool {
        **self == **other
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

    #[test]
    fn cells_deref_and_compare_as_the_string() {
        let cells = [PackedStr::new(b"b"), PackedStr::new(b"a\xFFz")];
        assert_eq!(cells[0], "b");
        assert_eq!(&cells[0], "b");
        assert_eq!(cells[0], *ProtoStr::new(b"b"));
        assert_eq!(cells[1].len(), 3);
        assert!(cells[1] < cells[0]);
        assert_eq!(cells[1].to_string(), "a\u{FFFD}z");
        assert_eq!(alloc::format!("{:?}", cells[0]), "\"b\"");
        let s: &ProtoStr = &cells[0];
        assert_eq!(s.as_bytes(), b"b");
    }

    // Covariant like `&'a ProtoStr`: `Owned`'s safety contract relies on decoded messages being
    // covariant.
    fn _covariant<'a: 'b, 'b>(s: PackedStr<'a>) -> PackedStr<'b> {
        s
    }

    // `get` keeps the cell's lifetime where a deref would end with the local copy.
    fn _get_outlives_the_copy<'a>(cells: &[PackedStr<'a>]) -> &'a [u8] {
        let cell = cells[0];
        cell.get().as_bytes()
    }
}
