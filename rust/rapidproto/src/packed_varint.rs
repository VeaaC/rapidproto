//! SWAR packed-varint decoder -- the Rust mirror of the C++ decoder's `decode_packed_varints`
//! (include/rapidproto/runtime.hpp). A large packed-varint span is decoded word-at-a-time with a
//! width-classified kernel instead of the per-element byte loop: on irregular/narrow varints the
//! branchless kernels avoid the byte loop's mispredictions, which is where the win comes from.
//!
//! The hot kernels use raw-pointer loads that deliberately read a full 8 (or 64) bytes at a time,
//! including bytes past the current varint. That is sound only inside a window the caller has already
//! bounds-checked (`end - q >= 8` / `>= 64` / `>= 72`); every such read is guarded. The non-hot
//! paths (the scalar tail, and the 9/10-byte cases) reuse the validated `wire::read_varint_full`, so the
//! malformed-input rules (truncation, overflow) live in one place. The kernel never aborts mid-span
//! on a *store* -- only a malformed varint stops it -- so the bulk stores stay vectorizable.
//!
//! This is arena-only (the streaming decoder can't vectorize a per-element callback store, matching
//! the C++ split). Decoding into a pre-sized [`DecodeVec`] via [`DecodeVec::fill_packed_varints`].

use core::ptr;

use crate::decode_vec::DecodeVec;
use crate::error::{DecodeError, ErrorKind};
use crate::wire::read_varint_full;

/// Span floor below which the kernels never engage: below this the classify scan and kernel dispatch
/// are not amortized, so tiny spans take the plain scalar loop. Matches C++ `kPackedKernelMinSpan`.
const KERNEL_MIN_SPAN: usize = 256;

// ── SWAR primitives (mirror of C++ `swar_detail`) ────────────────────────────────────────────────
mod swar {
    /// MSB of each byte.
    pub const MSB: u64 = 0x8080_8080_8080_8080;
    /// Low 7 bits of each byte.
    pub const LOW7: u64 = 0x7F7F_7F7F_7F7F_7F7F;

    /// Mask of the low `n` bytes (`n` in 1..=8).
    #[inline]
    pub const fn low_byte_mask(n: usize) -> u64 {
        if n >= 8 {
            u64::MAX
        } else {
            (1u64 << (8 * n)) - 1
        }
    }

    /// Load 8 bytes at `p` as a little-endian-logical `u64` (byte i in bits 8i..8i+7).
    ///
    /// # Safety
    /// `p` must be valid for an 8-byte read (the caller guarantees `>= 8` bytes remain).
    #[inline]
    pub unsafe fn load64(p: *const u8) -> u64 {
        // SAFETY: caller guarantees 8 readable bytes at `p`; `read_unaligned` needs no alignment.
        let w = unsafe { core::ptr::read_unaligned(p.cast::<u64>()) };
        // On a little-endian host the raw load IS the logical value; otherwise swap the bytes back
        // into logical order so the masks/compaction below are correct on any host.
        if cfg!(target_endian = "little") {
            w
        } else {
            w.swap_bytes()
        }
    }

    /// Gather the low 7 bits of each byte into a contiguous value (three SWAR compaction rounds).
    #[inline]
    pub fn compact7(x: u64) -> u64 {
        let x = ((x & 0x7F00_7F00_7F00_7F00) >> 1) | (x & 0x007F_007F_007F_007F);
        let x = ((x & 0x3FFF_0000_3FFF_0000) >> 2) | (x & 0x0000_3FFF_0000_3FFF);
        ((x & 0x0FFF_FFFF_0000_0000) >> 4) | (x & 0x0000_0000_0FFF_FFFF)
    }

    /// Movemask without SIMD: gather the MSB of each byte into 8 bits (bit i = MSB of byte i).
    #[inline]
    pub fn bytemask8(w: u64) -> u64 {
        ((w & MSB).wrapping_mul(0x0002_0408_1020_4081)) >> 56
    }

    /// 64-bit terminator mask over the 64 bytes at `p` (bit i set = byte i terminates a varint).
    ///
    /// # Safety
    /// `p` must be valid for a 64-byte read.
    #[inline]
    pub unsafe fn stopmask64(p: *const u8) -> u64 {
        let mut m = 0u64;
        for j in 0..8usize {
            // SAFETY: caller guarantees 64 readable bytes, so each `p + 8j` (j < 8) has 8 bytes.
            let w = unsafe { load64(p.add(8 * j)) };
            m |= bytemask8(!w) << (8 * j);
        }
        m
    }
}

