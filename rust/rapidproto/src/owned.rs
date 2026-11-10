//! A self-contained decode: [`decode_owned`] returns an [`Owned`] that owns its input bytes and its
//! [`Arena`], so the decoded tree stays valid with no external lifetime to manage — the Rust pendant
//! of the C++ `decode_owned`. Requires `alloc` (it heap-owns the input and a growing arena).
//!
//! The low-level [`decode`](crate::Arena) path (`T::decode(bytes, &arena)`) stays for callers who
//! bring their own arena / keep the input alive themselves; this is the owns-everything convenience.
//!
//! It is a self-referential value — the decoded root borrows from both the owned input and the owned
//! arena — which safe Rust cannot express, so the `unsafe` is concentrated HERE (no `yoke`/`self_cell`
//! dependency) and audited once. Generated code only provides a safe [`ArenaDecode`] impl.

use core::fmt;
use core::ptr::NonNull;

use alloc::boxed::Box;

use crate::{Arena, DecodeError};

/// A message type that [`Owned`] can hold.
///
/// Generated arena messages implement it, so you do not need to.
///
/// # Safety
/// [`Owned`] stores the message with its lifetime changed to `'static`, and [`get`](Owned::get)
/// shortens it again. That is sound only if `Self::Message`:
/// - is covariant in `'a`, so `Message<'static>` can be read as a `Message` with a shorter lifetime,
/// - has the same layout at every lifetime,
/// - has no interior mutability over the `'a` borrow, such as a `Cell<&'a _>` or a reachable
///   `&'a mut _`, so a short-lived `&self` cannot store a short reference where a `'static` one is
///   expected, and
/// - never points at the `Arena` value itself. The reference `decode_borrowed` returns, and everything
///   reachable from it, point only into `input`, memory `arena` allocated, or `'static` data. `Owned`
///   moves the arena after the decode, and its memory stays in place, but the `Arena` value does not.
///
/// Every generated message meets these conditions.
pub unsafe trait ArenaDecode {
    /// The message type, borrowing the input and the arena for `'a`.
    type Message<'a>: 'a;

    /// Decodes `input` into `arena` and returns the message.
    fn decode_borrowed<'a>(
        input: &'a [u8],
        arena: &'a Arena<'a>,
    ) -> Result<&'a Self::Message<'a>, DecodeError>;
}

/// A decoded message together with its input and its [`Arena`].
///
/// The generated `Msg::decode_owned` returns an `Owned<Msg<'static>>`. The `'static` only names the
/// message type. [`get`](Owned::get) returns the message borrowed from the `Owned`. An `Owned` has no
/// lifetime, so you can store it or return it from a function.
///
/// It is `Send` when the message is, and `Sync` when the message is. Every generated message is both,
/// so an `Owned` can move to another thread or be shared through an `Arc`.
pub struct Owned<D: ArenaDecode> {
    // The decoded root, with its borrow erased to `'static`. It points INTO `arena`'s chunks and
    // `input`'s bytes, both owned below and never moved/mutated/freed until this `Owned` drops, so
    // the pointee outlives every borrow `get` hands out. `get` re-borrows it for the shorter `&self`
    // lifetime (`Message` is covariant in its lifetime, so the widening is sound). `D` is a
    // never-instantiated tag, used only through this projection -- which still constrains the type
    // parameter, so no `PhantomData` is needed. First in declaration order, but it owns nothing --
    // drop order among the fields is irrelevant (the root is never read at drop, and `arena`/`input`
    // do not reference each other).
    root: NonNull<D::Message<'static>>,
    // `Arena<'static>` heap-owns its chunks (address-stable across the move into this struct); the
    // tree's pointers point into those chunks, not into the arena struct itself. Read only through
    // `root` and dropped at the end -- it exists to OWN the tree's backing, so `dead_code` (which
    // ignores Drop) would flag it.
    #[allow(dead_code)]
    arena: Arena<'static>,
    // The input bytes, owned as a RAW pointer (from `Box::leak`), not a `Box`: a `Box` asserts
    // unique access, so moving this struct would retag the heap block and invalidate the root's
    // borrow into it (Miri/Stacked Borrows). A raw pointer makes the ownership move-safe -- the same
    // trick the arena uses for its chunks. Freed once in `Drop`.
    input: NonNull<[u8]>,
}

