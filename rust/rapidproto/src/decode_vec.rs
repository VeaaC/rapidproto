//! A decode-time growable array backed by the arena, finished into a borrowed slice.
//!
//! Generated decoders use it to gather a repeated field in a single pass: elements are pushed as they
//! are read off the wire, then [`into_slice`](DecodeVec::into_slice) hands back the finished
//! `&'a [T]`. A grow allocates a larger arena array and copies the elements over; the old array is
//! abandoned (the arena never frees, matching the C++ decoder's growable repeated storage). This is
//! the only place the growth bookkeeping lives, so the generated code stays simple.

use core::ptr::{self, NonNull};

use crate::arena::{Arena, ArenaExhausted};

mod sealed {
    pub trait Sealed {}
}

/// A fixed-width scalar whose in-memory **little-endian** byte image is exactly its protobuf
/// `fixed32`/`fixed64` wire encoding: `u32`/`i32`/`f32` (4 bytes) and `u64`/`i64`/`f64` (8 bytes).
///
/// Sealed and implemented only for those six types. The bound lets
/// [`DecodeVec::extend_from_le_bytes`] bulk-copy a packed fixed span with one `memcpy`: every bit
/// pattern is a valid value for each of them, so copying raw bytes into the slots soundly
/// initialises them (no invalid-value UB, unlike `bool`/`char`/niche enums).
pub trait FixedLeScalar: Copy + sealed::Sealed {}

macro_rules! impl_fixed_le_scalar {
    ($($t:ty),* $(,)?) => {
        $(
            impl sealed::Sealed for $t {}
            impl FixedLeScalar for $t {}
        )*
    };
}
impl_fixed_le_scalar!(u32, i32, f32, u64, i64, f64);

/// The initial capacity on the first push, then geometric doubling (matching the C++ decoder's
/// start-4-double growth).
const FIRST_CAP: usize = 4;

/// A growable array of decoded `T` backed by `arena`, built during one field loop and finished with
/// [`into_slice`](DecodeVec::into_slice). `T` must be a plain decoded value with no `Drop` (every
/// generated message, scalar, enum, `ProtoStr` and `&[u8]` qualifies): a grow relocates the elements
/// with a bitwise copy and never runs their destructors, as the arena requires.
pub struct DecodeVec<'a, T> {
    arena: &'a Arena<'a>,
    ptr: NonNull<T>, // dangling while `cap == 0`
    len: usize,
    cap: usize,
}

impl<'a, T> DecodeVec<'a, T> {
    /// An empty accumulator; allocates nothing until the first [`push`](DecodeVec::push).
    #[inline]
    #[must_use]
    pub fn new(arena: &'a Arena<'a>) -> Self {
        Self {
            arena,
            ptr: NonNull::dangling(),
            len: 0,
            cap: 0,
        }
    }

    /// Append one element, growing the backing array if it is full. `Err(ArenaExhausted)` (never a
    /// panic) if the arena cannot grow.
    #[inline]
    pub fn push(&mut self, value: T) -> Result<(), ArenaExhausted> {
        if self.len == self.cap {
            self.grow()?;
        }
        // SAFETY: after `grow` (or when already non-full) `len < cap`, so `ptr + len` is the next
        // uninitialized slot inside the current array; writing initializes it.
        unsafe { self.ptr.as_ptr().add(self.len).write(value) };
        self.len += 1;
        Ok(())
    }

    /// Decode one element **in place** into the next slot -- no by-value construct-then-copy. Grow if
    /// full, hand the uninitialized tail slot to `f`, and publish it (`len += 1`) only if `f`
    /// succeeds. On `Err`, `len` is unchanged, so the half-written slot is abandoned and never read by
    /// [`into_slice`](Self::into_slice). The Rust equivalent of the C++ decoder's `rp_slot` +
    /// `rp_decode_into(*slot, ...)` for a repeated message element.
    #[inline]
    pub fn emplace<E: From<ArenaExhausted>>(
        &mut self,
        f: impl FnOnce(&mut core::mem::MaybeUninit<T>) -> Result<(), E>,
    ) -> Result<(), E> {
        if self.len == self.cap {
            self.grow()?;
        }
        // SAFETY: after `grow` (or when already non-full) `len < cap`, so `ptr + len` is the in-bounds
        // next slot; `MaybeUninit<T>` shares `T`'s layout, so viewing it as `&mut MaybeUninit<T>` lets
        // `f` initialize it without reading uninitialized memory.
        let slot = unsafe {
            &mut *self
                .ptr
                .as_ptr()
                .add(self.len)
                .cast::<core::mem::MaybeUninit<T>>()
        };
        f(slot)?;
        // `f` returned `Ok`, so the slot is initialized; publish it. (An error above returned without
        // touching `len`, so the partially-written slot stays out of the finished slice.)
        self.len += 1;
        Ok(())
    }