/// Which kernel to decode a packed-varint span with. A wrong guess is only slower, never incorrect.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Kernel {
    ByteLoop, // small span or overlong/unclassifiable: the scalar loop
    Swar,     // wide-mixed / 1-byte-dominant: per-element word-at-a-time read (1-byte fast path)
    Struct,   // narrow-mixed (max width <= 5): one stopmask per block, then independent compact7s
    Bulk1,    // homogeneous 1-byte: widen 64 bytes at once
    Bulk2,    // homogeneous 2-byte: four varints per 8-byte word
    Fixed,    // homogeneous 3..=10-byte: fixed-stride decode
}

/// Classify the width regime from the first 64-byte window at `p` (`span` bytes remain). For a
/// `Fixed` result the homogeneous width is returned; it is unspecified otherwise. Mirror of C++
/// `packed_strategy`.
///
/// # Safety
/// `p` must be valid for a 64-byte read when `span >= KERNEL_MIN_SPAN` (guaranteed by the floor).
unsafe fn classify(p: *const u8, span: usize) -> (Kernel, usize) {
    if span < KERNEL_MIN_SPAN {
        return (Kernel::ByteLoop, 0);
    }
    // SAFETY: span >= KERNEL_MIN_SPAN (256) >= 64, so 64 bytes are readable at `p`.
    let stop = unsafe { swar::stopmask64(p) };
    if stop == 0 {
        // No terminator in 64 bytes: a varint longer than the window (overlong/malformed) -> the
        // validating scalar loop rejects it.
        return (Kernel::ByteLoop, 0);
    }
    // Scan every terminator: track the first varint's width (homogeneity) and the max width.
    let mut m = stop;
    let mut last: i32 = -1;
    let mut g0: i32 = -1;
    let mut maxw = 0i32;
    let mut homo = true;
    while m != 0 {
        let b = m.trailing_zeros() as i32;
        m &= m - 1;
        let w = b - last;
        last = b;
        if w > maxw {
            maxw = w;
        }
        if g0 < 0 {
            g0 = w;
        } else if w != g0 {
            homo = false;
        }
    }
    if !homo {
        return if maxw <= 5 {
            (Kernel::Struct, 0)
        } else {
            (Kernel::Swar, 0)
        };
    }
    match g0 {
        1 => (Kernel::Bulk1, 0),
        2 => (Kernel::Bulk2, 0),
        3..=10 => (Kernel::Fixed, g0 as usize),
        _ => (Kernel::ByteLoop, 0), // g0 > 10: an overlong (malformed) varint -> scalar loop
    }
}

/// Read one varint from `[q, end)` via the validated scalar reader. Returns the value and the
/// advanced pointer, or the error kind.
///
/// # Safety
/// `q <= end` and both point into one allocation (the packed span).
///
/// `#[inline(always)]`, not `#[inline]`: inlining the full varint decode (`read_varint_full`) grows
/// this body past LLVM's inline threshold, so a plain hint lets it be out-of-lined -- and then every
/// scalar element (packed small-span fill, SWAR tail) pays a per-element call, which regresses large
/// packed spans (e.g. `big_osm`). Forcing the inline keeps the element read folded into the caller's
/// loop (as it was before the full-varint change, and as the C++ RP_FLATTENed small kernel does).
#[inline(always)]
unsafe fn read_one(q: *const u8, end: *const u8) -> Result<(u64, *const u8), ErrorKind> {
    // SAFETY: q <= end in the same span, so this is a valid subslice of the input.
    let rest = unsafe { core::slice::from_raw_parts(q, end.offset_from(q) as usize) };
    let mut raw = 0u64;
    // The fully-inlined reader: this is a tight per-element loop over a packed span, so inlining the
    // whole multibyte decode (no out-of-line #[cold] tail) avoids a per-element call on multibyte
    // values -- the dominant cost on a multibyte-heavy packed field (e.g. the `wide` bench).
    let after = read_varint_full(rest, &mut raw)?;
    Ok((raw, after.as_ptr()))
}

