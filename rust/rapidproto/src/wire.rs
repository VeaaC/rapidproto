//! The wire-decode primitives generated decoders build on: the value-threaded free functions
//! ([`read_tag_or_end`], [`read_varint`], [`read_fixed32`]/[`read_fixed64`], [`read_length_delimited`],
//! [`skip`]) and an ergonomic [`Cursor`] wrapper over them.
//!
//! # Two surfaces
//!
//! The free functions are the primary, hot-path API: each takes the not-yet-consumed `&[u8]` BY VALUE
//! and returns the slice past what it read (writing any decoded value to an out-param), reporting
//! failure as a bare [`ErrorKind`]. **Generated `decode` bodies call these directly** -- threading the
//! slice keeps it in registers across the field loop, the way the C++ reader threads a bare pointer;
//! a `&mut Cursor` across a non-inlined call would spill it to the stack every field. [`Cursor`] is a
//! thin `&mut self` wrapper for ergonomic/ad-hoc use (tests, tools) that adds the byte offset; it is
//! not the surface generated code should use on the hot path.
//!
//! # Error byte offsets are ABSOLUTE to the original input
//!
//! The contract (matching the C++ reader, which threads the buffer start through every reader) is that
//! a [`DecodeError`]'s offset is absolute to the whole top-level input, INCLUDING failures inside a
//! nested length-delimited sub-message. The free functions deliberately carry no offset (they return
//! only [`ErrorKind`]), leaving the caller to assign it: at the top level [`Cursor`] computes
//! `input_len - rest.len()` on the error path; a generated decoder recursing into a sub-message adds
//! the sub-message's absolute start offset to `sub_input.len() - rest.len()`. Keeping the offset off
//! the hot path (computed only on failure) is why the readers thread just the slice.
//!
//! One deliberate divergence from the C++ reader: a failure *inside* a group being [`skip`]ped is
//! reported at the group field's start, not at the deep byte where it was detected (the value-threaded
//! `skip` returns a bare kind and the caller still holds the field-start slice). The C++ reader threads
//! the buffer start and reports the deep position. This is cold (unknown/dropped fields) and the offset
//! is diagnostic only; it is the one spot the "derive the offset from the unadvanced caller slice"
//! trick cannot reach the true position.
//!
//! # Safety
//!
//! Every reader returns `Result`, never panics, and never reads out of bounds on arbitrary input (the
//! decoder runs on untrusted bytes) -- entirely in safe Rust, no `unsafe`. Values are returned raw;
//! interpreting a varint as zigzag/bool, or a fixed field as a float, is the caller's step (see
//! [`zigzag32`]/[`zigzag64`]).

use crate::error::{DecodeError, ErrorKind};

/// A Protobuf wire type -- the low 3 bits of a field tag. `#[repr(u8)]` with each variant pinned to
/// its wire code, so `wire as u8` is the on-wire value and generated decoders can use it as a raw tag
/// component (matching the C++ reader's raw-tag dispatch).
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum WireType {
    /// Wire type 0: base-128 varint (int32/64, uint32/64, sint*, bool, enum).
    Varint = 0,
    /// Wire type 1: fixed 64-bit (fixed64, sfixed64, double).
    I64 = 1,
    /// Wire type 2: length-delimited (string, bytes, embedded message, packed repeated).
    Len = 2,
    /// Wire type 3: group start (proto2 groups; deprecated).
    SGroup = 3,
    /// Wire type 4: group end.
    EGroup = 4,
    /// Wire type 5: fixed 32-bit (fixed32, sfixed32, float).
    I32 = 5,
}

impl WireType {
    /// Map the low 3 bits of a tag to a wire type, or `None` for the two unused codes (6, 7).
    #[inline]
    fn from_bits(bits: u32) -> Option<WireType> {
        match bits {
            0 => Some(WireType::Varint),
            1 => Some(WireType::I64),
            2 => Some(WireType::Len),
            3 => Some(WireType::SGroup),
            4 => Some(WireType::EGroup),
            5 => Some(WireType::I32),
            _ => None,
        }
    }
}

/// A decoded field tag: the 1-based field number and its wire type.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Tag {
    /// The field number (always >= 1; a 0 field number is rejected as an invalid tag).
    pub field: u32,
    /// The field's wire type.
    pub wire: WireType,
}

/// The largest legal Protobuf field number (`2^29 - 1`); a tag above it is malformed, matching the
/// C++ reader. (The 19000..=19999 reserved range is a schema rule, not a wire-decode one.)
const MAX_FIELD_NUMBER: u32 = (1 << 29) - 1;

/// The maximum nesting a [`Cursor::skip`] will descend through groups before giving up, so a
/// pathological run of group-start tags cannot drive unbounded recursion.
const MAX_SKIP_DEPTH: u32 = 100;

