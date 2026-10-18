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

use core::marker::PhantomData;
use core::ptr::NonNull;

use alloc::boxed::Box;

use crate::{Arena, DecodeError};

/// How [`decode_owned`] decodes a message type whose borrow it will own. Generated code implements it
/// on a zero-size per-message marker: `type Message` names the message at the borrow `'a`, and
/// `decode_borrowed` is the ordinary `T::decode`. A lifetime-parameter trait + a `for<'a>` bound (not
/// a GAT) names "the message at any lifetime" on a stable-for-years rustc.
pub trait ArenaDecode<'a> {
    /// The decoded message, borrowing the input and arena for `'a`.
    type Message: 'a;

    /// Decode the whole message from `input`, allocating its tree in `arena`.
    ///
    /// # Errors
    /// Returns the decoder's [`DecodeError`] for malformed or over-nested input.
    fn decode_borrowed(
        input: &'a [u8],
        arena: &'a Arena<'a>,
    ) -> Result<&'a Self::Message, DecodeError>;
}

/// A decoded message that owns everything it borrows: the input bytes and the [`Arena`] holding the
/// tree. Construct it with [`decode_owned`] (or a generated `T::decode_owned`); read the message with
/// [`get`](Owned::get). `'static`-capable — it carries no borrowed lifetime.
pub struct Owned<D: for<'a> ArenaDecode<'a>> {
    // The decoded root, with its borrow erased to `'static`. It points INTO `arena`'s chunks and
    // `input`'s bytes, both owned below and never moved/mutated/freed until this `Owned` drops, so
    // the pointee outlives every borrow `get` hands out. `get` re-borrows it for the shorter `&self`
    // lifetime (`Message` is covariant in its lifetime, so the widening is sound). First in
    // declaration order, but it owns nothing — drop order among the fields is irrelevant (the root
    // is never read at drop, and `arena`/`input` do not reference each other).
    root: NonNull<<D as ArenaDecode<'static>>::Message>,
    // `Arena<'static>` heap-owns its chunks (address-stable across the move into this struct); the
    // tree's pointers point into those chunks, not into the arena struct itself. Read only through
    // `root` and dropped at the end -- it exists to OWN the tree's backing, so `dead_code` (which
    // ignores Drop) would flag it.
    #[allow(dead_code)]
    arena: Arena<'static>,
    // The input bytes, owned as a RAW pointer (from `Box::into_raw`), not a `Box`: a `Box` asserts
    // unique access, so moving this struct would retag the heap block and invalidate the root's
    // borrow into it (Miri/Stacked Borrows). A raw pointer makes the ownership move-safe -- the same
    // trick the arena uses for its chunks. Freed once in `Drop`.
    input: NonNull<[u8]>,
    _marker: PhantomData<D>,
}

impl<D: for<'a> ArenaDecode<'a>> Drop for Owned<D> {
    fn drop(&mut self) {
        // SAFETY: `input` came from `Box::into_raw` in `decode_owned` and is reclaimed exactly once,
        // here; `root` (which points into it) is never used after drop begins.
        drop(unsafe { Box::from_raw(self.input.as_ptr()) });
    }
}

impl<D: for<'a> ArenaDecode<'a>> Owned<D> {
    /// The decoded message, borrowed for as long as this handle is. Call accessors on it as usual.
    #[must_use]
    pub fn get(&self) -> &<D as ArenaDecode<'_>>::Message {
        // SAFETY: `root` points into `self.arena`/`self.input`, both owned by `self` and unmoved and
        // unmutated since construction, so the pointee is live and valid for all of `&self`. The
        // returned reference is tied to `&self` (elision), so it cannot outlive the owner. The stored
        // pointee type is `Message` at `'static`; `Message` is covariant in that lifetime, so reading
        // it as `Message` at the shorter `&self` lifetime is sound.
        unsafe { self.root.cast::<<D as ArenaDecode<'_>>::Message>().as_ref() }
    }
}

/// Decode `input` into an [`Owned`] handle that owns the bytes and the arena.
///
/// `input` is taken by value (a `Vec<u8>`, `Box<[u8]>`, or a `&[u8]` it will copy). Prefer the
/// generated `T::decode_owned` for a named message; this is the type-parameter form.
///
/// # Errors
/// Returns the decoder's [`DecodeError`] (and drops the bytes/arena) for malformed or over-nested
/// input.
pub fn decode_owned<D>(input: impl Into<Box<[u8]>>) -> Result<Owned<D>, DecodeError>
where
    D: for<'a> ArenaDecode<'a>,
{
    // Own the input as a raw pointer BEFORE decoding, so the decode borrow derives from the raw
    // pointer (not from a `Box` whose later move would invalidate it). `Box::into_raw` never returns
    // null.
    let input: NonNull<[u8]> =
        NonNull::new(Box::into_raw(input.into())).expect("Box ptr is non-null");
    let arena = Arena::new();
    // SAFETY: `input` points to the live heap slice just created; the borrow is read-only and does
    // not escape this function (only the address, erased below, is kept).
    let decoded = D::decode_borrowed(unsafe { input.as_ref() }, &arena);
    let message = match decoded {
        Ok(message) => message,
        Err(e) => {
            // Decode failed: reclaim the input (the arena drops itself) and surface the error.
            drop(unsafe { Box::from_raw(input.as_ptr()) });
            return Err(e);
        }
    };
    // Erase the decode borrow to `'static` (an address reinterpret; a message's layout does not
    // depend on its lifetime). Sound to STORE because the pointee is kept alive, unmoved, by the
    // owner below; `get` only ever reborrows it for a shorter lifetime.
    let root: NonNull<<D as ArenaDecode<'static>>::Message> = NonNull::from(message).cast();
    Ok(Owned {
        root,
        arena,
        input,
        _marker: PhantomData,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // A mock message borrowing from BOTH the input (`bytes`) and the arena (`len`), so the test
    // exercises Owned's ownership of each (not a real generated decoder).
    struct Mock<'a> {
        bytes: &'a [u8],
        len: &'a u32,
    }
    struct MockDecoder;

    fn oom(_: crate::ArenaExhausted) -> DecodeError {
        DecodeError::new(crate::ErrorKind::InputTooLarge, 0)
    }

    impl<'a> ArenaDecode<'a> for MockDecoder {
        type Message = Mock<'a>;
        fn decode_borrowed(
            input: &'a [u8],
            arena: &'a Arena<'a>,
        ) -> Result<&'a Mock<'a>, DecodeError> {
            let len: &u32 = arena.alloc(input.len() as u32).map_err(oom)?;
            let node = arena.alloc(Mock { bytes: input, len }).map_err(oom)?;
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
    fn accepts_owned_and_borrowed_input() {
        let from_vec: Owned<MockDecoder> = decode_owned(alloc::vec![1u8, 2, 3]).unwrap();
        assert_eq!(from_vec.get().bytes, &[1, 2, 3]);
        let from_box: Owned<MockDecoder> =
            decode_owned(alloc::vec![4u8, 5].into_boxed_slice()).unwrap();
        assert_eq!(from_box.get().bytes, &[4, 5]);
    }
}