/// Read one varint from `[q, end)` word-at-a-time: a 1-byte value takes a straight-line fast path, a
/// 2..=8-byte value is decoded branchlessly from one 8-byte load, and a 9/10-byte value (all 8 low
/// bytes continue) falls to [`read_one`], which also reports overflow and truncation. Mirror of C++
/// `read_varint_packed`.
///
/// # Safety
/// `q <= end` in one span, with at least 8 bytes in `[q, end)`.
#[inline(always)]
unsafe fn read_one_swar(q: *const u8, end: *const u8) -> Result<(u64, *const u8), ErrorKind> {
    // SAFETY: >= 8 bytes remain at q.
    let b = unsafe { *q };
    if b < 0x80 {
        return Ok((u64::from(b), unsafe { q.add(1) }));
    }
    let w = unsafe { swar::load64(q) };
    let stop = !w & swar::MSB; // the MSB of each terminating byte
    if stop == 0 {
        return unsafe { read_one(q, end) };
    }
    let len = (stop.trailing_zeros() >> 3) as usize + 1;
    let mask = stop ^ (stop - 1); // every bit up to and including the first terminator's MSB
    Ok((swar::compact7(w & mask & swar::LOW7), unsafe { q.add(len) }))
}

/// Store `conv(raw)` at `out[i]`.
///
/// # Safety
/// `i < cap` for the reserved array `out` (the caller sized `out` to the span's element upper bound).
#[inline]
unsafe fn put<T, F: FnMut(u64) -> T>(out: *mut T, i: usize, raw: u64, conv: &mut F) {
    // SAFETY: `i` is within the reserved capacity; the slot is uninitialized, so `write` (no drop).
    unsafe { ptr::write(out.add(i), conv(raw)) };
}

/// Fixed-stride decode of homogeneous `W`-byte varints (`W` in 3..=8). Guards each element with a
/// full `W`-byte continuation-pattern compare and stops on the first width change. Mirror of C++
/// `fixed_fill`. Returns the advanced `q`; `*n` is bumped per element written.
///
/// # Safety
/// `[q, end)` is a valid range in the span; `out` has room for every index written (`*n` stays below
/// the reserved capacity because element count never exceeds span bytes).
unsafe fn fixed_fill<const W: usize, T, F: FnMut(u64) -> T>(
    mut q: *const u8,
    end: *const u8,
    n: &mut usize,
    out: *mut T,
    conv: &mut F,
) -> *const u8 {
    let len_mask = swar::low_byte_mask(W);
    let msb_w = swar::MSB & len_mask;
    // The one accepted pattern: bytes 0..W-2 continue (MSB set), byte W-1 terminates (MSB clear).
    let one_varint = msb_w ^ (0x80u64 << (8 * (W - 1)));
    if W <= 4 {
        // Two W-byte varints (2W <= 8) fit in one load -> decode both per load.
        let msb2 = msb_w | (msb_w << (8 * W));
        let two_varints = one_varint | (one_varint << (8 * W));
        while (end as usize).wrapping_sub(q as usize) >= 8 {
            // SAFETY: >= 8 bytes remain at q.
            let w = unsafe { swar::load64(q) };
            if (w & msb2) == two_varints {
                let r0 = swar::compact7(w & len_mask & swar::LOW7);
                let r1 = swar::compact7((w >> (8 * W)) & len_mask & swar::LOW7);
                // SAFETY: *n and *n+1 are within capacity (<= span bytes).
                unsafe {
                    put(out, *n, r0, conv);
                    put(out, *n + 1, r1, conv);
                }
                *n += 2;
                q = unsafe { q.add(2 * W) };
            } else if (w & msb_w) == one_varint {
                let r = swar::compact7(w & len_mask & swar::LOW7);
                unsafe { put(out, *n, r, conv) };
                *n += 1;
                q = unsafe { q.add(W) };
            } else {
                break;
            }
        }
    } else {
        while (end as usize).wrapping_sub(q as usize) >= 8 {
            // SAFETY: >= 8 bytes remain at q.
            let w = unsafe { swar::load64(q) };
            if (w & msb_w) != one_varint {
                break;
            }
            let r = swar::compact7(w & len_mask & swar::LOW7);
            unsafe { put(out, *n, r, conv) };
            *n += 1;
            q = unsafe { q.add(W) };
        }
    }
    q
}