/// A read cursor over the input: the not-yet-consumed bytes, plus the original input length so an
/// error's byte offset can be derived without a per-read counter. Cheap to copy; advancing reslices
/// `rest` (and nothing else), so the hot loop threads only the slice.
#[derive(Clone, Copy)]
pub struct Cursor<'a> {
    rest: &'a [u8],
    input_len: usize,
}

impl<'a> Cursor<'a> {
    /// A cursor over the whole of `input`.
    #[inline]
    #[must_use]
    pub fn new(input: &'a [u8]) -> Self {
        Self {
            rest: input,
            input_len: input.len(),
        }
    }

    /// The byte offset of the cursor into the original input (what a [`DecodeError`] reports).
    /// Derived from how much the slice has shrunk -- not tracked per read.
    #[inline]
    #[must_use]
    pub fn offset(&self) -> usize {
        self.input_len - self.rest.len()
    }

    /// Whether every byte has been consumed (the end of the top-level message).
    #[inline]
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.rest.is_empty()
    }

    /// The bytes not yet consumed (e.g. an embedded message's remaining body).
    #[inline]
    #[must_use]
    pub fn remaining(&self) -> &'a [u8] {
        self.rest
    }

    /// An error of `kind` at the current offset.
    #[inline]
    fn err(&self, kind: ErrorKind) -> DecodeError {
        DecodeError::new(kind, self.offset())
    }

    /// Read a base-128 varint (up to 10 bytes). `Truncated` if the continuation bit is still set at
    /// end of input; `InvalidTag` if the 10th byte sets bits above bit 0 (an over-long encoding).
    /// A thin wrapper over the value-threaded [`read_varint`] free function (which is what the hot
    /// field loop calls directly); the error offset is this cursor's position, the varint start.
    #[inline]
    pub fn read_varint(&mut self) -> Result<u64, DecodeError> {
        let mut value = 0;
        self.rest = read_varint(self.rest, &mut value).map_err(|k| self.err(k))?;
        Ok(value)
    }

    /// Read a field tag. `InvalidTag` for a field number of 0 or above the Protobuf maximum
    /// (2^29 - 1), or a wire type of 6/7. A thin wrapper over the value-threaded [`read_tag`] free
    /// function; the error offset is this cursor's position, the tag start.
    #[inline]
    pub fn read_tag(&mut self) -> Result<Tag, DecodeError> {
        let mut tag = Tag {
            field: 0,
            wire: WireType::Varint,
        };
        self.rest = read_tag(self.rest, &mut tag).map_err(|k| self.err(k))?;
        Ok(tag)
    }

    /// Read the next field tag, or `None` at end of input. Wrapper over the fused value-threaded
    /// [`read_tag_or_end`] free function (one bounds check distinguishes end from a tag).
    #[inline]
    pub fn read_tag_or_end(&mut self) -> Result<Option<Tag>, DecodeError> {
        let mut tag = Tag {
            field: 0,
            wire: WireType::Varint,
        };
        match read_tag_or_end(self.rest, &mut tag).map_err(|k| self.err(k))? {
            Some(rest) => {
                self.rest = rest;
                Ok(Some(tag))
            }
            None => Ok(None),
        }
    }

    /// Read a fixed 32-bit little-endian value (fixed32/sfixed32/float bits). Wrapper over the
    /// value-threaded [`read_fixed32`] free function.
    #[inline]
    pub fn read_fixed32(&mut self) -> Result<u32, DecodeError> {
        let mut value = 0;
        self.rest = read_fixed32(self.rest, &mut value).map_err(|k| self.err(k))?;
        Ok(value)
    }

    /// Read a fixed 64-bit little-endian value (fixed64/sfixed64/double bits). Wrapper over the
    /// value-threaded [`read_fixed64`] free function.
    #[inline]
    pub fn read_fixed64(&mut self) -> Result<u64, DecodeError> {
        let mut value = 0;
        self.rest = read_fixed64(self.rest, &mut value).map_err(|k| self.err(k))?;
        Ok(value)
    }

    /// Read a length-delimited payload (a varint length then that many bytes), returning the payload
    /// without copying. `Truncated` if the declared length runs past the input (reported at the length
    /// start). Wrapper over the value-threaded [`read_length_delimited`] free function.
    #[inline]
    pub fn read_length_delimited(&mut self) -> Result<&'a [u8], DecodeError> {
        let mut payload: &[u8] = &[];
        self.rest = read_length_delimited(self.rest, &mut payload).map_err(|k| self.err(k))?;
        Ok(payload)
    }

    /// Consume one field's value for the field whose `tag` was just read, for an unknown or dropped
    /// field. A group is skipped to its matching end tag -- matched by field number -- with nested
    /// groups recursing, capped by [`MAX_SKIP_DEPTH`]. Wrapper over the value-threaded [`skip`] free
    /// function; a failure is reported at this field's start.
    #[inline]
    pub fn skip(&mut self, tag: Tag) -> Result<(), DecodeError> {
        self.rest = skip(self.rest, tag).map_err(|k| self.err(k))?;
        Ok(())
    }
}

