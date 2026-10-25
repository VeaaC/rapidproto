//! A borrowed slice stored in 12 bytes.

use core::fmt;
use core::marker::PhantomData;
use core::ptr::NonNull;

/// A `&'a [T]` stored as its pointer and a `u32` length: 12 bytes with alignment 4, where a slice
/// reference takes 16 with alignment 8. Decoded messages store their repeated fields, `bytes` fields,
/// strings ([`ProtoStr`](crate::ProtoStr)) and maps ([`MapView`](crate::MapView)) this way, as the
/// C++ decoder stores its views, so a message's arena footprint matches C++. The accessors hand back
/// the plain slice via [`get`](PackedSlice::get).
///
/// The length is 32-bit because nothing a decode produces can exceed it: `decode` rejects an input
/// over `u32::MAX` bytes, and every view or element count is bounded by the input.
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
    /// Store `slice`. A slice longer than `u32::MAX` elements keeps only the low 32 bits of its length:
    /// a shorter view of the same memory, never out of bounds (debug builds assert against it).
    #[inline]
    pub fn new(slice: &'a [T]) -> Self {
        debug_assert!(
            u32::try_from(slice.len()).is_ok(),
            "PackedSlice longer than u32::MAX"
        );
        Self {
            ptr: NonNull::from(slice).cast(),
            // Truncates rather than checks: a check costs a branch on every view a decode stores (up
            // to 9% more instructions on string-heavy messages), for a length no decode can produce.
            len: slice.len() as u32,
            _marker: PhantomData,
        }
    }

    /// The stored slice.
    #[inline]
    pub fn get(self) -> &'a [T] {
        // Copy the fields out first: a reference to a field of a packed struct may be misaligned.
        let (ptr, len) = (self.hint_nonnull().ptr, self.len);
        // SAFETY: `ptr` and `len` came from a `&'a [T]` (or are a dangling pointer with length 0),
        // and `PhantomData<&'a [T]>` keeps that borrow alive for `'a`.
        unsafe { core::slice::from_raw_parts(ptr.as_ptr(), len as usize) }
    }

    /// This view, unchanged, with its pointer's non-null invariant restated to the optimizer. A read
    /// out of a packed field loses that fact, so an `Option` of the view (or of the slice it hands
    /// back) would test the pointer again; with the hint it tests only what built the `Option`.
    #[doc(hidden)]
    #[inline(always)]
    pub fn hint_nonnull(self) -> Self {
        let ptr = self.ptr;
        // The lint knows `ptr` is non-null from its type; the optimizer, after the packed read, does
        // not, and this tells it.
        #[allow(useless_ptr_null_checks)]
        // SAFETY: `ptr` is a `NonNull`; this only restates its invariant.
        unsafe {
            core::hint::assert_unchecked(!ptr.as_ptr().is_null());
        }
        self
    }
}

impl<T> Default for PackedSlice<'_, T> {
    /// An empty view.
    #[inline]
    fn default() -> Self {
        Self {
            ptr: NonNull::dangling(),
            len: 0,
            _marker: PhantomData,
        }
    }
}

impl<T: fmt::Debug> fmt::Debug for PackedSlice<'_, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.get().fmt(f)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout_is_twelve_bytes_align_four() {
        assert_eq!(core::mem::size_of::<PackedSlice<'_, u64>>(), 12);
        assert_eq!(core::mem::align_of::<PackedSlice<'_, u64>>(), 4);
    }

    #[test]
    fn round_trips_the_slice() {
        let data = [1u64, 2, 3];
        let s = PackedSlice::new(&data[1..]);
        assert_eq!(s.get(), &[2, 3]);
        assert_eq!(s.get().as_ptr(), data[1..].as_ptr());
        assert!(PackedSlice::<u64>::default().get().is_empty());
    }
}
