//! A chunked bump arena with one allocation API across three tiers.
//!
//! The same [`Arena`] type serves:
//!   - **`no_std`, no allocator** (embedded): the caller provides the first chunk via
//!     [`Arena::with_buffer`]; there is no growth, so exhausting the chunk returns
//!     `Err(ArenaExhausted)`.
//!   - **`no_std` + `alloc`**: the same, plus geometric heap growth once a chunk fills (chunk
//!     sizes double up to a cap). [`Arena::reset`] rewinds for reuse and **keeps** the chunks, so
//!     the next decode allocates nothing until it exceeds what is already held.
//!   - **`std`**: identical to `no_std` + `alloc`.
//!
//! [`Arena::alloc_bytes`] is identical in every tier and **never panics** — a failed host
//! allocation, or hitting [`set_capacity_limit`](Arena::set_capacity_limit), becomes `Err`, not an
//! abort. The hot path is a pointer bump; growth touches the chunk list.
//!
//! The bump path uses `pointer::align_offset` + `pointer::add` + `pointer::addr` only, with no
//! integer-to-pointer casts, so it is sound under Miri's strict-provenance model.

use core::cell::Cell;
use core::marker::PhantomData;
use core::ptr::NonNull;

#[cfg(feature = "alloc")]
use alloc::{boxed::Box, vec::Vec};
#[cfg(feature = "alloc")]
use core::cell::RefCell;
#[cfg(feature = "alloc")]
use core::mem::MaybeUninit;

/// The maximum alignment the arena guarantees for a chunk's usable region. Every generated type's
/// alignment is at most this (protobuf scalars top out at 8). Matches C++'s `alignof(max_align_t)`.
const MAX_ALIGN: usize = 16;

#[cfg(feature = "alloc")]
const DEFAULT_CHUNK: usize = 4096;
/// Geometric chunk growth stops doubling here, capping the wasted tail of the last chunk. Kept
/// under glibc's 128 KiB mmap threshold so cold-arena chunks stay on the heap.
#[cfg(feature = "alloc")]
const MAX_CHUNK: usize = 96 * 1024;

/// Returned when the arena cannot satisfy an allocation: in the no-allocator tier when the seed
/// chunk is full, or in an `alloc` tier when the capacity limit is hit or the host allocator
/// itself fails. Never a panic.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub struct ArenaExhausted;

/// One chunk's usable byte range. `storage` is the owning heap block as a raw `Box` pointer, freed
/// in `Arena`'s `Drop`; it is `None` for the caller-seeded buffer, which the arena never frees.
///
/// The storage is kept as a raw pointer (via `Box::into_raw`), not a live `Box`: a pointer derived
/// from a `Box` is invalidated under Stacked Borrows once the `Box` is moved (e.g. when the `Vec`
/// reallocates), whereas an `into_raw` pointer owns the allocation and survives moves.
#[cfg(feature = "alloc")]
struct Chunk {
    storage: Option<NonNull<[MaybeUninit<u8>]>>,
    data: NonNull<u8>,
    limit: NonNull<u8>,
}

#[cfg(feature = "alloc")]
struct Grow {
    chunks: Vec<Chunk>, // 0 = head (seed or first grown); 1.. are grown chunks
    cursor: usize,      // index of the active chunk (meaningful only when `chunks` is non-empty)
    next_chunk: usize,  // size of the next chunk to grow
    cap: usize,         // capacity limit on reserved bytes; usize::MAX = unbounded
}

/// A chunked bump arena. `'buf` ties it to a caller-seeded first chunk (if any); it defaults to
/// `'static` so the heap-only constructor carries no borrow.
pub struct Arena<'buf> {
    // Hot bump state (plain `Cell`s so the fast path never borrows the chunk list).
    cur: Cell<NonNull<u8>>,
    limit: Cell<NonNull<u8>>,
    used: Cell<usize>,
    reserved: Cell<usize>,
    #[cfg(feature = "alloc")]
    grow: RefCell<Grow>,
    // In the no-allocator tier the seed is the only region; remember it to rewind on reset.
    #[cfg(not(feature = "alloc"))]
    seed: Cell<Option<(NonNull<u8>, NonNull<u8>)>>,
    _marker: PhantomData<&'buf mut [u8]>,
}