    /// Pre-size the backing array so it can hold at least `additional` more elements beyond the
    /// current length, in a SINGLE arena allocation (not the geometric doubling `push` uses).
    /// A no-op when there is already room. Packed decode calls this once from the span's element
    /// count before the fill loop, so the per-element `push`es never trigger a geometric regrow
    /// (mirrors the C++ decoder's `allocate_array` up-front reserve in `emit_packed_fill`).
    #[inline]
    pub fn reserve(&mut self, additional: usize) -> Result<(), ArenaExhausted> {
        let needed = self.len.checked_add(additional).ok_or(ArenaExhausted)?;
        if needed > self.cap {
            self.grow_to(needed)?;
        }
        Ok(())
    }

    /// Trim the backing array to its exact length, returning the unused tail to the arena.
    ///
    /// Packed decode pre-sizes to an upper bound (`reserve`), fills, then calls this to reclaim the
    /// over-estimate (matching the C++ decoder's `Arena::shrink_last` after a packed-varint fill).
    /// The arena can only reclaim while this array is still its most-recent allocation -- true right
    /// after a fill, which allocates nothing; otherwise this just resets the capacity. Afterwards
    /// `cap == len`, so a later `push` regrows (as the C++ decoder resets `rp_cap` to `rp_n`).
    #[inline]
    pub fn shrink_to_fit(&mut self) {
        if self.cap > self.len {
            let size = core::mem::size_of::<T>();
            if size != 0 {
                self.arena
                    .shrink_last(self.ptr.cast::<u8>(), self.cap * size, self.len * size);
            }
            self.cap = self.len;
        }
    }

    /// Pointer to the start of the reserved-but-uninitialized spare capacity (`ptr + len`). A decode
    /// kernel writes elements directly here after a [`reserve`](Self::reserve), then calls
    /// [`advance_len`](Self::advance_len) to publish them.
    #[inline]
    pub(crate) fn spare_capacity_ptr(&mut self) -> *mut T {
        // SAFETY: `len <= cap`, so `ptr + len` is within the allocation (one-past-end when full).
        // When `cap == 0` the pointer is dangling, but callers only use it after a `reserve` that
        // made room, i.e. `cap > len`.
        unsafe { self.ptr.as_ptr().add(self.len) }
    }

    /// Publish `additional` elements that were written into the spare capacity.
    ///
    /// # Safety
    /// The caller must have initialized exactly `additional` contiguous elements starting at
    /// [`spare_capacity_ptr`](Self::spare_capacity_ptr), with `len + additional <= cap`.
    #[inline]
    pub(crate) unsafe fn advance_len(&mut self, additional: usize) {
        debug_assert!(self.len + additional <= self.cap);
        self.len += additional;
    }

    #[cold]
    fn grow(&mut self) -> Result<(), ArenaExhausted> {
        let new_cap = if self.cap == 0 {
            FIRST_CAP
        } else {
            self.cap.checked_mul(2).ok_or(ArenaExhausted)?
        };
        self.grow_to(new_cap)
    }

    /// Relocate into a fresh arena array of exactly `new_cap` elements (`new_cap > cap`). The old
    /// array is abandoned (the arena never frees); elements move with a bitwise copy.
    fn grow_to(&mut self, new_cap: usize) -> Result<(), ArenaExhausted> {
        debug_assert!(new_cap > self.cap);
        let new = self.arena.alloc_array::<T>(new_cap)?;
        if self.len > 0 {
            // SAFETY: the old array holds `len` initialized `T`s and the new array has room for at
            // least `len`; the two arena allocations are distinct (non-overlapping). A bitwise copy
            // relocates the elements -- sound for a non-`Drop`, non-self-referential `T` (which the
            // arena already requires); the old copies are abandoned, never dropped.
            unsafe { ptr::copy_nonoverlapping(self.ptr.as_ptr(), new.as_ptr(), self.len) };
        }
        self.ptr = new;
        self.cap = new_cap;
        Ok(())
    }