/// Fixed-stride decode of homogeneous 9/10-byte varints. These span more than one word, so the value
/// is assembled from the low 8 bytes plus the high byte(s), applying `read_varint`'s overflow rule.
/// Mirror of C++ `fixed_fill_wide`.
///
/// # Safety
/// As [`fixed_fill`].
unsafe fn fixed_fill_wide<const W: usize, T, F: FnMut(u64) -> T>(
    mut q: *const u8,
    end: *const u8,
    n: &mut usize,
    out: *mut T,
    conv: &mut F,
) -> *const u8 {
    debug_assert!(W == 9 || W == 10);
    while (end as usize).wrapping_sub(q as usize) >= W {
        // SAFETY: >= W >= 9 bytes remain, so the 8-byte load and q[8]/q[9] are in range.
        let w0 = unsafe { swar::load64(q) };
        if (w0 & swar::MSB) != swar::MSB {
            break; // a byte in 0..7 terminates -> narrower than W
        }
        let mut value = swar::compact7(w0 & swar::LOW7); // bits 0..55
        let b8 = unsafe { *q.add(8) };
        if W == 9 {
            if (b8 & 0x80) != 0 {
                break; // byte 8 continues -> wider than 9
            }
            value |= u64::from(b8 & 0x7F) << 56;
        } else {
            let b9 = unsafe { *q.add(9) };
            if (b8 & 0x80) == 0 || (b9 & 0x80) != 0 {
                break; // not exactly 10 bytes
            }
            if b9 > 1 {
                break; // 10th byte > 1 overflows -> let the scalar tail report it
            }
            value |= u64::from(b8 & 0x7F) << 56;
            value |= u64::from(b9) << 63;
        }
        unsafe { put(out, *n, value, conv) };
        *n += 1;
        q = unsafe { q.add(W) };
    }
    q
}