// SAFETY: an `Owned` is its input bytes, its arena and the message in them, all owned and reached
// only through `get`. Moving it to another thread moves all three, as a `Box` of them would, so it is
// `Send` when the message is. The bytes are plain data and the arena is `Send`.
unsafe impl<D: ArenaDecode> Send for Owned<D> where D::Message<'static>: Send {}

// SAFETY: a shared `Owned` reaches only the message (`get` and `Debug`), never the arena, so sharing
// it shares only the message, and it is `Sync` when the message is. The arena is not `Sync`, but
// nothing can allocate through a shared `Owned`.
unsafe impl<D: ArenaDecode> Sync for Owned<D> where D::Message<'static>: Sync {}

impl<D: ArenaDecode> Drop for Owned<D> {
    fn drop(&mut self) {
        // SAFETY: `input` came from `Box::leak` in `decode_owned` and is reclaimed exactly once,
        // here; `root` (which points into it) is never used after drop begins.
        drop(unsafe { Box::from_raw(self.input.as_ptr()) });
    }
}

impl<D: ArenaDecode> Owned<D> {
    /// Returns the message, borrowed from the `Owned`.
    #[must_use]
    pub fn get(&self) -> &D::Message<'_> {
        // SAFETY: `root` points into `self.arena`/`self.input`, both owned by `self` and unmoved and
        // unmutated since construction, so the pointee is live and valid for all of `&self`. The
        // returned reference is tied to `&self` (elision), so it cannot outlive the owner. The stored
        // pointee is `Message` at `'static`; reading it as `Message` at the shorter `&self` lifetime
        // is sound because `ArenaDecode`'s safety contract makes `Message` covariant, layout-stable,
        // and free of interior mutability over the borrow.
        unsafe { self.root.cast::<D::Message<'_>>().as_ref() }
    }
}

/// Prints the message that [`get`](Owned::get) returns.
impl<D: ArenaDecode> fmt::Debug for Owned<D>
where
    for<'a> D::Message<'a>: fmt::Debug,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self.get(), f)
    }
}

/// Decode `input` into an [`Owned`] handle that owns the bytes and the arena.
///
/// `input` is taken by value (a `Vec<u8>`, `Box<[u8]>`, or a `&[u8]` it will copy). Prefer the
/// generated `T::decode_owned` for a named message; this is the type-parameter form.
///
/// Converting `input` allocates unless it is already a `Box<[u8]>` or a `Vec` with no spare
/// capacity, and that allocation aborts if the host is out of memory, as `Box` does.
///
/// # Errors
/// Returns the decoder's [`DecodeError`] (and drops the bytes/arena) for malformed or over-nested
/// input. (A generated decoder never panics; were one to unwind, the input bytes would leak — a
/// leak, never UB.)
pub fn decode_owned<D>(input: impl Into<Box<[u8]>>) -> Result<Owned<D>, DecodeError>
where
    D: ArenaDecode,
{
    // Own the input as a raw pointer BEFORE decoding, so the decode borrow derives from the raw
    // pointer (not from a `Box` whose later move would invalidate it).
    let input: NonNull<[u8]> = NonNull::from(Box::leak(input.into()));
    let arena = Arena::new();
    // SAFETY: `input` points to the live heap slice just created; the borrow is read-only and does
    // not escape this function (only the address, erased below, is kept).
    let decoded = D::decode_borrowed(unsafe { input.as_ref() }, &arena);
    let message = match decoded {
        Ok(message) => message,
        Err(e) => {
            // Decode failed: reclaim the input (the arena drops itself) and surface the error.
            // SAFETY: `input` came from `Box::leak` above and nothing borrows it any more.
            drop(unsafe { Box::from_raw(input.as_ptr()) });
            return Err(e);
        }
    };
    // Erase the decode borrow to `'static` (an address reinterpret; a message's layout does not
    // depend on its lifetime). Sound to STORE because the pointee is kept alive, unmoved, by the
    // owner below; `get` only ever reborrows it for a shorter lifetime.
    let root: NonNull<D::Message<'static>> = NonNull::from(message).cast();
    Ok(Owned { root, arena, input })
}

#[cfg(test)]
mod tests {
    use super::*;