// Compute the usable, MAX_ALIGN-aligned sub-range of `[base, base+len)`. Returns `None` if the
// buffer is too small to carry an aligned region.
fn aligned_region(base: *mut u8, len: usize) -> Option<(NonNull<u8>, NonNull<u8>)> {
    let pad = base.align_offset(MAX_ALIGN);
    if pad == usize::MAX || pad >= len {
        return None;
    }
    // SAFETY: `pad < len`, so `base + pad` is in bounds and `base + len` is one past the end.
    let data = unsafe { base.add(pad) };
    let end = unsafe { base.add(len) };
    // SAFETY: both are derived from `base` and non-null.
    Some(unsafe { (NonNull::new_unchecked(data), NonNull::new_unchecked(end)) })
}

impl<'buf> Arena<'buf> {
    /// Seed the arena with a caller-provided first chunk. Available in **every** tier — this is the
    /// only constructor the no-allocator tier has, and it needs no allocator. A buffer of at most
    /// `MAX_ALIGN` bytes is ignored (too small to carry a useful aligned region, matching C++), so
    /// the arena starts empty and grows on first use.
    pub fn with_buffer(buf: &'buf mut [u8]) -> Self {
        let region = if buf.len() > MAX_ALIGN {
            aligned_region(buf.as_mut_ptr(), buf.len())
        } else {
            None
        };
        let dangling = NonNull::<u8>::dangling();
        let (cur, limit) = region.unwrap_or((dangling, dangling));
        let reserved = region.map_or(0, |(d, l)| l.as_ptr().addr() - d.as_ptr().addr());
        Arena {
            cur: Cell::new(cur),
            limit: Cell::new(limit),
            used: Cell::new(0),
            reserved: Cell::new(reserved),
            #[cfg(feature = "alloc")]
            grow: RefCell::new(Grow {
                chunks: region.map_or_else(Vec::new, |(d, l)| {
                    alloc::vec![Chunk {
                        storage: None,
                        data: d,
                        limit: l,
                    }]
                }),
                cursor: 0,
                next_chunk: DEFAULT_CHUNK,
                cap: usize::MAX,
            }),
            #[cfg(not(feature = "alloc"))]
            seed: Cell::new(region),
            _marker: PhantomData,
        }
    }

    /// The single allocation primitive: `bytes` aligned to `align` (which must be a power of two).
    /// Identical signature in all tiers; **never panics** — a non-power-of-two `align` is rejected
    /// with `Err` rather than aborting (generated code only ever passes `align_of`).
    pub fn alloc_bytes(&self, bytes: usize, align: usize) -> Result<NonNull<u8>, ArenaExhausted> {
        if !align.is_power_of_two() {
            return Err(ArenaExhausted);
        }
        if let Some(p) = self.try_bump(bytes, align) {
            return Ok(p);
        }
        self.allocate_slow(bytes, align)
    }

    /// Typed convenience: allocate and move `val` in, handing back `&mut T`.
    ///
    /// This is the bump-arena idiom of handing out `&mut` from `&self`; distinct allocations never
    /// overlap, which is what makes it sound (verified under Miri's Stacked/Tree Borrows).
    ///
    /// The arena never runs `T`'s destructor, so `T` must not own resources that need `Drop`.
    /// Decoded values are borrowed or plain data (scalars, `&[T]`, `ProtoStr`, `MapView`), so this
    /// holds; do not arena-allocate a `Drop` type.
    #[allow(clippy::mut_from_ref)]
    pub fn alloc<T>(&self, val: T) -> Result<&mut T, ArenaExhausted> {
        let p = self
            .alloc_bytes(core::mem::size_of::<T>(), core::mem::align_of::<T>())?
            .cast::<T>();
        // SAFETY: `p` points at `size_of::<T>()` freshly-reserved, `align_of::<T>()`-aligned bytes
        // that no other reference aliases, so writing `val` and forming a `&mut T` is sound.
        unsafe {
            p.as_ptr().write(val);
            Ok(&mut *p.as_ptr())
        }
    }