/// Decode a whole packed-varint span into `out`, applying `conv` per raw value. Returns the element
/// count, or `(kind, offset-within-span)` on a malformed varint. Mirror of C++
/// `decode_packed_varints`. Out-of-line (the kernel is large): only spans `>= KERNEL_MIN_SPAN` reach
/// it; smaller spans take the inline [`decode_scalar`] directly from `fill_packed_varints`.
///
/// # Safety
/// `out` must have room for at least `span.len()` elements (the element-count upper bound).
unsafe fn decode_kernel<T, F: FnMut(u64) -> T>(
    span: &[u8],
    out: *mut T,
    conv: &mut F,
) -> Result<usize, (ErrorKind, usize)> {
    let begin = span.as_ptr();
    let end = unsafe { begin.add(span.len()) };
    let mut q = begin;
    let mut n = 0usize;
    let avail = |q: *const u8| (end as usize).wrapping_sub(q as usize);

    loop {
        let q_start = q;
        // SAFETY: avail(q) >= 256 here (loop re-entry guards below keep it so), 64 bytes readable.
        let (kern, width) = unsafe { classify(q, avail(q)) };
        match kern {
            Kernel::Bulk1 => {
                while avail(q) >= 64 {
                    let mut any = 0u64;
                    for j in 0..8usize {
                        any |= unsafe { swar::load64(q.add(8 * j)) } & swar::MSB;
                    }
                    if any == 0 {
                        for i in 0..64usize {
                            let byte = unsafe { *q.add(i) };
                            unsafe { put(out, n + i, u64::from(byte), conv) };
                        }
                        q = unsafe { q.add(64) };
                        n += 64;
                    } else {
                        break;
                    }
                }
            }
            Kernel::Bulk2 => {
                while avail(q) >= 8 {
                    let w = unsafe { swar::load64(q) };
                    if (w & swar::MSB) == 0x0080_0080_0080_0080 {
                        let x = w & swar::LOW7;
                        let y = (x & 0x007F_007F_007F_007F) | ((x & 0x7F00_7F00_7F00_7F00) >> 1);
                        unsafe {
                            put(out, n, u64::from(y as u16), conv);
                            put(out, n + 1, u64::from((y >> 16) as u16), conv);
                            put(out, n + 2, u64::from((y >> 32) as u16), conv);
                            put(out, n + 3, u64::from((y >> 48) as u16), conv);
                        }
                        q = unsafe { q.add(8) };
                        n += 4;
                    } else {
                        break;
                    }
                }
            }
            Kernel::Fixed => {
                q = unsafe {
                    match width {
                        3 => fixed_fill::<3, _, _>(q, end, &mut n, out, conv),
                        4 => fixed_fill::<4, _, _>(q, end, &mut n, out, conv),
                        5 => fixed_fill::<5, _, _>(q, end, &mut n, out, conv),
                        6 => fixed_fill::<6, _, _>(q, end, &mut n, out, conv),
                        7 => fixed_fill::<7, _, _>(q, end, &mut n, out, conv),
                        8 => fixed_fill::<8, _, _>(q, end, &mut n, out, conv),
                        9 => fixed_fill_wide::<9, _, _>(q, end, &mut n, out, conv),
                        _ => fixed_fill_wide::<10, _, _>(q, end, &mut n, out, conv),
                    }
                };
            }
            Kernel::Struct => {
                while avail(q) >= 72 {
                    let mut t = unsafe { swar::stopmask64(q) };
                    if t == 0 {
                        break;
                    }
                    let mut start = 0i32;
                    loop {
                        let b = t.trailing_zeros() as i32;
                        t &= t - 1;
                        let len = (b - start + 1) as usize;
                        if len <= 8 {
                            let lenmask = swar::low_byte_mask(len);
                            let w = unsafe { swar::load64(q.add(start as usize)) };
                            let r = swar::compact7(w & lenmask & swar::LOW7);
                            unsafe { put(out, n, r, conv) };
                            n += 1;
                        } else {
                            // 9/10-byte varint: decode and validate with the scalar reader.
                            let (raw, _) = unsafe { read_one(q.add(start as usize), end) }
                                .map_err(|k| {
                                    (k, unsafe {
                                        q.add(start as usize).offset_from(begin) as usize
                                    })
                                })?;
                            unsafe { put(out, n, raw, conv) };
                            n += 1;
                        }
                        start = b + 1;
                        if t == 0 {
                            break;
                        }
                    }
                    q = unsafe { q.add(start as usize) };
                }
            }
            Kernel::Swar => {
                while avail(q) >= 8 {
                    let (raw, next) = unsafe { read_one_swar(q, end) }
                        .map_err(|k| (k, unsafe { q.offset_from(begin) as usize }))?;
                    q = next;
                    unsafe { put(out, n, raw, conv) };
                    n += 1;
                }
            }
            Kernel::ByteLoop => {}
        }
        if avail(q) < 512 {
            break; // little left -> the scalar tail finishes it
        }
        if (q as usize).wrapping_sub(q_start as usize) < 4096 {
            break; // kernel got no traction (churny data / ByteLoop pick) -> scalar tail
        }
        // a homogeneous kernel ran a long stretch then hit a width change: re-classify remainder
    }

    // Finish the remainder after the kernel (or a ByteLoop pick) with the inline scalar loop.
    let consumed = unsafe { q.offset_from(begin) } as usize;
    // SAFETY: `out + n` has room for the remaining elements (<= span.len() total); `consumed` bytes
    // were consumed, so the remaining span is `span[consumed..]`.
    let tail = unsafe { decode_scalar(&span[consumed..], out.add(n), conv) }
        .map_err(|(k, off)| (k, consumed + off))?;
    Ok(n + tail)
}

/// The validating scalar varint loop over a whole span -- both the small-span path (kept `#[inline]`
/// so it folds into the generated decoder for tiny packed fields, with no out-of-line call to
/// amortize) and the remainder tail of [`decode_kernel`]. Returns the element count or
/// `(kind, offset-within-span)`.
///
/// # Safety
/// `out` must have room for at least `span.len()` elements (the element-count upper bound).
#[inline]
unsafe fn decode_scalar<T, F: FnMut(u64) -> T>(
    span: &[u8],
    out: *mut T,
    conv: &mut F,
) -> Result<usize, (ErrorKind, usize)> {
    let begin = span.as_ptr();
    let end = unsafe { begin.add(span.len()) };
    let mut q = begin;
    let mut n = 0usize;
    while q < end {
        let (raw, next) = unsafe { read_one(q, end) }
            .map_err(|k| (k, unsafe { q.offset_from(begin) as usize }))?;
        q = next;
        unsafe { put(out, n, raw, conv) };
        n += 1;
    }
    Ok(n)
}

