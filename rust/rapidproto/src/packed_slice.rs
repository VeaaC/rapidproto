//! A borrowed slice stored in 12 bytes.

use core::fmt;
use core::marker::PhantomData;
use core::ptr::NonNull;

/// A `&'a [T]` stored as its pointer and a `u32` length: on a 64-bit target 12 bytes with alignment
/// 4, where a slice reference takes 16 with alignment 8. Decoded messages store their strings
/// ([`ProtoStr`](crate::ProtoStr)), maps ([`MapView`](crate::MapView)), `bytes` fields and repeated
/// fields (other than raw ones) this way, as the C++ decoder stores its views. The accessors hand back
/// the plain slice via [`get`](PackedSlice::get).
///
/// The length is 32-bit, as in C++: the arena decoder rejects an input over `u32::MAX` bytes, so no
/// view or element count it produces can exceed it. The streaming decoder takes any input, so a
/// string over `u32::MAX` bytes (never valid protobuf, which caps a message at 2 GiB) reaches its
/// visitor with only the low 32 bits of its length: shorter, never out of bounds.
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
    /// a shorter view of the same memory, never out of bounds.
    #[inline]
    pub fn new(slice: &'a [T]) -> Self {
        Self {
            ptr: NonNull::from(slice).cast(),
            // Truncates rather than checks: a check costs a branch on every view a decode stores (up
            // to 9% more instructions on string-heavy messages), for a length no valid input has.
            len: slice.len() as u32,
            _marker: PhantomData,
        }
    }

    /// The stored slice.
    #[inline]
    pub fn get(self) -> &'a [T] {
        // Copy the fields out first: a reference to a field of a packed struct may be misaligned.
        let (ptr, len) = (self.hint_nonnull().ptr, self.len);
        // SAFETY: `ptr` came from a `&'a [T]` and `len` is at most that slice's length (`new` only
        // ever truncates it), and `PhantomData<&'a [T]>` keeps the borrow alive for `'a`.
        unsafe { core::slice::from_raw_parts(ptr.as_ptr(), len as usize) }
    }

    /// This view, unchanged, with its pointer's non-null invariant restated to the optimizer. A read
    /// out of a packed field loses that fact, so an `Option` of the view (or of the slice it hands
    /// back) would test the pointer again; with the hint it tests only what built the `Option`.
    #[inline(always)]
    pub(crate) fn hint_nonnull(self) -> Self {
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

impl<T: fmt::Debug> fmt::Debug for PackedSlice<'_, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.get().fmt(f)
    }
}

#[cfg(test)]
mod tests {
    use core::mem::{align_of, size_of};

    use super::*;
    use crate::{MapView, ProtoStr};

    // The sizes the packed layout exists for. `Option<ProtoStr>` keeps the pointer niche.
    #[cfg(target_pointer_width = "64")]
    const _: () = assert!(
        size_of::<PackedSlice<'_, u64>>() == 12
            && align_of::<PackedSlice<'_, u64>>() == 4
            && size_of::<ProtoStr<'_>>() == 12
            && size_of::<Option<ProtoStr<'_>>>() == 12
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
        assert_eq!(s.get(), &[2, 3]);
        assert_eq!(s.get().as_ptr(), data[1..].as_ptr());
        assert!(PackedSlice::new(&data[..0]).get().is_empty());
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
            let s = cell.get();
            assert_eq!(s.as_ptr().addr() % 16, 0);
            assert_eq!(s, &[Wide(want)]);
        }
    }
}