    /// Allocate a `T` slot and decode into it **in place** -- no by-value construct-then-copy. `f`
    /// initializes the slot (a generated `decode_into` writing each field straight into it); the
    /// `&mut T` is returned only if `f` succeeds. On `Err` the slot is abandoned, never read (the
    /// arena never frees, so the reserved bytes are simply unused). This is the Rust equivalent of the
    /// C++ decoder allocating the slot then `rp_decode_into(*slot, ...)`.
    #[allow(clippy::mut_from_ref)]
    // `#[inline(always)]`: the closure captures the sub-message payload slice (and arena/depth) by
    // reference, so an out-of-line `emplace` would pass that environment by pointer and pin the 16-byte
    // `&[u8]` to memory -- then the decode reloads it with a wide load that cannot store-forward from the
    // narrower stores that wrote it, a large store-forward stall. Inlining keeps the payload in registers
    // (the C++ by-value shape): measured ~15-20% faster on sub-message-heavy decodes, store-forward
    // blocks eliminated, flat elsewhere.
    #[inline(always)]
    pub fn emplace<T, E, F>(&self, f: F) -> Result<&mut T, E>
    where
        E: From<ArenaExhausted>,
        F: for<'id> FnOnce(
            crate::decode_slot::Slot<'id, T>,
        ) -> Result<crate::decode_slot::Init<'id>, E>,
    {
        let p = self
            .alloc_bytes(core::mem::size_of::<T>(), core::mem::align_of::<T>())?
            .cast::<T>();
        // SAFETY: `p` is `size_of::<T>()` freshly reserved, `align_of::<T>()`-aligned bytes nothing
        // else aliases, valid to write for the `Slot`'s (this call's) lifetime.
        let slot = unsafe { crate::decode_slot::Slot::new(p) };
        let _init = f(slot)?;
        // SAFETY: `f` returned `Init<'id>` of this slot's fresh brand, obtainable only via
        // `Slot::write` on this slot (unforgeable), so `p` is initialized. The returned `&mut T` is
        // tied to `&self` (elided), so it cannot outlive the arena -- no use-after-free.
        Ok(unsafe { &mut *p.as_ptr() })
    }

    /// Reserve **uninitialized** storage for `n` values of `T`, returning a pointer to the first.
    /// The caller must initialize all `n` before reading. `n == 0` and a count that would overflow
    /// are rejected with `Err`. (Used by repeated/packed decode to pre-size an array.)
    ///
    /// A zero-sized `T` (e.g. an empty generated message) needs no storage, so any `n > 0` yields a
    /// valid aligned pointer to a 0-byte region, as for any ZST slice.
    pub fn alloc_array<T>(&self, n: usize) -> Result<NonNull<T>, ArenaExhausted> {
        let elem = core::mem::size_of::<T>();
        if n == 0 || (elem != 0 && n > usize::MAX / elem) {
            return Err(ArenaExhausted);
        }
        let p = self.alloc_bytes(n * elem, core::mem::align_of::<T>())?;
        Ok(p.cast::<T>())
    }

    /// Reclaim the unused tail of the **most-recent** allocation, shrinking it from `old_bytes` to
    /// `new_bytes` (`new_bytes <= old_bytes`). A no-op unless `ptr` is still the last allocation —
    /// its end is exactly the bump cursor, i.e. nothing has been allocated since.
    ///
    /// Packed/repeated decode uses this: pre-size an array to an upper bound, fill it (allocating
    /// nothing in between), then trim the over-estimate to the exact element count. Decoder-internal
    /// — `old_bytes` must be the true size of that allocation.
    pub fn shrink_last(&self, ptr: NonNull<u8>, old_bytes: usize, new_bytes: usize) {
        if new_bytes > old_bytes {
            return;
        }
        let base = ptr.as_ptr();
        // Integer comparison, never `base.add(old_bytes)`: forming that pointer would be
        // out-of-bounds (hence UB) if `ptr` were not actually the last allocation.
        if base.addr().wrapping_add(old_bytes) == self.cur.get().as_ptr().addr() {
            // SAFETY: the end addresses matched, so `ptr` is the last allocation and spans at least
            // `old_bytes`; `new_bytes <= old_bytes`, so `base + new_bytes` stays within that block.
            let new_cur = unsafe { base.add(new_bytes) };
            self.cur.set(unsafe { NonNull::new_unchecked(new_cur) });
            self.used.set(self.used.get() - (old_bytes - new_bytes));
        }
    }

    /// Bytes handed out to callers (the useful payload; alignment padding is not counted).
    pub fn bytes_used(&self) -> usize {
        self.used.get()
    }

    /// Total usable bytes across all held chunks (the host memory the arena is holding).
    pub fn bytes_reserved(&self) -> usize {
        self.reserved.get()
    }

    /// Cap the total reserved bytes: a grow past `max_reserved_bytes` fails with `Err`, so decoding
    /// untrusted input cannot make the arena allocate without bound. Default: unbounded. Set before
    /// decoding; should be at least any seed buffer. In the no-allocator tier there is no growth to
    /// cap, so this has no effect.
    pub fn set_capacity_limit(&self, max_reserved_bytes: usize) {
        #[cfg(feature = "alloc")]
        {
            self.grow.borrow_mut().cap = max_reserved_bytes;
        }
        #[cfg(not(feature = "alloc"))]
        let _ = max_reserved_bytes;
    }

    /// Rewind to the first chunk for reuse. In an `alloc` tier the chunks are **kept** (no free, no
    /// malloc on the next decode); in the no-allocator tier it rewinds to the seed.
    pub fn reset(&mut self) {
        self.used.set(0);
        #[cfg(feature = "alloc")]
        {
            let g = self.grow.get_mut();
            g.cursor = 0;
            match g.chunks.first() {
                Some(head) => {
                    self.cur.set(head.data);
                    self.limit.set(head.limit);
                }
                None => {
                    let d = NonNull::<u8>::dangling();
                    self.cur.set(d);
                    self.limit.set(d);
                }
            }
        }
        #[cfg(not(feature = "alloc"))]
        match self.seed.get() {
            Some((data, limit)) => {
                self.cur.set(data);
                self.limit.set(limit);
            }
            None => {
                let d = NonNull::<u8>::dangling();
                self.cur.set(d);
                self.limit.set(d);
            }
        }
    }

    fn try_bump(&self, bytes: usize, align: usize) -> Option<NonNull<u8>> {
        let cur = self.cur.get().as_ptr();
        let limit = self.limit.get().as_ptr();
        // Provenance-clean: `addr()` for the arithmetic, `add()` for the actual pointer. `cur <=
        // limit` is an invariant, so this never underflows.
        let avail = limit.addr() - cur.addr();
        let pad = cur.align_offset(align);
        if pad == usize::MAX || pad > avail || bytes > avail - pad {
            return None;
        }
        // SAFETY: `pad + bytes <= avail`, so both offsets stay within (or one past) the chunk.
        let aligned = unsafe { cur.add(pad) };
        let new_cur = unsafe { aligned.add(bytes) };
        self.cur.set(unsafe { NonNull::new_unchecked(new_cur) });
        self.used.set(self.used.get() + bytes);
        Some(unsafe { NonNull::new_unchecked(aligned) })
    }

    #[cfg(not(feature = "alloc"))]
    fn allocate_slow(&self, _bytes: usize, _align: usize) -> Result<NonNull<u8>, ArenaExhausted> {
        Err(ArenaExhausted) // no allocator → cannot grow; a clean error, never a panic
    }

    #[cfg(feature = "alloc")]
    fn allocate_slow(&self, bytes: usize, align: usize) -> Result<NonNull<u8>, ArenaExhausted> {
        {
            let mut g = self.grow.borrow_mut();
            // After a reset, reuse an already-held successor chunk if the request fits it.
            if g.cursor + 1 < g.chunks.len() && chunk_fits(&g.chunks[g.cursor + 1], bytes, align) {
                g.cursor += 1;
                let c = &g.chunks[g.cursor];
                self.cur.set(c.data);
                self.limit.set(c.limit);
            } else {
                self.grow_chunk(&mut g, bytes, align)?;
            }
        }
        // The active chunk now fits; the fast path (no borrow held) completes the allocation.
        self.try_bump(bytes, align).ok_or(ArenaExhausted)
    }

    #[cfg(feature = "alloc")]
    fn grow_chunk(&self, g: &mut Grow, bytes: usize, align: usize) -> Result<(), ArenaExhausted> {
        let min_usable = bytes.checked_add(align).ok_or(ArenaExhausted)?;
        let want = if g.next_chunk < min_usable {
            min_usable
        } else {
            g.next_chunk
        };
        // +MAX_ALIGN so the block base can always be aligned up within it.
        let raw = want.checked_add(MAX_ALIGN).ok_or(ArenaExhausted)?;
        if raw > isize::MAX as usize {
            return Err(ArenaExhausted); // too large for a single allocation
        }
        let reserved = self.reserved.get();
        if reserved >= g.cap || raw > g.cap - reserved {
            return Err(ArenaExhausted); // capacity cap reached → treated as OOM
        }
        // `into_raw` yields a pointer that owns the allocation and stays valid when the chunk moves
        // in the Vec; deriving `base` from a live `Box` would be invalidated by that move.
        let storage: *mut [MaybeUninit<u8>] = Box::into_raw(Box::<[u8]>::new_uninit_slice(raw));
        let base = storage.cast::<u8>(); // first byte, provenance over the whole allocation
        let (data, limit) = aligned_region(base, raw).ok_or(ArenaExhausted)?;
        let usable = limit.as_ptr().addr() - data.as_ptr().addr();

        let chunk = Chunk {
            // SAFETY: `storage` came from `Box::into_raw`, so it is non-null.
            storage: Some(unsafe { NonNull::new_unchecked(storage) }),
            data,
            limit,
        };
        if g.chunks.is_empty() {
            g.chunks.push(chunk);
            g.cursor = 0;
        } else {
            // Insert right after the current chunk so a grow after reset keeps later chunks for reuse.
            g.chunks.insert(g.cursor + 1, chunk);
            g.cursor += 1;
        }
        self.reserved.set(reserved + usable);
        self.cur.set(data);
        self.limit.set(limit);
        if g.next_chunk < MAX_CHUNK {
            g.next_chunk = (g.next_chunk << 1).min(MAX_CHUNK);
        }
        Ok(())
    }
}