impl<'a, T> DecodeVec<'a, T> {
    /// Decode a whole packed-varint `span` into `self`, applying `conv` to each raw value, with
    /// `base_off` the span's byte offset within the message body (for error reporting).
    ///
    /// Reserves the span's element upper bound (each varint is >= 1 byte), fills it with the SWAR
    /// kernel (large spans) or the scalar loop (small spans), then trims the over-estimate -- the
    /// Rust mirror of the C++ decoder's `emit_packed_fill` + `decode_packed_varints`. Generated
    /// decoders call this for every packed-varint field; the only `unsafe` stays here, not in them.
    #[inline]
    pub fn fill_packed_varints<F>(
        &mut self,
        span: &[u8],
        base_off: usize,
        mut conv: F,
    ) -> Result<(), DecodeError>
    where
        F: FnMut(u64) -> T,
    {
        if span.is_empty() {
            return Ok(());
        }
        self.reserve(span.len())
            .map_err(|_| DecodeError::new(ErrorKind::ArenaExhausted, 0))?;
        let tail = self.spare_capacity_ptr();
        // SAFETY: `reserve(span.len())` guarantees `span.len()` free slots at `tail`, and the element
        // count never exceeds `span.len()` (>= 1 byte each), so every write is in bounds. Small spans
        // take the inline scalar loop (nothing to amortize an out-of-line call); large spans go
        // through the out-of-line SWAR kernel. Mirrors C++ `emit_packed_fill`'s span-size split.
        let written = if span.len() >= KERNEL_MIN_SPAN {
            unsafe { decode_kernel(span, tail, &mut conv) }
        } else {
            unsafe { decode_scalar(span, tail, &mut conv) }
        }
        .map_err(|(k, off)| DecodeError::new(k, base_off + off))?;
        // SAFETY: `written` elements were initialized contiguously at `tail`.
        unsafe { self.advance_len(written) };
        self.shrink_to_fit();
        Ok(())
    }
}

#[cfg(all(test, feature = "alloc"))]
mod tests {
    use super::*;
    use crate::arena::Arena;
    use crate::wire::read_varint;

    // Encode `v` as a LEB128 varint into `out`.
    fn put_varint(out: &mut alloc::vec::Vec<u8>, mut v: u64) {
        loop {
            let b = (v & 0x7F) as u8;
            v >>= 7;
            if v != 0 {
                out.push(b | 0x80);
            } else {
                out.push(b);
                break;
            }
        }
    }

    fn decode_u64(span: &[u8]) -> alloc::vec::Vec<u64> {
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        v.fill_packed_varints(span, 0, |raw| raw).unwrap();
        v.into_slice().to_vec()
    }

    // The reference: the plain scalar loop, which the kernels must match for every input.
    fn reference(span: &[u8]) -> alloc::vec::Vec<u64> {
        let mut out = alloc::vec::Vec::new();
        let mut rest = span;
        while !rest.is_empty() {
            let mut raw = 0u64;
            rest = read_varint(rest, &mut raw).unwrap();
            out.push(raw);
        }
        out
    }

    fn check(values: &[u64]) {
        let mut span = alloc::vec::Vec::new();
        for &x in values {
            put_varint(&mut span, x);
        }
        assert_eq!(decode_u64(&span), values, "span len {}", span.len());
        assert_eq!(decode_u64(&span), reference(&span));
    }

    #[test]
    fn empty_span() {
        assert_eq!(decode_u64(&[]), alloc::vec::Vec::<u64>::new());
    }

    #[test]
    fn small_span_uses_scalar_loop() {
        check(&[0, 1, 127, 128, 300, 16384, u64::MAX]);
    }

    #[test]
    fn homogeneous_1_byte_bulk1() {
        // > KERNEL_MIN_SPAN one-byte varints -> Bulk1.
        let vals: alloc::vec::Vec<u64> = (0..2000u64).map(|i| i % 128).collect();
        check(&vals);
    }

    #[test]
    fn homogeneous_2_byte_bulk2() {
        // Values 128..=16383 all encode in 2 bytes -> Bulk2.
        let vals: alloc::vec::Vec<u64> = (0..2000u64).map(|i| 128 + (i % 16000)).collect();
        check(&vals);
    }

    #[test]
    fn homogeneous_fixed_widths_3_through_10() {
        // Pick a value whose encoding is exactly W bytes for each W in 3..=10.
        let per_width = [
            (3usize, 1u64 << 14), // 16384 -> 3 bytes
            (4, 1 << 21),
            (5, 1 << 28),
            (6, 1 << 35),
            (7, 1 << 42),
            (8, 1 << 49),
            (9, 1 << 56),
            (10, 1 << 63),
        ];
        for (w, base) in per_width {
            let vals: alloc::vec::Vec<u64> = (0..1500u64).map(|i| base + (i & 0x3F)).collect();
            check(&vals);
            let _ = w;
        }
    }