    // A mock message borrowing from BOTH the input (`bytes`) and the arena (`len`), so the test
    // exercises Owned's ownership of each (not a real generated decoder).
    #[derive(Debug)]
    struct Mock<'a> {
        bytes: &'a [u8],
        len: &'a u32,
    }
    struct MockDecoder;

    // SAFETY: `Mock<'a>` holds only `&'a` shared references, into the input and into arena-allocated
    // memory -- covariant, layout-stable, no interior mutability over `'a`, no `&Arena`.
    unsafe impl ArenaDecode for MockDecoder {
        type Message<'a> = Mock<'a>;
        fn decode_borrowed<'a>(
            input: &'a [u8],
            arena: &'a Arena<'a>,
        ) -> Result<&'a Mock<'a>, DecodeError> {
            let len: &u32 =
                arena.emplace(|slot| Ok::<_, DecodeError>(slot.write(input.len() as u32)))?;
            let node = arena
                .emplace(|slot| Ok::<_, DecodeError>(slot.write(Mock { bytes: input, len })))?;
            Ok(node)
        }
    }

    #[test]
    fn owns_input_and_arena_across_moves() {
        let owned: Owned<MockDecoder> = decode_owned(&b"hello world"[..]).unwrap();
        // Move it (into a Vec, out again) to prove nothing dangles when the handle moves.
        let mut v = alloc::vec::Vec::new();
        v.push(owned);
        let owned = v.pop().unwrap();
        assert_eq!(owned.get().bytes, b"hello world"); // borrows the owned input
        assert_eq!(*owned.get().len, 11); // borrows the owned arena
    }

    #[test]
    fn debug_prints_the_message() {
        let owned: Owned<MockDecoder> = decode_owned(&b"hi"[..]).unwrap();
        assert_eq!(
            alloc::format!("{owned:?}"),
            "Mock { bytes: [104, 105], len: 2 }"
        );
    }

    #[test]
    fn moves_to_another_thread() {
        let owned: Owned<MockDecoder> = decode_owned(&b"moved"[..]).unwrap();
        let len = std::thread::spawn(move || *owned.get().len).join().unwrap();
        assert_eq!(len, 5);
    }

    #[test]
    fn threads_share_it_through_an_arc() {
        let owned: Owned<MockDecoder> = decode_owned(&b"shared"[..]).unwrap();
        let shared = std::sync::Arc::new(owned);
        let threads: alloc::vec::Vec<_> = (0..4)
            .map(|_| {
                let shared = std::sync::Arc::clone(&shared);
                std::thread::spawn(move || {
                    assert_eq!(shared.get().bytes, b"shared");
                    *shared.get().len
                })
            })
            .collect();
        for thread in threads {
            assert_eq!(thread.join().unwrap(), 6);
        }
    }

    #[test]
    fn drops_on_the_receiving_thread() {
        let (send, receive) = std::sync::mpsc::channel::<Owned<MockDecoder>>();
        let receiver = std::thread::spawn(move || {
            let owned = receive.recv().unwrap();
            assert_eq!(owned.get().bytes, b"sent");
        });
        send.send(decode_owned(&b"sent"[..]).unwrap()).unwrap();
        receiver.join().unwrap();
    }

    // A task that holds an arena and a message decoded into it across an `.await` is `Send`, so a
    // multi-threaded executor can run it. Checked at compile time, nothing runs.
    #[test]
    fn a_task_holding_an_arena_and_its_message_is_send() {
        fn assert_send<T: Send>(_: T) {}
        async fn later() {}
        assert_send(async {
            let arena = Arena::new();
            let message = MockDecoder::decode_borrowed(b"task", &arena).unwrap();
            later().await;
            assert_eq!(*message.len, 4);
        });
    }

    #[test]
    fn accepts_owned_and_borrowed_input() {
        let from_vec: Owned<MockDecoder> = decode_owned(alloc::vec![1u8, 2, 3]).unwrap();
        assert_eq!(from_vec.get().bytes, &[1, 2, 3]);
        let from_box: Owned<MockDecoder> =
            decode_owned(alloc::vec![4u8, 5].into_boxed_slice()).unwrap();
        assert_eq!(from_box.get().bytes, &[4, 5]);
    }
}