    /// Number of elements pushed so far.
    #[inline]
    #[must_use]
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether nothing has been pushed.
    #[inline]
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Finish: the pushed elements as a borrowed slice tied to the arena. Empty when nothing was
    /// pushed (no allocation).
    #[inline]
    #[must_use]
    pub fn into_slice(self) -> &'a [T] {
        if self.len == 0 {
            return &[];
        }
        // SAFETY: `ptr` points at `len` contiguous, initialized `T`s in the arena (lifetime `'a`);
        // they are a fresh arena allocation nothing else aliases, and the arena outlives `'a`.
        unsafe { core::slice::from_raw_parts(self.ptr.as_ptr(), self.len) }
    }
}

impl<'a, T: FixedLeScalar> DecodeVec<'a, T> {
    /// Append the packed fixed-width elements whose little-endian wire image is `bytes`, as a single
    /// bulk copy. The generated decoder calls this only for a packed `fixed32`/`fixed64` field on a
    /// little-endian host, after checking `bytes.len()` is a whole multiple of `size_of::<T>()`; any
    /// trailing partial element is ignored (copied as whole elements only, so never out of bounds).
    ///
    /// This reserves the exact element count, copies the span into the array tail with one `memcpy`
    /// (on a little-endian host the span IS the array's native byte image -- protoc fills packed
    /// fixed arrays the same way), and never regrows. The exact reserve means no trim is needed.
    /// `Err(ArenaExhausted)` (never a panic) if the arena cannot grow.
    #[inline]
    pub fn extend_from_le_bytes(&mut self, bytes: &[u8]) -> Result<(), ArenaExhausted> {
        let size = core::mem::size_of::<T>();
        debug_assert!(size != 0 && bytes.len().is_multiple_of(size));
        let count = bytes.len() / size;
        if count == 0 {
            return Ok(());
        }
        self.reserve(count)?;
        // SAFETY: `reserve(count)` guaranteed room for `count` more `T` starting at `ptr + len`, and
        // the arena array is aligned for `T`. `T: FixedLeScalar` is a fixed-width scalar for which
        // every bit pattern is a valid value, so copying `count * size` input bytes into those slots
        // soundly initialises them (the caller guarantees a little-endian host, where the wire image
        // equals the native image). Source (the input span) and destination (a fresh arena array)
        // are distinct allocations, so they never overlap.
        unsafe {
            ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                self.ptr.as_ptr().add(self.len).cast::<u8>(),
                count * size,
            );
        }
        self.len += count;
        Ok(())
    }
}

// The tests build a heap `Arena`, so they need an allocator; gate them on `alloc` (the no-allocator
// tier has no `Arena::new`, matching the arena module's own tests).
#[cfg(all(test, feature = "alloc"))]
mod tests {
    use super::*;

    #[test]
    fn grows_and_preserves_order_across_reallocations() {
        let arena = Arena::new();
        let mut v = DecodeVec::<u32>::new(&arena);
        assert!(v.is_empty());
        // Push well past the initial capacity to force several geometric grows (4 -> 8 -> 16 -> ...).
        for i in 0..100u32 {
            v.push(i * 3).unwrap();
        }
        assert_eq!(v.len(), 100);
        let s = v.into_slice();
        assert_eq!(s.len(), 100);
        for (i, &x) in s.iter().enumerate() {
            assert_eq!(x, i as u32 * 3);
        }
    }

    #[test]
    fn empty_is_an_empty_slice_and_allocates_nothing() {
        let arena = Arena::new();
        let v = DecodeVec::<u64>::new(&arena);
        assert_eq!(v.into_slice(), &[] as &[u64]);
        assert_eq!(arena.bytes_used(), 0);
    }

    #[test]
    fn works_for_a_non_copy_relocatable_value() {
        // A move-only (non-Copy) value with no Drop, like a generated message struct: relocation on
        // grow must preserve it.
        #[derive(PartialEq, Debug)]
        struct Pair(u32, u64);
        let arena = Arena::new();
        let mut v = DecodeVec::<Pair>::new(&arena);
        for i in 0..20u32 {
            v.push(Pair(i, u64::from(i) << 40)).unwrap();
        }
        let s = v.into_slice();
        for (i, p) in s.iter().enumerate() {
            assert_eq!(*p, Pair(i as u32, (i as u64) << 40));
        }
    }

    #[test]
    fn reserve_pre_sizes_in_one_allocation_so_pushes_do_not_regrow() {
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        v.reserve(100).unwrap();
        // A single arena allocation of exactly 100 elements -- nothing abandoned yet.
        assert_eq!(arena.bytes_used(), 100 * 8);
        for i in 0..100u64 {
            v.push(i).unwrap();
        }
        // Filling the reserved capacity triggered no geometric regrow (no further allocation).
        assert_eq!(arena.bytes_used(), 100 * 8);
        let s = v.into_slice();
        assert_eq!(s.len(), 100);
        for (i, &x) in s.iter().enumerate() {
            assert_eq!(x, i as u64);
        }
    }