    #[test]
    fn narrow_mixed_struct() {
        // Mixed small widths (1..=5 bytes), max <= 5 -> Struct.
        let vals: alloc::vec::Vec<u64> = (0..3000u64)
            .map(|i| match i % 5 {
                0 => i % 100,           // 1 byte
                1 => 200 + (i % 10000), // 2-3 bytes
                2 => 1 << 20,           // 3 bytes
                3 => 1 << 24,           // 4 bytes
                _ => 1 << 30,           // 5 bytes
            })
            .collect();
        check(&vals);
    }

    // Encode `values` as one packed span, asserting the classifier sends it to the Swar kernel.
    fn swar_span(values: &[u64]) -> alloc::vec::Vec<u8> {
        let mut span = alloc::vec::Vec::new();
        for &x in values {
            put_varint(&mut span, x);
        }
        assert!(unsafe { classify(span.as_ptr(), span.len()) }.0 == Kernel::Swar);
        span
    }

    // A Swar span of 1-byte values with every third one 10 bytes wide.
    fn swar_prefix() -> alloc::vec::Vec<u8> {
        swar_span(
            &(0..100u64)
                .map(|i| if i % 3 == 0 { u64::MAX } else { i })
                .collect::<alloc::vec::Vec<_>>(),
        )
    }

    #[test]
    fn wide_mixed_swar() {
        // Mix in wide values (max width > 5) -> Swar. The classifier only samples the first 64-byte
        // window, so a representative mix must appear up front for this to engage Swar.
        let vals: alloc::vec::Vec<u64> = (0..3000u64)
            .map(|i| if i % 7 == 0 { u64::MAX - i } else { i % 50 })
            .collect();
        swar_span(&vals);
        check(&vals);
    }

    #[test]
    fn wide_mixed_swar_every_width() {
        // Widths 1..=10 in turn: the Swar kernel's word read decodes every 2..=8-byte width, and the
        // 9/10-byte ones fall back to the validating reader.
        let vals: alloc::vec::Vec<u64> = (0..3000u64)
            .map(|i| match i % 10 {
                9 => u64::MAX - i,
                w => (1u64 << (7 * w)) + i % 7, // exactly w + 1 bytes
            })
            .collect();
        let span = swar_span(&vals);
        assert_eq!(decode_u64(&span), vals);
    }

    // The error `fill_packed_varints` reports for `span`, as (kind, offset).
    fn decode_err(span: &[u8]) -> (ErrorKind, usize) {
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        let err = v.fill_packed_varints(span, 0, |raw| raw).unwrap_err();
        (err.kind(), err.offset())
    }

    #[test]
    fn swar_overflow_reports_offset() {
        // A 10-byte varint whose last byte is > 1 overflows. At least 8 bytes remain at it, so the
        // Swar loop's fallback reads it and reports it at its own offset.
        let mut span = swar_prefix();
        let overflow_at = span.len();
        span.resize(span.len() + 9, 0x80);
        span.push(0x02);
        assert_eq!(decode_err(&span), (ErrorKind::VarintOverflow, overflow_at));
    }

    #[test]
    fn swar_truncation_reports_offset() {
        // A varint cut off at the end of the span: with 9 bytes left the Swar loop's fallback reads
        // it, with 7 the scalar tail does. Both report it at its start.
        for left in [9usize, 7] {
            let mut span = swar_prefix();
            let truncated_at = span.len();
            span.resize(span.len() + left, 0x80);
            assert_eq!(decode_err(&span), (ErrorKind::Truncated, truncated_at));
        }
    }

    #[test]
    fn swar_non_canonical_padding() {
        // Zero-padded encodings (1 as 0x81 0x80 0x80 0x00, 300 as 0xac 0x82 0x00) decode like the byte
        // loop reads them.
        let mut span = swar_prefix();
        for _ in 0..50 {
            span.extend_from_slice(&[0x81, 0x80, 0x80, 0x00, 0xac, 0x82, 0x00, 0x07]);
        }
        assert_eq!(decode_u64(&span), reference(&span));
    }

