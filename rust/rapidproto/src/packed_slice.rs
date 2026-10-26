//! A borrowed slice stored in 12 bytes.

use core::borrow::Borrow;
use core::cmp::Ordering;
use core::fmt;
use core::hash::{Hash, Hasher};
use core::marker::PhantomData;
use core::ops::Deref;
use core::ptr::NonNull;

/// A `&'a [T]` stored as its pointer and a `u32` length: on a 64-bit target 12 bytes with alignment
/// 4, where a slice reference takes 16 with alignment 8. Decoded messages store their repeated
/// fields, maps and `bytes` values this way, as the C++ decoder stores its views. A `bytes` value
/// inside a repeated field, map or oneof, and a repeated raw payload, is a `PackedSlice<'a, u8>`.
///
/// It derefs to the slice. Deref borrows the cell, so a slice reached through it lives only as long
/// as that borrow; [`as_slice`](PackedSlice::as_slice) returns it for the full `'a`.
#[repr(C, packed(4))]
pub struct PackedSlice<'a, T> {
    ptr: NonNull<T>,
    len: u32,
    _marker: PhantomData<&'a [T]>,
}

// Manual `Copy`/`Clone` so the impls don't require `T: Copy` (the view only borrows).
impl<T> Clone for PackedSlice<'_, T> {
    #[inline]
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for PackedSlice<'_, T> {}

// SAFETY: a `PackedSlice<'a, T>` is a `&'a [T]` in another layout, so it may cross or be shared
// between threads exactly when the slice reference may: when `T: Sync`.
unsafe impl<T: Sync> Send for PackedSlice<'_, T> {}
unsafe impl<T: Sync> Sync for PackedSlice<'_, T> {}

impl<'a, T> PackedSlice<'a, T> {
    /// Store `slice`. The length truncates to 32 bits rather than being checked: a check costs a
    /// branch on every view a decode stores (up to 9% more instructions on string-heavy messages),
    /// and the arena decoder rejects an input over `u32::MAX` bytes, so nothing it stores is
    /// longer. A debug build asserts that.
    #[doc(hidden)]
    #[inline]
    pub fn new(slice: &'a [T]) -> Self {
        debug_assert!(u32::try_from(slice.len()).is_ok());
        Self {
            ptr: NonNull::from(slice).cast(),
            len: slice.len() as u32,
            _marker: PhantomData,
        }
    }

    /// The stored slice, for the full `'a`.
    #[inline]
    pub fn as_slice(self) -> &'a [T] {
        // Copy the fields out first: a reference to a field of a packed struct may be misaligned.
        let (ptr, len) = (self.ptr, self.len);
        // A read out of a packed field loses the fact that `ptr` is non-null, so an `Option` of the
        // slice would test the pointer again. This restates it; the lint knows it from the type.
        #[allow(useless_ptr_null_checks)]
        // SAFETY: `ptr` is a `NonNull`; this only restates its invariant.
        unsafe {
            core::hint::assert_unchecked(!ptr.as_ptr().is_null());
        }
        // SAFETY: `ptr` came from a `&'a [T]` and `len` is at most that slice's length (`new` only
        // ever truncates it), and `PhantomData<&'a [T]>` keeps the borrow alive for `'a`.
        unsafe { core::slice::from_raw_parts(ptr.as_ptr(), len as usize) }
    }
}

impl<T> Deref for PackedSlice<'_, T> {
    type Target = [T];
    #[inline]
    fn deref(&self) -> &[T] {
        self.as_slice()
    }
}

impl<T> AsRef<[T]> for PackedSlice<'_, T> {
    #[inline]
    fn as_ref(&self) -> &[T] {
        self
    }
}

impl<T> Borrow<[T]> for PackedSlice<'_, T> {
    #[inline]
    fn borrow(&self) -> &[T] {
        self
    }
}

impl<'a, T> IntoIterator for PackedSlice<'a, T> {
    type Item = &'a T;
    type IntoIter = core::slice::Iter<'a, T>;
    #[inline]
    fn into_iter(self) -> Self::IntoIter {
        self.as_slice().iter()
    }
}

impl<'a, T> IntoIterator for &PackedSlice<'a, T> {
    type Item = &'a T;
    type IntoIter = core::slice::Iter<'a, T>;
    #[inline]
    fn into_iter(self) -> Self::IntoIter {
        self.as_slice().iter()
    }
}

impl<T: fmt::Debug> fmt::Debug for PackedSlice<'_, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.as_slice().fmt(f)
    }
}

// Comparison, ordering and hashing are the slice's, so `Borrow<[T]>` keys look up consistently.
impl<T: PartialEq<U>, U> PartialEq<PackedSlice<'_, U>> for PackedSlice<'_, T> {
    #[inline]
    fn eq(&self, other: &PackedSlice<'_, U>) -> bool {
        self.as_slice() == other.as_slice()
    }
}
impl<T: Eq> Eq for PackedSlice<'_, T> {}

