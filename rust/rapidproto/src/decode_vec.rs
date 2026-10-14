//! A decode-time growable array backed by the arena, finished into a borrowed slice.
//!
//! Generated decoders use it to gather a repeated field in a single pass: elements are pushed as they
//! are read off the wire, then [`into_slice`](DecodeVec::into_slice) hands back the finished
//! `&'a [T]`. A grow allocates a larger arena array and copies the elements over; the old array is
//! abandoned (the arena never frees, matching the C++ decoder's growable repeated storage). This is
//! the only place the growth bookkeeping lives, so the generated code stays simple.

use core::ptr::{self, NonNull};

use crate::arena::{Arena, ArenaExhausted};

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

    #[cold]
    fn grow(&mut self) -> Result<(), ArenaExhausted> {
        let new_cap = if self.cap == 0 {
            FIRST_CAP
        } else {
            self.cap.checked_mul(2).ok_or(ArenaExhausted)?
        };
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
}