/// Does `bytes` aligned to `align` fit in `c`'s usable region? Size comparison only (no OOB ptr).
#[cfg(feature = "alloc")]
fn chunk_fits(c: &Chunk, bytes: usize, align: usize) -> bool {
    let data = c.data.as_ptr();
    let pad = data.align_offset(align);
    let avail = c.limit.as_ptr().addr() - data.addr();
    pad != usize::MAX && pad <= avail && bytes <= avail - pad
}

/// Heap-only constructors, available only when an allocator is present.
#[cfg(feature = "alloc")]
impl Arena<'static> {
    /// A heap-only arena: empty, growing from the first allocation.
    pub fn new() -> Arena<'static> {
        let dangling = NonNull::<u8>::dangling();
        Arena {
            cur: Cell::new(dangling),
            limit: Cell::new(dangling),
            used: Cell::new(0),
            reserved: Cell::new(0),
            grow: RefCell::new(Grow {
                chunks: Vec::new(),
                cursor: 0,
                next_chunk: DEFAULT_CHUNK,
                cap: usize::MAX,
            }),
            _marker: PhantomData,
        }
    }
}

#[cfg(feature = "alloc")]
impl Default for Arena<'static> {
    fn default() -> Self {
        Arena::new()
    }
}

#[cfg(feature = "alloc")]
impl Drop for Arena<'_> {
    fn drop(&mut self) {
        for chunk in self.grow.get_mut().chunks.drain(..) {
            if let Some(storage) = chunk.storage {
                // SAFETY: `storage` came from `Box::into_raw` of a `Box<[MaybeUninit<u8>]>` and is
                // owned solely by this chunk, which is drained exactly once; reconstruct and free it.
                drop(unsafe { Box::from_raw(storage.as_ptr()) });
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_aligned(p: NonNull<u8>, align: usize) {
        assert_eq!(p.as_ptr().addr() % align, 0, "mis-aligned allocation");
    }

    // 8-aligned backing so an exact-fit count is deterministic (a bare `[u8; N]` is only
    // 1-aligned, which Miri enforces). Only the no-allocator exhaustion test uses it.
    #[cfg(not(feature = "alloc"))]
    #[repr(align(16))]
    struct Aligned64([u8; 64]);

    #[test]
    fn alignment_is_honored() {
        let mut buf = [0u8; 256];
        let a = Arena::with_buffer(&mut buf);
        a.alloc_bytes(1, 1).unwrap(); // knock the cursor off-alignment
        for &align in &[1usize, 2, 4, 8, 16] {
            let p = a.alloc_bytes(align, align).unwrap();
            assert_aligned(p, align);
        }
    }

    // The embedded-tier guarantee: a seed-only arena refuses with a clean error (no panic, no UB)
    // once full, and never hands out a pointer beyond its seed buffer. Growth is compiled out here.
    #[cfg(not(feature = "alloc"))]
    #[test]
    fn no_alloc_exhaustion_is_an_error_not_a_panic() {
        let mut backing = Aligned64([0u8; 64]);
        let buf = &mut backing.0;
        let lo = buf.as_mut_ptr().addr();
        let hi = lo + buf.len();
        let a = Arena::with_buffer(buf);
        let mut handed_out = 0usize;
        while let Ok(p) = a.alloc_bytes(8, 8) {
            let addr = p.as_ptr().addr();
            assert!(
                addr >= lo && addr + 8 <= hi,
                "allocation escaped the seed buffer"
            );
            handed_out += 1;
            assert!(handed_out <= 8, "handed out more than the buffer can hold");
        }
        assert_eq!(handed_out, 8, "should fit exactly 8 aligned 8-byte cells");
    }

    #[test]
    fn non_power_of_two_align_is_an_error_not_a_panic() {
        let mut buf = [0u8; 64];
        let a = Arena::with_buffer(&mut buf);
        assert_eq!(a.alloc_bytes(8, 3), Err(ArenaExhausted));
        assert_eq!(a.alloc_bytes(8, 0), Err(ArenaExhausted));
    }

    #[test]
    fn writes_round_trip() {
        let mut buf = [0u8; 256];
        let a = Arena::with_buffer(&mut buf);
        let x: &mut u64 = a.alloc(0xDEAD_BEEF_1234_5678u64).unwrap();
        let y: &mut u32 = a.alloc(0xA5A5_5A5Au32).unwrap();
        assert_eq!(*x, 0xDEAD_BEEF_1234_5678);
        assert_eq!(*y, 0xA5A5_5A5A);
        *x = 1;
        assert_eq!(*x, 1);
    }

    #[test]
    fn bytes_used_tracks_payload_and_seed_is_reserved() {
        let mut buf = [0u8; 256];
        let a = Arena::with_buffer(&mut buf);
        assert!(a.bytes_reserved() >= 240); // ~256 minus alignment padding of the seed region
        assert_eq!(a.bytes_used(), 0);
        a.alloc_bytes(8, 8).unwrap();
        a.alloc_bytes(16, 8).unwrap();
        assert_eq!(a.bytes_used(), 24);
    }

    #[test]
    fn shrink_last_reclaims_the_tail_of_the_last_alloc() {
        let mut buf = [0u8; 256];
        let a = Arena::with_buffer(&mut buf);
        let p = a.alloc_bytes(64, 1).unwrap();
        assert_eq!(a.bytes_used(), 64);
        a.shrink_last(p, 64, 16); // keep 16, give back 48
        assert_eq!(a.bytes_used(), 16);
        let q = a.alloc_bytes(8, 1).unwrap();
        assert_eq!(q.as_ptr().addr(), p.as_ptr().addr() + 16);
    }

    #[test]
    fn shrink_last_is_a_noop_when_not_the_last_alloc() {
        let mut buf = [0u8; 256];
        let a = Arena::with_buffer(&mut buf);
        let p = a.alloc_bytes(64, 1).unwrap();
        let r = a.alloc_bytes(8, 1).unwrap(); // p is no longer the last allocation
        a.shrink_last(p, 64, 16); // must not move the cursor
        let s = a.alloc_bytes(8, 1).unwrap();
        assert_eq!(
            s.as_ptr().addr(),
            r.as_ptr().addr() + 8,
            "cursor must be untouched"
        );
    }

    #[test]
    fn shrink_last_rejects_growth_and_handles_zero() {
        let mut buf = [0u8; 256];
        let a = Arena::with_buffer(&mut buf);
        let p = a.alloc_bytes(32, 1).unwrap();
        a.shrink_last(p, 32, 48); // new > old → no-op
        let q = a.alloc_bytes(8, 1).unwrap();
        assert_eq!(q.as_ptr().addr(), p.as_ptr().addr() + 32, "no-op on growth");
        a.shrink_last(q, 8, 0); // trim the whole last alloc
        let t = a.alloc_bytes(8, 1).unwrap();
        assert_eq!(
            t.as_ptr().addr(),
            q.as_ptr().addr(),
            "shrink-to-zero reclaims fully"
        );
    }

    #[test]
    fn reset_reuses_the_seed() {
        let mut buf = [0u8; 64];
        let mut a = Arena::with_buffer(&mut buf);
        let first = a.alloc_bytes(8, 8).unwrap().as_ptr().addr();
        a.alloc_bytes(8, 8).unwrap();
        a.reset();
        assert_eq!(a.bytes_used(), 0);
        let after = a.alloc_bytes(8, 8).unwrap().as_ptr().addr();
        assert_eq!(first, after, "reset should rewind to the seed base");
    }

    #[cfg(feature = "alloc")]
    #[test]
    fn heap_arena_grows_past_any_chunk() {
        let a = Arena::new();
        let mut last = 0usize;
        for i in 0..5000 {
            let p = a.alloc(i as u64).unwrap();
            assert_eq!(*p, i as u64);
            last = *p as usize;
        }
        assert_eq!(last, 4999);
    }

    #[cfg(feature = "alloc")]
    #[test]
    fn seed_then_grow_is_seamless() {
        let mut buf = [0u8; 16];
        let a = Arena::with_buffer(&mut buf);
        for i in 0..1000u64 {
            assert!(a.alloc(i).is_ok(), "seed→heap spill must be seamless");
        }
    }

    #[cfg(feature = "alloc")]
    #[test]
    fn big_request_bigger_than_grow_step_succeeds() {
        let a = Arena::new();
        let p = a.alloc_bytes(1 << 20, 64).unwrap();
        assert_aligned(p, 64);
    }

    #[cfg(feature = "alloc")]
    #[test]
    fn alloc_array_reserves_storage() {
        let a = Arena::new();
        let p = a.alloc_array::<u32>(4).unwrap();
        assert_aligned(p.cast::<u8>(), core::mem::align_of::<u32>());
        // Initialize and read back through the raw pointer.
        for i in 0..4 {
            unsafe { p.as_ptr().add(i).write(i as u32 * 10) };
        }
        for i in 0..4 {
            assert_eq!(unsafe { *p.as_ptr().add(i) }, i as u32 * 10);
        }
        assert_eq!(a.alloc_array::<u32>(0), Err(ArenaExhausted)); // zero rejected
    }

    #[cfg(feature = "alloc")]
    #[test]
    fn capacity_limit_makes_growth_fail_cleanly() {
        let a = Arena::new();
        a.set_capacity_limit(8 * 1024); // small cap
        let mut total = 0usize;
        // Keep allocating until the cap is hit; must get a clean Err, never a panic/abort.
        let mut hit_cap = false;
        for _ in 0..100_000 {
            match a.alloc_bytes(64, 8) {
                Ok(_) => total += 64,
                Err(ArenaExhausted) => {
                    hit_cap = true;
                    break;
                }
            }
        }
        assert!(hit_cap, "the cap should eventually be reached");
        assert!(
            a.bytes_reserved() <= 8 * 1024,
            "reserved must stay within the cap"
        );
        assert!(total > 0);
    }

    #[cfg(feature = "alloc")]
    #[test]
    fn reset_keeps_chunks_and_reserved() {
        let a = Arena::new();
        for i in 0..5000u64 {
            a.alloc(i).unwrap(); // force several chunk growths
        }
        let reserved_before = a.bytes_reserved();
        assert!(reserved_before > 0);
        // reset() needs &mut; move out of the shared borrows by reborrowing a local binding.
        let mut a = a;
        a.reset();
        assert_eq!(a.bytes_used(), 0);
        assert_eq!(
            a.bytes_reserved(),
            reserved_before,
            "reset keeps chunks (no free), so reserved is unchanged"
        );
        // The next allocations reuse held chunks (no new reservation until exceeded).
        for i in 0..5000u64 {
            a.alloc(i).unwrap();
        }
        assert_eq!(
            a.bytes_reserved(),
            reserved_before,
            "reused the held chunks without growing"
        );
    }

    #[test]
    fn emplace_initializes_in_place_on_ok_and_abandons_on_err() {
        // `with_buffer` so this also runs on the no-allocator (`--no-default-features`) Miri tier.
        let mut buf = [0u8; 64];
        let a = Arena::with_buffer(&mut buf);
        // Ok: the slot is written in place; `emplace` hands back a `&mut` to the initialized value.
        let r = a
            .emplace::<u64, ArenaExhausted, _>(|slot| Ok(slot.write(0xABCD)))
            .unwrap();
        assert_eq!(*r, 0xABCD);
        // Err after a write: the `Init` proof is discarded and `emplace` returns `Err` without forming
        // a published `&mut T` for the caller. Running clean (incl. under Miri) is the assertion.
        let e = a.emplace::<u64, ArenaExhausted, _>(|slot| {
            slot.write(1);
            Err(ArenaExhausted)
        });
        assert!(e.is_err());
    }

    // A closure that tries to return `Ok` WITHOUT writing the slot cannot be written: there is no way
    // to obtain the `Init<'id>` proof except `Slot::write`, so the forge that broke the earlier
    // `-> &mut T` witness (returning a leaked/foreign reference) is a compile error now. That guarantee
    // is pinned by a `compile_fail` doctest on `decode_slot::Init`.

    #[test]
    fn emplace_handles_a_zero_sized_type() {
        // A ZST message (empty / marker-only) goes through `alloc_bytes(0, _)`; emplacing it must be
        // sound (exercises the 0-byte-alloc + uninit-slot write path under Miri).
        let mut buf = [0u8; 8];
        let a = Arena::with_buffer(&mut buf);
        let r = a
            .emplace::<(), ArenaExhausted, _>(|slot| Ok(slot.write(())))
            .unwrap();
        assert_eq!(*r, ());
    }

    #[cfg(feature = "alloc")]
    #[test]
    fn emplace_handles_a_zero_sized_type_from_a_dangling_cursor() {
        // A fresh heap arena serves a 0-byte (ZST) alloc straight off the dangling bump cursor
        // (`try_bump` returns `cur` with nothing allocated yet). Minting a `Slot` over that aligned,
        // non-null-but-dangling pointer and writing a ZST through it must be sound -- the `with_buffer`
        // case above can't reach this path (its cursor points into real storage). Miri is the check.
        let a = Arena::new();
        let r = a
            .emplace::<(), ArenaExhausted, _>(|slot| Ok(slot.write(())))
            .unwrap();
        assert_eq!(*r, ());
    }

    #[test]
    fn decode_value_moves_out_on_ok_and_propagates_err() {
        let v = crate::decode_value::<u64, ArenaExhausted, _>(|slot| Ok(slot.write(123))).unwrap();
        assert_eq!(v, 123);
        let e = crate::decode_value::<u64, ArenaExhausted, _>(|slot| {
            slot.write(9);
            Err(ArenaExhausted)
        });
        assert!(e.is_err());
    }
}