impl<T: PartialOrd> PartialOrd for PackedSlice<'_, T> {
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        self.as_slice().partial_cmp(other.as_slice())
    }
}
impl<T: Ord> Ord for PackedSlice<'_, T> {
    #[inline]
    fn cmp(&self, other: &Self) -> Ordering {
        self.as_slice().cmp(other.as_slice())
    }
}

impl<T: Hash> Hash for PackedSlice<'_, T> {
    #[inline]
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.as_slice().hash(state);
    }
}

impl<T: PartialEq<U>, U> PartialEq<[U]> for PackedSlice<'_, T> {
    #[inline]
    fn eq(&self, other: &[U]) -> bool {
        self.as_slice() == other
    }
}
impl<T: PartialEq<U>, U> PartialEq<&[U]> for PackedSlice<'_, T> {
    #[inline]
    fn eq(&self, other: &&[U]) -> bool {
        self.as_slice() == *other
    }
}
impl<T: PartialEq<U>, U, const N: usize> PartialEq<[U; N]> for PackedSlice<'_, T> {
    #[inline]
    fn eq(&self, other: &[U; N]) -> bool {
        self.as_slice() == other
    }
}
impl<T: PartialEq<U>, U, const N: usize> PartialEq<&[U; N]> for PackedSlice<'_, T> {
    #[inline]
    fn eq(&self, other: &&[U; N]) -> bool {
        self.as_slice() == *other
    }
}
impl<T: PartialEq<U>, U> PartialEq<PackedSlice<'_, U>> for [T] {
    #[inline]
    fn eq(&self, other: &PackedSlice<'_, U>) -> bool {
        self == other.as_slice()
    }
}
impl<T: PartialEq<U>, U> PartialEq<PackedSlice<'_, U>> for &[T] {
    #[inline]
    fn eq(&self, other: &PackedSlice<'_, U>) -> bool {
        *self == other.as_slice()
    }
}

#[cfg(test)]
mod tests {
    use core::mem::{align_of, size_of};

    use super::*;
    use crate::{MapView, PackedStr};

    // The sizes the packed layout exists for. `Option` of a cell keeps the pointer niche.
    #[cfg(target_pointer_width = "64")]
    const _: () = assert!(
        size_of::<PackedSlice<'_, u64>>() == 12
            && align_of::<PackedSlice<'_, u64>>() == 4
            && size_of::<Option<PackedSlice<'_, u8>>>() == 12
            && size_of::<PackedStr<'_>>() == 12
            && size_of::<Option<PackedStr<'_>>>() == 12
            && size_of::<MapView<'_, u8, u8>>() == 12
    );

    // Covariant like `&'a [T]`: `Owned`'s safety contract relies on decoded messages being covariant.
    fn _covariant<'a: 'b, 'b>(s: PackedSlice<'a, &'a u8>) -> PackedSlice<'b, &'b u8> {
        s
    }

    #[test]
    fn round_trips_the_slice() {
        let data = [1u64, 2, 3];
        let s = PackedSlice::new(&data[1..]);
        assert_eq!(s.as_slice(), &[2, 3]);
        assert_eq!(s.as_slice().as_ptr(), data[1..].as_ptr());
        assert!(PackedSlice::new(&data[..0]).as_slice().is_empty());
    }

    #[test]
    fn derefs_and_compares_as_the_slice() {
        let data = [1u8, 2, 3];
        let cells = [PackedSlice::new(&data[..1]), PackedSlice::new(&data[1..])];
        assert_eq!(cells[1].len(), 2);
        assert_eq!(cells[1][0], 2);
        assert_eq!(cells[1].get(1), Some(&3));
        assert_eq!(cells, [&[1u8][..], &[2, 3]]);
        assert_eq!(cells[1], [2u8, 3]);
        assert_eq!(&[2u8, 3][..], cells[1]);
        assert!(cells[0] < cells[1]);
        assert_eq!(cells[1].into_iter().sum::<u8>(), 5);
    }

    #[test]
    fn under_aligned_cells_keep_an_aligned_pointer() {
        // Only the 12-byte cell is 4-aligned; the slice it holds keeps its element's alignment, at
        // either cell position.
        #[repr(align(16))]
        #[derive(Debug, PartialEq)]
        struct Wide(u8);
        let data = [Wide(1), Wide(2)];
        let cells = [PackedSlice::new(&data[..1]), PackedSlice::new(&data[1..])];
        for (cell, want) in cells.iter().zip([1u8, 2]) {
            let s: &[Wide] = cell;
            assert_eq!(s.as_ptr().addr() % 16, 0);
            assert_eq!(s, &[Wide(want)]);
        }
    }
}
