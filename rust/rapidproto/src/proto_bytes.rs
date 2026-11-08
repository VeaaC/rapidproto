//! A protobuf `bytes` value.

use core::borrow::Borrow;
use core::cmp::Ordering;
use core::fmt;
use core::hash::{Hash, Hasher};
use core::ops::Deref;

use crate::PackedSlice;

/// A protobuf `bytes` value, borrowed for `'a`.
///
/// It derefs to `[u8]` and compares with byte slices and arrays, as in `blob == b"\x01\x02"`. A slice
/// reached through deref lives only as long as the borrow of the `ProtoBytes`;
/// [`as_slice`](ProtoBytes::as_slice) returns it for the full `'a`. `Debug` prints a byte string
/// literal, such as `b"a\xff"`. `Default` is the empty value.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct ProtoBytes<'a>(PackedSlice<'a, u8>);

impl<'a> ProtoBytes<'a> {
    /// Wrap `bytes`, without copying them. Use it for a `ProtoBytes` that does not come from decoding,
    /// such as a lookup key or a test value.
    ///
    /// # Panics
    ///
    /// If `bytes` is longer than `u32::MAX`, the most a `ProtoBytes` holds.
    #[inline]
    pub fn new(bytes: &'a [u8]) -> Self {
        assert!(
            u32::try_from(bytes.len()).is_ok(),
            "a ProtoBytes holds at most u32::MAX bytes"
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

    /// The bytes, for the full `'a`.
    #[inline]
    pub fn as_slice(self) -> &'a [u8] {
        self.0.as_slice()
    }
}

impl Default for ProtoBytes<'_> {
    #[inline]
    fn default() -> Self {
        Self::new_truncating(&[])
    }
}

impl Deref for ProtoBytes<'_> {
    type Target = [u8];
    #[inline]
    fn deref(&self) -> &[u8] {
        self.as_slice()
    }
}

impl AsRef<[u8]> for ProtoBytes<'_> {
    #[inline]
    fn as_ref(&self) -> &[u8] {
        self.as_slice()
    }
}

impl Borrow<[u8]> for ProtoBytes<'_> {
    #[inline]
    fn borrow(&self) -> &[u8] {
        self.as_slice()
    }
}

impl<'a> IntoIterator for ProtoBytes<'a> {
    type Item = &'a u8;
    type IntoIter = core::slice::Iter<'a, u8>;
    #[inline]
    fn into_iter(self) -> Self::IntoIter {
        self.as_slice().iter()
    }
}

impl<'a> IntoIterator for &ProtoBytes<'a> {
    type Item = &'a u8;
    type IntoIter = core::slice::Iter<'a, u8>;
    #[inline]
    fn into_iter(self) -> Self::IntoIter {
        self.as_slice().iter()
    }
}

impl fmt::Debug for ProtoBytes<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "b\"{}\"", self.as_slice().escape_ascii())
    }
}

// Comparison, ordering and hashing are the slice's, so `Borrow<[u8]>` keys look up consistently.
impl PartialEq for ProtoBytes<'_> {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}
impl Eq for ProtoBytes<'_> {}

impl PartialOrd for ProtoBytes<'_> {
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for ProtoBytes<'_> {
    #[inline]
    fn cmp(&self, other: &Self) -> Ordering {
        self.as_slice().cmp(other.as_slice())
    }
}

impl Hash for ProtoBytes<'_> {
    #[inline]
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.as_slice().hash(state);
    }
}

impl PartialEq<[u8]> for ProtoBytes<'_> {
    #[inline]
    fn eq(&self, other: &[u8]) -> bool {
        self.as_slice() == other
    }
}
impl PartialEq<&[u8]> for ProtoBytes<'_> {
    #[inline]
    fn eq(&self, other: &&[u8]) -> bool {
        self.as_slice() == *other
    }
}
impl<const N: usize> PartialEq<[u8; N]> for ProtoBytes<'_> {
    #[inline]
    fn eq(&self, other: &[u8; N]) -> bool {
        self.as_slice() == other
    }
}
impl<const N: usize> PartialEq<&[u8; N]> for ProtoBytes<'_> {
    #[inline]
    fn eq(&self, other: &&[u8; N]) -> bool {
        self.as_slice() == *other
    }
}
impl PartialEq<ProtoBytes<'_>> for [u8] {
    #[inline]
    fn eq(&self, other: &ProtoBytes<'_>) -> bool {
        self == other.as_slice()
    }
}
impl PartialEq<ProtoBytes<'_>> for &[u8] {
    #[inline]
    fn eq(&self, other: &ProtoBytes<'_>) -> bool {
        *self == other.as_slice()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    extern crate alloc;
    use alloc::format;

    #[test]
    fn derefs_and_compares_as_the_slice() {
        let (a, b) = (ProtoBytes::new(b"\x01"), ProtoBytes::new(b"\x02\x03"));
        assert_eq!(b.len(), 2);
        assert_eq!(b[0], 2);
        assert_eq!(b.get(1), Some(&3));
        assert_eq!(b, [2u8, 3]);
        assert_eq!(b, b"\x02\x03");
        assert_eq!(b, &[2u8, 3][..]);
        assert_eq!(&[2u8, 3][..], b);
        assert!(a < b);
        assert_eq!(b.into_iter().sum::<u8>(), 5);
        assert!(ProtoBytes::default().is_empty());
    }

    #[test]
    fn debug_prints_a_byte_string_literal() {
        let b = ProtoBytes::new(b"a\xff\"\\\n");
        assert_eq!(format!("{b:?}"), r#"b"a\xff\"\\\n""#);
    }

    // Covariant like `&'a [u8]`: `Owned`'s safety contract relies on decoded messages being covariant.
    fn _covariant<'a: 'b, 'b>(b: ProtoBytes<'a>) -> ProtoBytes<'b> {
        b
    }

    // `as_slice` keeps the full `'a` where a deref would end with the local copy.
    fn _as_slice_outlives_the_copy<'a>(values: &[ProtoBytes<'a>]) -> &'a [u8] {
        let value = values[0];
        value.as_slice()
    }
}