// ── Value-threaded wire readers ────────────────────────────────────────────────────────────────
// The hot decode primitives. Each takes the not-yet-consumed bytes BY VALUE and returns the slice past
// what it read (writing any decoded value to an out-param), or the failing `ErrorKind`. Threading the
// slice rather than a `&mut Cursor` is what keeps it in registers across a field loop -- a mutable
// reference would make the cursor's address escape and force ptr + len to the stack every field (the
// C++ reader threads a bare pointer for the same reason). Generated `decode` bodies call these
// directly; [`Cursor`] is a thin wrapper that adds the error offset. `#[inline]`, so the `Result` is
// folded into the caller's branches rather than returned through memory. All safe Rust: no `unsafe`,
// no panic, no out-of-bounds read on arbitrary input. The caller derives the error's byte offset from
// the slice it still holds (unchanged on the error path).

/// Read a base-128 varint (up to 10 bytes). `take(10)` caps the trip count so the loop unrolls like
/// the C++ reader's fixed 10-iteration one; the over-long check sits in the terminator branch (hit
/// once for the only well-formed case). `Truncated` at end of input; `VarintOverflow` for an over-long
/// encoding (10th byte above bit 0, or ten continuation bytes) -- a context-neutral kind, since this
/// reads values and length prefixes, not only tags.
#[inline]
pub fn read_varint<'a>(rest: &'a [u8], out: &mut u64) -> Result<&'a [u8], ErrorKind> {
    // No separate scalar 1-byte fast path (unlike the C++ reader): the loop's first iteration peels to
    // exactly that (asm-confirmed -- load, mask, continuation-bit test, return with one byte consumed),
    // so the common single-byte value compiles to the same branchless load C++ hand-writes.
    let mut result: u64 = 0;
    let mut shift: u32 = 0;
    for (i, &byte) in rest.iter().take(10).enumerate() {
        result |= u64::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            if i == 9 && byte > 0x01 {
                return Err(ErrorKind::VarintOverflow);
            }
            *out = result;
            return Ok(&rest[i + 1..]);
        }
        shift += 7;
    }
    // Fell through without a terminator: ten continuation bytes (an over-long encoding that cannot fit
    // 64 bits) vs fewer bytes than the varint needs (the input ran out mid-varint).
    if rest.len() >= 10 {
        Err(ErrorKind::VarintOverflow)
    } else {
        Err(ErrorKind::Truncated)
    }
}

/// Read the next field tag, or report clean end of input as `Ok(None)` -- THE hot-loop driver a
/// generated decode body calls each field. A single `split_first` distinguishes end from a tag (no
/// separate `is_empty` the loop condition would duplicate), then the fused 1-byte / 2-byte decode runs
/// inline. Mirrors the C++ `wire::read_tag_or_end`, which fuses the bounds check and both fast paths
/// for exactly this reason.
#[inline]
pub fn read_tag_or_end<'a>(rest: &'a [u8], out: &mut Tag) -> Result<Option<&'a [u8]>, ErrorKind> {
    match rest.split_first() {
        None => Ok(None),
        Some((&first, tail)) => read_tag_from(first, tail, rest, out).map(Some),
    }
}

/// Read a field tag, erroring (`Truncated`) at end of input. Same fused decode as
/// [`read_tag_or_end`], for callers (group skip, the ergonomic [`Cursor`]) that require a tag.
/// `InvalidTag` for field 0, a field above `2^29 - 1`, or a wire type of 6/7.
#[inline]
pub fn read_tag<'a>(rest: &'a [u8], out: &mut Tag) -> Result<&'a [u8], ErrorKind> {
    match rest.split_first() {
        None => Err(ErrorKind::Truncated),
        Some((&first, tail)) => read_tag_from(first, tail, rest, out),
    }
}

/// The shared fused tag decode: `first`/`tail` are `rest.split_first()` (so `rest` is non-empty).
/// Decodes a 1-byte tag (fields 1..=15) or a 2-byte tag (fields 16..=2047) inline -- the common cases
/// -- and falls through to the general multibyte varint otherwise. Mirrors the C++ fused paths; the
/// 2-byte fusion is measured -31% on a >15-field decode without it.
#[inline]
fn read_tag_from<'a>(
    first: u8,
    tail: &'a [u8],
    rest: &'a [u8],
    out: &mut Tag,
) -> Result<&'a [u8], ErrorKind> {
    if first & 0x80 == 0 {
        // Fused 1-byte tag: field 1..=15, wire in the low 3 bits.
        let field = u32::from(first >> 3);
        if field != 0 {
            if let Some(wire) = WireType::from_bits(u32::from(first & 0x07)) {
                *out = Tag { field, wire };
                return Ok(tail);
            }
        }
        // field 0 or a reserved wire type (6/7): fall through for a uniform error.
    } else if let Some((&second, rest2)) = tail.split_first() {
        // Fused 2-byte tag: `first` has the continuation bit; `second - 1 < 0x7f` means the second
        // byte terminates (no continuation) and is nonzero, so the value is 128..=16383 -> field
        // 16..=2047, never 0 and within range. A reserved wire type (6/7) still falls through. Matches
        // the C++ fused 2-byte path's `(u8)(p[1]-1) < 0x7F` guard.
        if second.wrapping_sub(1) < 0x7f {
            let value = (u32::from(first) & 0x7f) | (u32::from(second) << 7);
            if let Some(wire) = WireType::from_bits(value & 0x07) {
                *out = Tag {
                    field: value >> 3,
                    wire,
                };
                return Ok(rest2);
            }
        }
        // non-canonical 2-byte, 3+ byte, or reserved wire type: fall through.
    }
    read_tag_general(rest, out)
}

