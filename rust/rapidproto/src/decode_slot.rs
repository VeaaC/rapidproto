//! A branded write-once slot for in-place decode, and its initialization proof.
//!
//! The in-place helpers ([`Arena::emplace`](crate::Arena::emplace),
//! [`DecodeVec::emplace`](crate::DecodeVec::emplace), [`decode_value`](crate::decode_value)) hand a
//! decode closure a [`Slot`] over uninitialized storage and must then treat that storage as a live
//! `T` (publish it into a slice, return a `&mut T`, or move it out). For that to be sound **for any
//! safe closure**, the helper needs an *unforgeable* proof that the closure actually wrote the slot.
//!
//! The proof is [`Init`]: a zero-sized token carrying a brand lifetime `'id`. [`Slot::write`] is the
//! only way to obtain an `Init<'id>`, and it writes the slot. The helpers bind the closure as
//! `for<'id> FnOnce(Slot<'id, T>) -> Result<Init<'id>, E>`, so `'id` is a *fresh, invariant* brand
//! per call: the closure cannot possess an `Init<'id>` of that brand from anywhere else (a token of a
//! different brand does not coerce, and nothing but `write` constructs one). Hence an `Ok(Init)`
//! return is a compile-time proof the slot was initialized -- a leaked/foreign reference cannot stand
//! in for the token. This is the GhostToken/`generativity` branding technique.

use core::marker::PhantomData;
use core::mem::MaybeUninit;
use core::ptr::NonNull;

/// `PhantomData` that is **invariant** in `'id` (so no other lifetime, including `'static`, unifies
/// with a fresh brand).
type Brand<'id> = PhantomData<fn(&'id ()) -> &'id ()>;

/// A write-once handle to one uninitialized `T` slot, branded with `'id`. Obtained only from an
/// in-place helper; its sole consuming operation is [`write`](Self::write), which initializes the
/// slot and yields the matching [`Init`] proof.
pub struct Slot<'id, T> {
    ptr: NonNull<T>,
    // Invariant in `'id` (unforgeable brand) and in `T` (`*mut T`); also makes `Slot` neither `Send`
    // nor `Sync`, matching an exclusive borrow of uninitialized storage.
    _marker: PhantomData<(Brand<'id>, *mut T)>,
}

impl<'id, T> Slot<'id, T> {
    /// Mint a slot over `ptr`.
    ///
    /// # Safety
    /// `ptr` must point at properly aligned storage for one `T` that is valid to write for the
    /// duration of this `Slot`, and that the caller alone accesses. (Crate-internal: the helpers mint
    /// slots over a fresh arena allocation / a reserved `DecodeVec` tail / a stack `MaybeUninit`.)
    #[inline]
    pub(crate) unsafe fn new(ptr: NonNull<T>) -> Self {
        Slot {
            ptr,
            _marker: PhantomData,
        }
    }

    /// Initialize the slot with `value`, consuming the slot and returning the [`Init`] proof.
    #[inline]
    pub fn write(self, value: T) -> Init<'id> {
        // SAFETY: `new`'s contract guarantees `ptr` is aligned storage for one `T` this `Slot`
        // uniquely owns; `ptr::write` initializes it without dropping the previous (uninitialized)
        // contents.
        unsafe { self.ptr.as_ptr().write(value) };
        Init {
            _marker: PhantomData,
        }
    }
}

/// Proof that the [`Slot`] of the same brand `'id` was written. A zero-sized token constructible only
/// by [`Slot::write`]; it cannot be forged, so a helper that receives `Init<'id>` from its closure
/// knows the branded slot is initialized.
///
/// The forge that broke the earlier `-> &mut T` witness -- a safe closure returning `Ok` without ever
/// writing the slot -- is a compile error now: there is no way to produce an `Init<'id>` of the
/// freshly-branded slot except by writing it. A closure that tries to return some *other* `Init`
/// cannot type-check, because the brand is invariant and fresh per call:
///
/// ```compile_fail
/// use rapidproto::{decode_value, Init, Slot};
/// // Try to satisfy the `Init` proof without writing the slot by smuggling in a token of another
/// // brand. The `for<'id>` bound makes the slot's brand fresh and invariant, so no outside `Init`
/// // unifies with it: this does not compile.
/// fn forge<'other>(stolen: Init<'other>) -> u64 {
///     decode_value::<u64, (), _>(|_slot: Slot<'_, u64>| Ok(stolen)).unwrap()
/// }
/// ```
pub struct Init<'id> {
    _marker: Brand<'id>,
}

/// Decode a `T` **by value** in place: run `f` against a stack slot and, on its [`Init`] proof,
/// return the now-initialized value by move. Lets generated map-value / by-value sites use the
/// branded-slot decode form while keeping all `unsafe` in the runtime. Sound for any safe `f` (the
/// `Init` proof is unforgeable); does not allocate.
///
/// `#[inline(always)]`: like the other in-place helpers ([`Arena::emplace`](crate::Arena::emplace),
/// [`DecodeVec::emplace`](crate::DecodeVec::emplace)), this must inline into the caller so the decoded
/// payload slice stays in registers rather than being spilled through the closure environment -- an
/// out-of-line call forces the 16-byte slice to memory and the wide reload cannot store-forward.
#[inline(always)]
pub fn decode_value<T, E, F>(f: F) -> Result<T, E>
where
    F: for<'id> FnOnce(Slot<'id, T>) -> Result<Init<'id>, E>,
{
    let mut uninit = MaybeUninit::<T>::uninit();
    // SAFETY: `uninit` is aligned storage for one `T`, owned here, valid for the slot's use below.
    let slot = unsafe { Slot::new(NonNull::new_unchecked(uninit.as_mut_ptr())) };
    let _init: Init<'_> = f(slot)?;
    // SAFETY: `f` returned `Init<'id>` of the slot's brand, obtainable only via `Slot::write` on that
    // slot (the brand is fresh per call and cannot be forged), so `uninit` is initialized.
    Ok(unsafe { uninit.assume_init() })
}