    #[test]
    fn shrink_to_fit_reclaims_the_unused_tail_and_keeps_contents() {
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        // Pre-size to an upper bound, fill fewer (the packed-varint over-estimate case).
        v.reserve(100).unwrap();
        for i in 0..10u64 {
            v.push(i * 7).unwrap();
        }
        assert_eq!(arena.bytes_used(), 100 * 8);
        v.shrink_to_fit();
        // The 90-element tail was returned to the arena; it is still the last allocation.
        assert_eq!(arena.bytes_used(), 10 * 8);
        // A subsequent push regrows (cap was reset to len) and preserves order.
        v.push(999).unwrap();
        let s = v.into_slice();
        assert_eq!(s.len(), 11);
        for (i, &x) in s.iter().take(10).enumerate() {
            assert_eq!(x, i as u64 * 7);
        }
        assert_eq!(s[10], 999);
    }

    #[test]
    fn shrink_to_fit_is_a_noop_when_not_the_last_allocation() {
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        v.reserve(100).unwrap();
        for i in 0..10u64 {
            v.push(i).unwrap();
        }
        // Something else becomes the arena's last allocation, so the tail can no longer be reclaimed.
        let _other = arena.alloc_array::<u64>(4).unwrap();
        let before = arena.bytes_used();
        v.shrink_to_fit();
        // No reclaim (not the last allocation), but the contents stay correct.
        assert_eq!(arena.bytes_used(), before);
        let s = v.into_slice();
        assert_eq!(s.len(), 10);
        for (i, &x) in s.iter().enumerate() {
            assert_eq!(x, i as u64);
        }
    }

    #[test]
    fn push_reports_arena_exhaustion_without_panicking() {
        // A tiny seed-only (no-grow would still be alloc tier here, but cap the heap) arena: cap the
        // capacity so a grow eventually fails cleanly.
        let arena = Arena::new();
        arena.set_capacity_limit(256);
        let mut v = DecodeVec::<u64>::new(&arena);
        let mut pushed = 0usize;
        while v.push(pushed as u64).is_ok() {
            pushed += 1;
            assert!(pushed <= 10_000, "expected the capped arena to exhaust");
        }
        // Whatever fit is intact and in order.
        let got = pushed;
        let s = v.into_slice();
        assert_eq!(s.len(), got);
        for (i, &x) in s.iter().enumerate() {
            assert_eq!(x, i as u64);
        }
    }

    #[test]
    fn extend_from_le_bytes_bulk_copies_fixed_elements_in_one_allocation() {
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        // The little-endian wire image of 50 u64 values.
        let vals: [u64; 50] = core::array::from_fn(|i| (i as u64) << 40 | i as u64);
        let mut bytes = alloc::vec::Vec::new();
        for x in vals {
            bytes.extend_from_slice(&x.to_le_bytes());
        }
        v.extend_from_le_bytes(&bytes).unwrap();
        // Exactly one arena allocation of the exact element count (no over-estimate, no regrow).
        assert_eq!(arena.bytes_used(), 50 * 8);
        let s = v.into_slice();
        assert_eq!(s, &vals[..]);
    }

    #[test]
    fn extend_from_le_bytes_decodes_float_bits_and_appends_after_existing() {
        let arena = Arena::new();
        let mut v = DecodeVec::<f64>::new(&arena);
        // A prior (e.g. unpacked) element already present, then a packed fixed run appended.
        v.push(2.5f64).unwrap();
        let vals = [1.0f64, -0.0, f64::INFINITY, 12345.6789];
        let mut bytes = alloc::vec::Vec::new();
        for x in vals {
            bytes.extend_from_slice(&x.to_le_bytes());
        }
        v.extend_from_le_bytes(&bytes).unwrap();
        let s = v.into_slice();
        assert_eq!(s.len(), 5);
        assert_eq!(s[0], 2.5);
        assert_eq!(s[1], 1.0);
        assert_eq!(s[2], -0.0);
        assert_eq!(s[3], f64::INFINITY);
        assert_eq!(s[4], 12345.6789);
    }

    #[test]
    fn extend_from_le_bytes_empty_is_a_noop() {
        let arena = Arena::new();
        let mut v = DecodeVec::<u32>::new(&arena);
        v.extend_from_le_bytes(&[]).unwrap();
        assert!(v.is_empty());
        assert_eq!(arena.bytes_used(), 0);
        assert_eq!(v.into_slice(), &[] as &[u32]);
    }
}