/// The tag general case: a 3+ byte tag, or a 1-/2-byte tag the fast paths rejected. Reads the full
/// varint and range-checks it. `#[inline]` so the whole reader (incl. this cold path) folds into the
/// caller -- that lets the `Tag` out-param scalarize to registers instead of escaping to the stack
/// (the C++ reader gets the same via `flatten`); the cold multibyte code just sits after the return.
#[inline]
fn read_tag_general<'a>(rest: &'a [u8], out: &mut Tag) -> Result<&'a [u8], ErrorKind> {
    let mut raw = 0u64;
    let rest = read_varint(rest, &mut raw)?;
    let field = raw >> 3;
    if field == 0 || field > u64::from(MAX_FIELD_NUMBER) {
        return Err(ErrorKind::InvalidTag);
    }
    let wire = WireType::from_bits((raw & 0x07) as u32).ok_or(ErrorKind::InvalidTag)?;
    *out = Tag {
        field: field as u32,
        wire,
    };
    Ok(rest)
}

/// Read a fixed 32-bit little-endian value (fixed32/sfixed32/float bits). `Truncated` if fewer than 4
/// bytes remain.
#[inline]
pub fn read_fixed32<'a>(rest: &'a [u8], out: &mut u32) -> Result<&'a [u8], ErrorKind> {
    match rest.first_chunk::<4>() {
        Some(bytes) => {
            *out = u32::from_le_bytes(*bytes);
            Ok(&rest[4..])
        }
        None => Err(ErrorKind::Truncated),
    }
}

/// Read a fixed 64-bit little-endian value (fixed64/sfixed64/double bits). `Truncated` if fewer than 8
/// bytes remain.
#[inline]
pub fn read_fixed64<'a>(rest: &'a [u8], out: &mut u64) -> Result<&'a [u8], ErrorKind> {
    match rest.first_chunk::<8>() {
        Some(bytes) => {
            *out = u64::from_le_bytes(*bytes);
            Ok(&rest[8..])
        }
        None => Err(ErrorKind::Truncated),
    }
}

/// Read a length-delimited payload (a varint length then that many bytes), writing the borrowed
/// payload to `out`. `Truncated` if the declared length does not fit the remaining input.
#[inline]
pub fn read_length_delimited<'a>(
    rest: &'a [u8],
    out: &mut &'a [u8],
) -> Result<&'a [u8], ErrorKind> {
    let mut len = 0u64;
    let rest = read_varint(rest, &mut len)?;
    // A length that does not fit a usize cannot fit the remaining input either.
    let len = usize::try_from(len).map_err(|_| ErrorKind::Truncated)?;
    if len > rest.len() {
        return Err(ErrorKind::Truncated);
    }
    let (payload, tail) = rest.split_at(len);
    *out = payload;
    Ok(tail)
}

/// Consume one field's value for the field whose `tag` was just read (an unknown or dropped field),
/// returning the slice past it. A group (wire type [`SGroup`](WireType::SGroup)) skips to the matching
/// end tag -- the one whose field number equals the group's, as the C++ reader requires -- with nested
/// groups recursing, capped by [`MAX_SKIP_DEPTH`]. Cold relative to the typed readers. `tag.field` is
/// used only to match a group's end; for the scalar wire types it is ignored.
pub fn skip(rest: &[u8], tag: Tag) -> Result<&[u8], ErrorKind> {
    skip_depth(rest, tag, 0)
}

