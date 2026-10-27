//! The arena under a controlled host allocator: a refused allocation surfaces as `Err`, and chunks
//! the allocator places back to back stay separate. A global allocator is per binary, so these tests
//! have their own.
#![cfg(feature = "alloc")]

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::{Cell, UnsafeCell};
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};

use rapidproto::{Arena, ArenaExhausted, DecodeVec};

/// What the host allocator does on this thread.
#[derive(Clone, Copy)]
enum Mode {
    System,
    /// Refuse every allocation.
    Fail,
    /// Refuse the arena's chunk list (aligned like a pointer) but not its byte-aligned chunks.
    FailAligned,
    /// Serve blocks back to back from `BUMP`, never freeing them.
    Bump,
}

thread_local! {
    static MODE: Cell<Mode> = const { Cell::new(Mode::System) };
}

const BUMP_SIZE: usize = 1 << 20;

#[repr(align(4096))]
struct BumpBuf(UnsafeCell<[u8; BUMP_SIZE]>);

// SAFETY: the buffer is only handed out in disjoint blocks, claimed through the atomic `BUMP_NEXT`.
unsafe impl Sync for BumpBuf {}

static BUMP: BumpBuf = BumpBuf(UnsafeCell::new([0; BUMP_SIZE]));
static BUMP_NEXT: AtomicUsize = AtomicUsize::new(0);

fn bump(layout: Layout) -> *mut u8 {
    let mut next = BUMP_NEXT.load(Ordering::Relaxed);
    loop {
        // Checked: a panic inside the global allocator aborts.
        let Some((start, end)) = next
            .checked_next_multiple_of(layout.align())
            .and_then(|start| Some((start, start.checked_add(layout.size())?)))
            .filter(|&(_, end)| end <= BUMP_SIZE)
        else {
            return ptr::null_mut();
        };
        match BUMP_NEXT.compare_exchange(next, end, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return BUMP.0.get().cast::<u8>().wrapping_add(start),
            Err(seen) => next = seen,
        }
    }
}

struct Host;

// SAFETY: every mode returns null or a block fitting `layout` that no other live block overlaps;
// `dealloc` frees through `System` exactly the blocks `System` allocated.
unsafe impl GlobalAlloc for Host {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        match MODE.with(Cell::get) {
            // SAFETY: the caller upholds `GlobalAlloc::alloc`'s contract, which `System` shares.
            Mode::System => unsafe { System.alloc(layout) },
            Mode::Fail => ptr::null_mut(),
            Mode::FailAligned if layout.align() > 1 => ptr::null_mut(),
            // SAFETY: as for `Mode::System`.
            Mode::FailAligned => unsafe { System.alloc(layout) },
            Mode::Bump => bump(layout),
        }
    }

    unsafe fn dealloc(&self, block: *mut u8, layout: Layout) {
        let base = BUMP.0.get().cast::<u8>().addr();
        if !(base..base + BUMP_SIZE).contains(&block.addr()) {
            // SAFETY: a block outside the bump buffer came from `System` with this layout.
            unsafe { System.dealloc(block, layout) };
        }
    }
}

#[global_allocator]
static HOST: Host = Host;

/// Run `f` with the host allocator in `mode` on this thread.
fn with_host<R>(mode: Mode, f: impl FnOnce() -> R) -> R {
    MODE.with(|m| m.set(mode));
    let r = f();
    MODE.with(|m| m.set(Mode::System));
    r
}

#[test]
fn a_refused_first_chunk_is_an_error() {
    let a = with_host(Mode::Fail, Arena::new);
    assert_eq!(
        with_host(Mode::Fail, || a.alloc_bytes(1, 1)),
        Err(ArenaExhausted)
    );
    assert!(a.alloc_bytes(1, 1).is_ok());
}

#[test]
fn a_refused_chunk_list_is_an_error() {
    let a = Arena::new();
    a.alloc_bytes(1, 1).unwrap(); // chunk 0 only: no list yet
    let reserved = a.bytes_reserved();
    assert_eq!(
        with_host(Mode::FailAligned, || a.alloc_bytes(5000, 1)),
        Err(ArenaExhausted)
    );
    assert_eq!(a.bytes_reserved(), reserved);
    assert!(a.alloc_bytes(5000, 1).is_ok());
}

#[test]
fn a_refused_later_chunk_is_an_error() {
    let a = Arena::new();
    a.alloc_bytes(1, 1).unwrap();
    a.alloc_bytes(5000, 1).unwrap(); // the list exists, with room to spare
    let reserved = a.bytes_reserved();
    assert_eq!(
        with_host(Mode::Fail, || a.alloc_bytes(20000, 1)),
        Err(ArenaExhausted)
    );
    assert_eq!(a.bytes_reserved(), reserved);
    assert!(a.alloc_bytes(20000, 1).is_ok());
}

#[test]
fn a_seeded_arena_allocates_nothing_until_it_grows() {
    let mut buf = [0u8; 256];
    let a = with_host(Mode::Fail, || Arena::with_buffer(&mut buf));
    assert!(with_host(Mode::Fail, || a.alloc_bytes(64, 8)).is_ok());
}

// A `DecodeVec` that ends exactly where the next chunk begins: shrinking it must not move the cursor
// back into its chunk, or the next allocation would run across both chunks.
#[test]
fn shrink_to_fit_never_moves_the_cursor_into_an_earlier_chunk() {
    with_host(Mode::Bump, || {
        let arena = Arena::new();
        arena.alloc_bytes(1, 1).unwrap(); // chunk 0
        let reserved = arena.bytes_reserved();
        let p = arena.alloc_bytes(5000, 1).unwrap(); // chunk 1
        let chunk1_end = p.as_ptr().addr() + (arena.bytes_reserved() - reserved);
        let mut a = DecodeVec::<u8>::new(&arena);
        a.reserve(chunk1_end - p.as_ptr().addr() - 5000).unwrap(); // fill chunk 1 to its end
        a.push(7).unwrap();
        let mut b = DecodeVec::<u8>::new(&arena);
        b.reserve(10).unwrap(); // chunk 2, right behind chunk 1
        b.shrink_to_fit(); // empty: the cursor returns to chunk 2's start, which is chunk 1's end
        let cursor = arena.alloc_bytes(0, 1).unwrap().as_ptr().addr();
        assert_eq!(
            cursor, chunk1_end,
            "chunk 2 should start where chunk 1 ends"
        );
        a.shrink_to_fit(); // `a` ends at the cursor too, but lies in chunk 1
        let next = arena.alloc_bytes(10_000, 1).unwrap().as_ptr().addr();
        assert!(
            next >= chunk1_end || next + 10_000 <= chunk1_end,
            "an allocation runs across chunk 1's end"
        );
    });
}