    #[test]
    fn re_classify_across_regime_change() {
        // A long homogeneous run (forces a kernel) followed by a different regime, long enough to
        // re-classify (> 4096 bytes of traction, then > 512 remaining).
        let mut vals: alloc::vec::Vec<u64> = (0..6000u64).map(|_| 1).collect(); // 1-byte run
        vals.extend((0..3000u64).map(|i| 128 + (i % 16000))); // 2-byte run
        vals.extend((0..3000u64).map(|i| if i % 7 == 0 { u64::MAX } else { i % 50 }));
        check(&vals);
    }

    #[test]
    fn conv_is_applied_per_element() {
        let arena = Arena::new();
        let mut v = DecodeVec::<i32>::new(&arena);
        let mut span = alloc::vec::Vec::new();
        for i in 0..1000u64 {
            put_varint(&mut span, i);
        }
        v.fill_packed_varints(&span, 0, |raw| raw as i32).unwrap();
        let got = v.into_slice();
        assert_eq!(got.len(), 1000);
        for (i, &x) in got.iter().enumerate() {
            assert_eq!(x, i as i32);
        }
    }

    #[test]
    fn truncated_varint_errors_without_panic() {
        // A valid large run, then a dangling continuation byte at the end.
        let mut span = alloc::vec::Vec::new();
        for _ in 0..500 {
            put_varint(&mut span, 300); // 2-byte each
        }
        span.push(0x80); // continuation with no terminator
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        let err = v.fill_packed_varints(&span, 10, |raw| raw).unwrap_err();
        assert_eq!(err.kind(), ErrorKind::Truncated);
        // Offset is base_off + position of the dangling byte within the span.
        assert_eq!(err.offset(), 10 + (span.len() - 1));
    }

    #[test]
    fn spans_around_the_kernel_min_span_boundary() {
        // 1-byte varints -> one byte each, so element count == span length in bytes. Exercise 255
        // (below the 256 floor: scalar loop), 256 (floor: kernels engage), and 257.
        for n in [255usize, 256, 257] {
            let vals: alloc::vec::Vec<u64> = (0..n as u64).map(|i| i % 128).collect();
            check(&vals);
        }
    }

    #[test]
    fn ten_byte_overflow_bails_to_tail_and_reports_overflow() {
        // A homogeneous 10-byte run (value with bit 63 set encodes in exactly 10 bytes) forces the
        // Fixed(10) kernel; then an over-long 10-byte varint whose 10th byte is > 1 encodes bits above
        // 63. fixed_fill_wide<10> must bail that element to the validating tail, which reports
        // VarintOverflow -- never a wrong value, never a panic.
        let mut span = alloc::vec::Vec::new();
        for _ in 0..40 {
            put_varint(&mut span, 1u64 << 63); // 10 bytes each -> >= 256 bytes, classifies Fixed(10)
        }
        let overflow_at = span.len();
        span.resize(span.len() + 9, 0x80); // nine continuation bytes
        span.push(0x02); // 10th byte > 1 -> overflow
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        let err = v.fill_packed_varints(&span, 0, |raw| raw).unwrap_err();
        assert_eq!(err.kind(), ErrorKind::VarintOverflow);
        assert_eq!(err.offset(), overflow_at);
    }

    #[test]
    fn two_segments_reserve_into_nonempty() {
        // A packed field can arrive in two wire segments: the second fill appends into a DecodeVec
        // that already holds elements (reserve-into-nonempty, then relocate-on-grow).
        let arena = Arena::new();
        let mut v = DecodeVec::<u64>::new(&arena);
        let seg1: alloc::vec::Vec<u64> = (0..300u64).map(|i| 128 + (i % 16000)).collect(); // 2-byte
        let seg2: alloc::vec::Vec<u64> = (0..400u64).map(|i| i % 64).collect(); // 1-byte
        let mut b1 = alloc::vec::Vec::new();
        for &x in &seg1 {
            put_varint(&mut b1, x);
        }
        let mut b2 = alloc::vec::Vec::new();
        for &x in &seg2 {
            put_varint(&mut b2, x);
        }
        v.fill_packed_varints(&b1, 0, |raw| raw).unwrap();
        v.fill_packed_varints(&b2, 0, |raw| raw).unwrap();
        let got = v.into_slice();
        let mut expected = seg1.clone();
        expected.extend(seg2);
        assert_eq!(got, &expected[..]);
    }
}