fn skip_depth(rest: &[u8], tag: Tag, depth: u32) -> Result<&[u8], ErrorKind> {
    match tag.wire {
        WireType::Varint => {
            let mut v = 0;
            read_varint(rest, &mut v)
        }
        WireType::I64 => {
            let mut v = 0;
            read_fixed64(rest, &mut v)
        }
        WireType::I32 => {
            let mut v = 0;
            read_fixed32(rest, &mut v)
        }
        WireType::Len => {
            let mut p: &[u8] = &[];
            read_length_delimited(rest, &mut p)
        }
        // A group-end with no matching start: malformed.
        WireType::EGroup => Err(ErrorKind::InvalidWireType),
        WireType::SGroup => {
            if depth >= MAX_SKIP_DEPTH {
                return Err(ErrorKind::DepthLimitExceeded);
            }
            let mut rest = rest;
            loop {
                let mut inner = Tag {
                    field: 0,
                    wire: WireType::Varint,
                };
                rest = read_tag(rest, &mut inner)?;
                if inner.wire == WireType::EGroup {
                    // The end tag must close THIS group: its field number must match. A mismatched end
                    // group is a malformed nesting (the C++ reader rejects it too).
                    if inner.field != tag.field {
                        return Err(ErrorKind::InvalidWireType);
                    }
                    break;
                }
                rest = skip_depth(rest, inner, depth + 1)?;
            }
            Ok(rest)
        }
    }
}

/// Decode a zigzag-encoded 32-bit signed integer (sint32) from its varint value.
#[inline]
#[must_use]
pub fn zigzag32(value: u32) -> i32 {
    ((value >> 1) as i32) ^ -((value & 1) as i32)
}

