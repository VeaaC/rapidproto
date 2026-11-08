//! A borrowed slice stored in 12 bytes.

use core::marker::PhantomData;
use core::ptr::NonNull;

/// A `&'a [T]` stored as its pointer and a `u32` length: on a 64-bit target 12 bytes with alignment
/// 4, where a slice reference takes 16 with alignment 8. Decoded messages store their repeated fields
/// this way, and [`ProtoStr`](crate::ProtoStr), [`ProtoBytes`](crate::ProtoBytes) and
/// [`MapView`](crate::MapView) wrap one.
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

// SAFETY: a `PackedSlice<'a, T>` is a `&'a [T]` in another layout, so it may cross threads exactly
// when the slice reference may: when `T: Sync`.
unsafe impl<T: Sync> Send for PackedSlice<'_, T> {}
// SAFETY: as for `Send`, it may be shared between threads exactly when `&'a [T]` may.
unsafe impl<T: Sync> Sync for PackedSlice<'_, T> {}

impl<'a, T> PackedSlice<'a, T> {
    /// Store `slice`. The length truncates to 32 bits rather than being checked: a check costs a
    /// branch on every view a decode stores (up to 9% more instructions on string-heavy messages),
    /// and both decoders reject an input over `u32::MAX` bytes, so nothing they store is longer. A
    /// debug build asserts that.
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
        // SAFETY: `ptr` came from a `&'a [T]` and `len` is at most that slice's length (`new` only
        // ever truncates it), and `PhantomData<&'a [T]>` keeps the borrow alive for `'a`.
        unsafe { core::slice::from_raw_parts(ptr.as_ptr(), len as usize) }
    }
}

#[cfg(test)]
mod tests {
    use core::mem::{align_of, size_of};

    use super::*;
    use crate::{MapView, ProtoBytes, ProtoStr};

    // The sizes the packed layout exists for. `Option` of a cell keeps the pointer niche.
    #[cfg(target_pointer_width = "64")]
    const _: () = assert!(
        size_of::<PackedSlice<'_, u64>>() == 12
            && align_of::<PackedSlice<'_, u64>>() == 4
            && size_of::<Option<PackedSlice<'_, u8>>>() == 12
            && size_of::<ProtoStr<'_>>() == 12
            && size_of::<Option<ProtoStr<'_>>>() == 12
            && size_of::<ProtoBytes<'_>>() == 12
            && size_of::<Option<ProtoBytes<'_>>>() == 12
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
    fn under_aligned_cells_keep_an_aligned_pointer() {
        // Only the 12-byte cell is 4-aligned; the slice it holds keeps its element's alignment, at
        // either cell position.
        #[repr(align(16))]
        #[derive(Debug, PartialEq)]
        struct Wide(u8);
        let data = [Wide(1), Wide(2)];
        let cells = [PackedSlice::new(&data[..1]), PackedSlice::new(&data[1..])];
        for (cell, want) in cells.iter().zip([1u8, 2]) {
            let s: &[Wide] = cell.as_slice();
            assert_eq!(s.as_ptr().addr() % 16, 0);
            assert_eq!(s, &[Wide(want)]);
        }
    }
}