/// Decode a zigzag-encoded 64-bit signed integer (sint64) from its varint value.
#[inline]
#[must_use]
pub fn zigzag64(value: u64) -> i64 {
    ((value >> 1) as i64) ^ -((value & 1) as i64)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kind(input: &[u8]) -> ErrorKind {
        Cursor::new(input).read_varint().unwrap_err().kind()
    }

    // A tag for `skip`; the field number matters only for matching a group's end tag.
    fn tag(field: u32, wire: WireType) -> Tag {
        Tag { field, wire }
    }

    #[test]
    fn varint_values_and_advance() {
        let mut c = Cursor::new(&[0x00]);
        assert_eq!(c.read_varint().unwrap(), 0);
        assert!(c.is_empty());

        // 300 = 0xAC 0x02; a trailing byte stays unconsumed.
        let mut c = Cursor::new(&[0xac, 0x02, 0x7f]);
        assert_eq!(c.read_varint().unwrap(), 300);
        assert_eq!(c.offset(), 2);
        assert_eq!(c.remaining(), &[0x7f]);

        // u64::MAX is ten bytes, last byte 0x01.
        let mut c = Cursor::new(&[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]);
        assert_eq!(c.read_varint().unwrap(), u64::MAX);
        assert!(c.is_empty());
    }

    #[test]
    fn varint_rejects_truncated_and_overlong() {
        assert_eq!(kind(&[]), ErrorKind::Truncated);
        assert_eq!(kind(&[0x80]), ErrorKind::Truncated); // continuation bit, no more bytes
        assert_eq!(kind(&[0x80; 5]), ErrorKind::Truncated);
        // Ten continuation bytes: an over-long encoding that cannot fit 64 bits -> VarintOverflow,
        // a context-neutral kind (not InvalidTag -- this reader also decodes values and lengths).
        assert_eq!(kind(&[0x80; 10]), ErrorKind::VarintOverflow);
        // Tenth byte sets bits above bit 0 -> over-long.
        assert_eq!(
            kind(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02]),
            ErrorKind::VarintOverflow
        );
    }

    #[test]
    fn tag_decode_and_rejections() {
        // field 1, wire Len: (1 << 3) | 2 = 0x0a.
        let mut c = Cursor::new(&[0x0a]);
        assert_eq!(
            c.read_tag().unwrap(),
            Tag {
                field: 1,
                wire: WireType::Len
            }
        );

        // field 0 is invalid.
        assert_eq!(
            Cursor::new(&[0x00]).read_tag().unwrap_err().kind(),
            ErrorKind::InvalidTag
        );
        // wire type 7 (the high 3 bits set) is invalid: (1 << 3) | 7 = 0x0f.
        assert_eq!(
            Cursor::new(&[0x0f]).read_tag().unwrap_err().kind(),
            ErrorKind::InvalidTag
        );

        // The max field number (2^29 - 1) with wire Len is accepted; one past it is rejected.
        let encode = |value: u64| {
            let mut buf = Vec::new();
            let mut v = value;
            loop {
                let byte = (v & 0x7f) as u8;
                v >>= 7;
                if v == 0 {
                    buf.push(byte);
                    break;
                }
                buf.push(byte | 0x80);
            }
            buf
        };
        let max_tag = (u64::from(MAX_FIELD_NUMBER) << 3) | 2;
        let over_tag = ((u64::from(MAX_FIELD_NUMBER) + 1) << 3) | 2;
        assert_eq!(
            Cursor::new(&encode(max_tag)).read_tag().unwrap().field,
            MAX_FIELD_NUMBER
        );
        assert_eq!(
            Cursor::new(&encode(over_tag))
                .read_tag()
                .unwrap_err()
                .kind(),
            ErrorKind::InvalidTag
        );

        assert_eq!(Cursor::new(&[]).read_tag_or_end().unwrap(), None);
        assert!(Cursor::new(&[0x0a]).read_tag_or_end().unwrap().is_some());

        // Fused 2-byte tag path: field 18 varint = (18 << 3) | 0 = 144 -> varint 0x90 0x01. The fused
        // path and the general path must agree, and the trailing byte stays unconsumed.
        let mut c = Cursor::new(&[0x90, 0x01, 0x7f]);
        assert_eq!(
            c.read_tag().unwrap(),
            Tag {
                field: 18,
                wire: WireType::Varint
            }
        );
        assert_eq!(c.remaining(), &[0x7f]);
        // A 2-byte tag with a reserved wire type (6) falls through and is rejected: field 18 wire 6 =
        // 150 -> 0x96 0x01.
        assert_eq!(
            Cursor::new(&[0x96, 0x01]).read_tag().unwrap_err().kind(),
            ErrorKind::InvalidTag
        );
        // A tag truncated mid-varint (continuation bit set, no next byte) is Truncated, not a panic.
        assert_eq!(
            Cursor::new(&[0x88]).read_tag().unwrap_err().kind(),
            ErrorKind::Truncated
        );
    }

    #[test]
    fn fixed_and_length_delimited() {
        let mut c = Cursor::new(&[0x78, 0x56, 0x34, 0x12, 0xff]);
        assert_eq!(c.read_fixed32().unwrap(), 0x1234_5678);
        assert_eq!(c.remaining(), &[0xff]);
        assert_eq!(
            Cursor::new(&[0, 0, 0]).read_fixed32().unwrap_err().kind(),
            ErrorKind::Truncated
        );

        let mut c = Cursor::new(&[1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(c.read_fixed64().unwrap(), 0x0807_0605_0403_0201);

        // A fixed read past the end reports Truncated at ITS start offset (not offset 0): consume a
        // 1-byte tag first, then only 3 bytes remain for a fixed32.
        let mut c = Cursor::new(&[0x08, 1, 2, 3]);
        c.read_tag().unwrap();
        let err = c.read_fixed32().unwrap_err();
        assert_eq!(err.kind(), ErrorKind::Truncated);
        assert_eq!(err.offset(), 1);
        // Same for fixed64: a 1-byte tag then only 7 bytes -> Truncated at offset 1.
        let mut c = Cursor::new(&[0x08, 1, 2, 3, 4, 5, 6, 7]);
        c.read_tag().unwrap();
        let err = c.read_fixed64().unwrap_err();
        assert_eq!(err.kind(), ErrorKind::Truncated);
        assert_eq!(err.offset(), 1);

        // length 3 then "abc", with a trailing byte.
        let mut c = Cursor::new(&[0x03, b'a', b'b', b'c', b'z']);
        assert_eq!(c.read_length_delimited().unwrap(), b"abc");
        assert_eq!(c.remaining(), b"z");
        // A length past the end is truncated, reported at the length's offset (0).
        let err = Cursor::new(&[0x05, b'a'])
            .read_length_delimited()
            .unwrap_err();
        assert_eq!(err.kind(), ErrorKind::Truncated);
        assert_eq!(err.offset(), 0);
    }

    #[test]
    fn skip_every_wire_type_including_nested_groups() {
        // varint, i64, i32, len
        assert!(Cursor::new(&[0xac, 0x02])
            .skip(tag(1, WireType::Varint))
            .is_ok());
        assert!(Cursor::new(&[0; 8]).skip(tag(1, WireType::I64)).is_ok());
        assert!(Cursor::new(&[0; 4]).skip(tag(1, WireType::I32)).is_ok());
        assert!(Cursor::new(&[0x02, b'h', b'i'])
            .skip(tag(1, WireType::Len))
            .is_ok());

        // A group: start-tag already consumed by the caller, so skip() is given the SGroup tag and
        // consumes through the matching end. Body: field 1 varint (0x08 0x01), inner group (0x0b ...
        // 0x0c), then this group's end (0x0c). Tags: SGroup=(f<<3)|3, EGroup=(f<<3)|4 -- all field 1.
        let group = [
            0x08, 0x01, /*inner start f1*/ 0x0b, /*inner end f1*/ 0x0c,
            /*end f1*/ 0x0c,
        ];
        let mut c = Cursor::new(&group);
        assert!(c.skip(tag(1, WireType::SGroup)).is_ok());
        assert!(c.is_empty());

        // A stray group-end is malformed.
        assert_eq!(
            Cursor::new(&[])
                .skip(tag(1, WireType::EGroup))
                .unwrap_err()
                .kind(),
            ErrorKind::InvalidWireType
        );

        // A group closed by an end tag with a DIFFERENT field number is malformed (matches the C++
        // reader, which pairs end groups by field number). Start field 1 (skip arg), end field 2
        // (0x14 = (2<<3)|4) -> rejected.
        assert_eq!(
            Cursor::new(&[0x14])
                .skip(tag(1, WireType::SGroup))
                .unwrap_err()
                .kind(),
            ErrorKind::InvalidWireType
        );
        // The same body closed by the matching field-1 end (0x0c) is accepted.
        assert!(Cursor::new(&[0x0c]).skip(tag(1, WireType::SGroup)).is_ok());
    }

    #[test]
    fn skip_group_depth_is_capped() {
        // A run of group-start tags with no ends must hit the depth cap, not recurse unboundedly.
        let starts = vec![0x0b_u8; (MAX_SKIP_DEPTH as usize) + 5];
        assert_eq!(
            Cursor::new(&starts)
                .skip(tag(1, WireType::SGroup))
                .unwrap_err()
                .kind(),
            ErrorKind::DepthLimitExceeded
        );

        // Exact boundary: the outer skip is depth 0, so reading N field-1 start tags (0x0b) descends
        // to depth N. The cap rejects at depth == MAX_SKIP_DEPTH. With MAX_SKIP_DEPTH starts the
        // deepest recursion hits the cap (DepthLimitExceeded); with one fewer it never does and instead
        // runs out of input (Truncated) -- pinning the off-by-one (depth 99 allowed, 100 rejected).
        let at_cap = vec![0x0b_u8; MAX_SKIP_DEPTH as usize];
        let under_cap = vec![0x0b_u8; (MAX_SKIP_DEPTH as usize) - 1];
        assert_eq!(
            Cursor::new(&at_cap)
                .skip(tag(1, WireType::SGroup))
                .unwrap_err()
                .kind(),
            ErrorKind::DepthLimitExceeded
        );
        assert_eq!(
            Cursor::new(&under_cap)
                .skip(tag(1, WireType::SGroup))
                .unwrap_err()
                .kind(),
            ErrorKind::Truncated
        );
    }

    #[test]
    fn zigzag_round_trips_known_values() {
        assert_eq!(zigzag32(0), 0);
        assert_eq!(zigzag32(1), -1);
        assert_eq!(zigzag32(2), 1);
        assert_eq!(zigzag32(4_294_967_294), i32::MAX);
        assert_eq!(zigzag32(4_294_967_295), i32::MIN);
        assert_eq!(zigzag64(0), 0);
        assert_eq!(zigzag64(1), -1);
        assert_eq!(zigzag64(u64::MAX), i64::MIN);
    }

    #[test]
    fn never_panics_on_arbitrary_or_truncated_input() {
        // Every prefix of a plausible message, and some adversarial byte runs, must yield Ok or Err
        // -- never a panic (the decoder runs on untrusted bytes).
        let samples: &[&[u8]] = &[
            &[
                0x08, 0xac, 0x02, 0x12, 0x03, b'a', b'b', b'c', 0x1d, 1, 2, 3, 4,
            ],
            &[0xff; 32],
            &[0x80; 32],
            &[0x0b, 0x0b, 0x0b, 0x12, 0xff],
        ];
        for sample in samples {
            for end in 0..=sample.len() {
                let mut c = Cursor::new(&sample[..end]);
                // Drive a field loop to exhaustion or first error; just must not panic.
                while let Ok(Some(field_tag)) = c.read_tag_or_end() {
                    if c.skip(field_tag).is_err() {
                        break;
                    }
                }
            }
        }
    }

    #[test]
    fn wire_type_repr_is_the_wire_code() {
        // `#[repr(u8)]` pins each variant to its on-wire code, so generated decoders can use it raw.
        assert_eq!(WireType::Varint as u8, 0);
        assert_eq!(WireType::I64 as u8, 1);
        assert_eq!(WireType::Len as u8, 2);
        assert_eq!(WireType::SGroup as u8, 3);
        assert_eq!(WireType::EGroup as u8, 4);
        assert_eq!(WireType::I32 as u8, 5);
    }

    #[test]
    fn free_fns_thread_the_slice_directly() {
        // The value-threaded readers (what generated code calls) return the advanced slice directly.
        let mut v = 0;
        assert_eq!(read_varint(&[0xac, 0x02, 0x7f], &mut v).unwrap(), &[0x7f]);
        assert_eq!(v, 300);

        let mut x = 0u32;
        assert_eq!(
            read_fixed32(&[0x78, 0x56, 0x34, 0x12, 0xff], &mut x).unwrap(),
            &[0xff]
        );
        assert_eq!(x, 0x1234_5678);

        let mut y = 0u64;
        assert!(read_fixed64(&[1, 2, 3, 4, 5, 6, 7, 8], &mut y).is_ok());
        assert_eq!(y, 0x0807_0605_0403_0201);

        let mut payload: &[u8] = &[];
        assert_eq!(
            read_length_delimited(&[0x03, b'a', b'b', b'c', b'z'], &mut payload).unwrap(),
            b"z"
        );
        assert_eq!(payload, b"abc");

        let mut t = tag(0, WireType::Varint);
        assert_eq!(read_tag(&[0x0a, 0xff], &mut t).unwrap(), &[0xff]);
        assert_eq!(t, tag(1, WireType::Len));

        assert_eq!(
            skip(&[0xac, 0x02, 0x7f], tag(1, WireType::Varint)).unwrap(),
            &[0x7f]
        );

        // read_tag on empty is Truncated (it requires a tag); read_tag_or_end reports clean end.
        assert_eq!(read_tag(&[], &mut t).unwrap_err(), ErrorKind::Truncated);
    }

    #[test]
    fn read_tag_or_end_fused_end_one_two_and_three_byte() {
        let mut t = tag(0, WireType::Varint);
        // Clean end of input.
        assert_eq!(read_tag_or_end(&[], &mut t).unwrap(), None);
        // 1-byte tag: field 1, Len.
        assert_eq!(read_tag_or_end(&[0x0a, 0x00], &mut t).unwrap(), Some(&[0x00][..]));
        assert_eq!(t, tag(1, WireType::Len));
        // 2-byte fused tag: field 18 varint = 0x90 0x01 (consumes exactly 2).
        assert_eq!(read_tag_or_end(&[0x90, 0x01], &mut t).unwrap(), Some(&[][..]));
        assert_eq!(t, tag(18, WireType::Varint));
        // 3-byte tag: field 2048 (just past the fused 2-byte range) via the general path. (2048 << 3)
        // = 16384 -> 0x80 0x80 0x01, with a trailing byte.
        assert_eq!(
            read_tag_or_end(&[0x80, 0x80, 0x01, 0x42], &mut t).unwrap(),
            Some(&[0x42][..])
        );
        assert_eq!(t, tag(2048, WireType::Varint));
        // Malformed tag (reserved wire type 7): (1 << 3) | 7 = 0x0f.
        assert_eq!(
            read_tag_or_end(&[0x0f], &mut t).unwrap_err(),
            ErrorKind::InvalidTag
        );
    }

    #[test]
    fn fused_2byte_and_general_paths_agree_for_a_mid_range_field() {
        // Field 18 varint, canonical 2-byte tag (0x90 0x01) goes through the fused 2-byte path; a
        // NON-canonical 3-byte encoding of the same tag (0x90 0x81 0x00) fails the 2-byte guard
        // (second byte 0x81 has the continuation bit) and goes through the general path. Both must
        // decode to the identical tag -- the fast and general paths cannot disagree for 16..=2047.
        let mut fused = tag(0, WireType::Varint);
        let mut general = tag(0, WireType::Varint);
        assert!(read_tag(&[0x90, 0x01], &mut fused).unwrap().is_empty());
        assert!(read_tag(&[0x90, 0x81, 0x00], &mut general).unwrap().is_empty());
        assert_eq!(fused, general);
        assert_eq!(fused, tag(18, WireType::Varint));
    }

    #[test]
    fn varint_overflow_surfaces_through_read_tag() {
        // An over-long tag varint (eleven continuation bytes) falls through to the general varint and
        // overflows -> VarintOverflow (context-neutral), not InvalidTag.
        let mut t = tag(0, WireType::Varint);
        assert_eq!(
            read_tag(&[0xff; 11], &mut t).unwrap_err(),
            ErrorKind::VarintOverflow
        );
    }

    #[test]
    fn length_delimited_rejects_huge_and_overflowing_length() {
        // A length far past the input is Truncated, reported at the length start (offset 0).
        let err = Cursor::new(&[0xff, 0xff, 0xff, 0xff, 0x0f, b'x'])
            .read_length_delimited()
            .unwrap_err();
        assert_eq!(err.kind(), ErrorKind::Truncated);
        assert_eq!(err.offset(), 0);
        // A length varint that itself overflows 64 bits is VarintOverflow.
        let mut p: &[u8] = &[];
        assert_eq!(
            read_length_delimited(&[0xff; 11], &mut p).unwrap_err(),
            ErrorKind::VarintOverflow
        );
    }

    #[test]
    fn skip_matches_nested_groups_with_differing_fields() {
        // Outer group field 1; body = inner group field 2 (start 0x13, end 0x14), then the outer end
        // (0x0c). Tags: f2 SGroup=(2<<3)|3=0x13, f2 EGroup=(2<<3)|4=0x14, f1 EGroup=0x0c.
        assert!(Cursor::new(&[0x13, 0x14, 0x0c])
            .skip(tag(1, WireType::SGroup))
            .is_ok());
        // The inner (field-2) group closed by a field-1 end tag (0x0c) is a mismatch -> rejected.
        assert_eq!(
            Cursor::new(&[0x13, 0x0c, 0x0c])
                .skip(tag(1, WireType::SGroup))
                .unwrap_err()
                .kind(),
            ErrorKind::InvalidWireType
        );
    }
}
