//! The wire-decode primitives generated decoders build on: a [`Cursor`] over the input bytes with
//! the Protobuf field-encoding readers (varint, fixed32/64, length-delimited, tag, skip).
//!
//! The cursor is a shrinking `&[u8]` slice plus a `consumed` counter -- effectively a cursor, but
//! reading the FRONT of a slice that shrinks is what lets the bounds checks fold away in the hot
//! loop, while `consumed` still gives every error an accurate byte offset. Every reader returns
//! `Result`, never panics, and never reads out of bounds on arbitrary input (the decoder runs on
//! untrusted bytes). Values are returned raw; interpreting a varint as zigzag/bool, or a fixed field
//! as a float, is the caller's step (see [`zigzag32`]/[`zigzag64`]).

use crate::error::{DecodeError, ErrorKind};

/// A Protobuf wire type -- the low 3 bits of a field tag.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum WireType {
    /// Wire type 0: base-128 varint (int32/64, uint32/64, sint*, bool, enum).
    Varint,
    /// Wire type 1: fixed 64-bit (fixed64, sfixed64, double).
    I64,
    /// Wire type 2: length-delimited (string, bytes, embedded message, packed repeated).
    Len,
    /// Wire type 3: group start (proto2 groups; deprecated).
    SGroup,
    /// Wire type 4: group end.
    EGroup,
    /// Wire type 5: fixed 32-bit (fixed32, sfixed32, float).
    I32,
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

/// A read cursor over the input: the not-yet-consumed bytes plus the count already consumed (which
/// is the byte offset carried in any [`DecodeError`]). Cheap to copy; advancing reslices `rest`.
#[derive(Clone, Copy)]
pub struct Cursor<'a> {
    rest: &'a [u8],
    consumed: usize,
}

impl<'a> Cursor<'a> {
    /// A cursor over the whole of `input`.
    #[inline]
    #[must_use]
    pub fn new(input: &'a [u8]) -> Self {
        Self {
            rest: input,
            consumed: 0,
        }
    }

    /// The byte offset of the cursor into the original input (what a [`DecodeError`] reports).
    #[inline]
    #[must_use]
    pub fn offset(&self) -> usize {
        self.consumed
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
        DecodeError::new(kind, self.consumed)
    }

    /// Advance past `n` already-read bytes. `n` must be `<= self.rest.len()` (guaranteed by the
    /// caller having just read those bytes).
    #[inline]
    fn advance(&mut self, n: usize) {
        self.rest = &self.rest[n..];
        self.consumed += n;
    }

    /// Read a base-128 varint (up to 10 bytes). `Truncated` if the continuation bit is still set at
    /// end of input; `InvalidTag` if the 10th byte sets bits above bit 0 (an over-long encoding).
    /// Mirrors the C++ reader: a 1-byte fast path for the common case (every tag and most small
    /// fields), then the general loop. Safe Rust: `split_first` and the slice iterator keep the hot
    /// paths bounds-check-free without `unsafe`.
    #[inline]
    pub fn read_varint(&mut self) -> Result<u64, DecodeError> {
        let bytes = self.rest;
        // 1-byte fast path: one byte, continuation bit clear.
        if let Some((&first, _)) = bytes.split_first() {
            if first & 0x80 == 0 {
                self.advance(1);
                return Ok(u64::from(first));
            }
        }
        // General case: iterate the bytes (the iterator elides per-byte bounds checks; we return by
        // the 10th byte, so `shift` never exceeds 63).
        let mut result: u64 = 0;
        let mut shift: u32 = 0;
        for (i, &byte) in bytes.iter().enumerate() {
            // The 10th byte (i == 9) reaches shift 63: only its low bit fits in u64, so an over-long
            // encoding (any higher bit set) is rejected rather than silently truncated.
            if i == 9 && byte > 0x01 {
                return Err(self.err(ErrorKind::InvalidTag));
            }
            result |= u64::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                self.advance(i + 1);
                return Ok(result);
            }
            shift += 7;
        }
        // The loop only falls through by running out of bytes (i == 9 always returns), so the input
        // ended mid-varint.
        Err(self.err(ErrorKind::Truncated))
    }

    /// Read a field tag. `InvalidTag` for a field number of 0 or above the Protobuf maximum
    /// (2^29 - 1), or a wire type of 6/7.
    #[inline]
    pub fn read_tag(&mut self) -> Result<Tag, DecodeError> {
        let before = *self;
        let raw = self.read_varint()?;
        let field = raw >> 3;
        if field == 0 || field > u64::from(MAX_FIELD_NUMBER) {
            return Err(before.err(ErrorKind::InvalidTag));
        }
        let wire = WireType::from_bits((raw & 0x07) as u32)
            .ok_or_else(|| before.err(ErrorKind::InvalidTag))?;
        Ok(Tag {
            field: field as u32,
            wire,
        })
    }

    /// Read the next field tag, or `None` at end of input -- the field-loop primitive.
    #[inline]
    pub fn read_tag_or_end(&mut self) -> Result<Option<Tag>, DecodeError> {
        if self.rest.is_empty() {
            return Ok(None);
        }
        self.read_tag().map(Some)
    }

    /// Read a fixed 32-bit little-endian value (fixed32/sfixed32/float bits).
    #[inline]
    pub fn read_fixed32(&mut self) -> Result<u32, DecodeError> {
        match self.rest.first_chunk::<4>() {
            Some(bytes) => {
                let value = u32::from_le_bytes(*bytes);
                self.advance(4);
                Ok(value)
            }
            None => Err(self.err(ErrorKind::Truncated)),
        }
    }

    /// Read a fixed 64-bit little-endian value (fixed64/sfixed64/double bits).
    #[inline]
    pub fn read_fixed64(&mut self) -> Result<u64, DecodeError> {
        match self.rest.first_chunk::<8>() {
            Some(bytes) => {
                let value = u64::from_le_bytes(*bytes);
                self.advance(8);
                Ok(value)
            }
            None => Err(self.err(ErrorKind::Truncated)),
        }
    }

    /// Read a length-delimited payload (a varint length then that many bytes), returning the payload
    /// without copying. `Truncated` if the declared length runs past the input.
    #[inline]
    pub fn read_length_delimited(&mut self) -> Result<&'a [u8], DecodeError> {
        let before = *self;
        let len = self.read_varint()?;
        // A length that does not fit a usize cannot fit the remaining input either -- treat as
        // truncated at the length's offset (not the cursor's post-varint offset).
        let len = usize::try_from(len).map_err(|_| before.err(ErrorKind::Truncated))?;
        if len > self.rest.len() {
            return Err(before.err(ErrorKind::Truncated));
        }
        let (payload, rest) = self.rest.split_at(len);
        self.rest = rest;
        self.consumed += len;
        Ok(payload)
    }

    /// Consume one field's value given its wire type, for an unknown or dropped field. Groups are
    /// skipped to their matching end tag (nested groups recurse, capped by [`MAX_SKIP_DEPTH`]).
    pub fn skip(&mut self, wire: WireType) -> Result<(), DecodeError> {
        self.skip_depth(wire, 0)
    }

    fn skip_depth(&mut self, wire: WireType, depth: u32) -> Result<(), DecodeError> {
        match wire {
            WireType::Varint => {
                self.read_varint()?;
            }
            WireType::I64 => {
                self.read_fixed64()?;
            }
            WireType::I32 => {
                self.read_fixed32()?;
            }
            WireType::Len => {
                self.read_length_delimited()?;
            }
            WireType::EGroup => {
                // A group-end with no matching start: malformed.
                return Err(self.err(ErrorKind::InvalidWireType));
            }
            WireType::SGroup => {
                if depth >= MAX_SKIP_DEPTH {
                    return Err(self.err(ErrorKind::DepthLimitExceeded));
                }
                loop {
                    let tag = self.read_tag()?;
                    if tag.wire == WireType::EGroup {
                        break; // the matching (or an inner group's) end
                    }
                    self.skip_depth(tag.wire, depth + 1)?;
                }
            }
        }
        Ok(())
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
        // Ten continuation bytes then overflow: an over-long encoding.
        assert_eq!(kind(&[0x80; 10]), ErrorKind::InvalidTag);
        // Tenth byte sets bits above bit 0 -> over-long.
        assert_eq!(
            kind(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02]),
            ErrorKind::InvalidTag
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
        assert!(Cursor::new(&[0xac, 0x02]).skip(WireType::Varint).is_ok());
        assert!(Cursor::new(&[0; 8]).skip(WireType::I64).is_ok());
        assert!(Cursor::new(&[0; 4]).skip(WireType::I32).is_ok());
        assert!(Cursor::new(&[0x02, b'h', b'i']).skip(WireType::Len).is_ok());

        // A group: start-tag already consumed by the caller, so skip() is given SGroup and consumes
        // through the matching end. Body: field 1 varint (0x08 0x01), inner group (0x0b ... 0x0c),
        // then this group's end (0x0c). Tags: SGroup=(f<<3)|3, EGroup=(f<<3)|4.
        let group = [
            0x08, 0x01, /*inner start f1*/ 0x0b, /*inner end f1*/ 0x0c,
            /*end f1*/ 0x0c,
        ];
        let mut c = Cursor::new(&group);
        assert!(c.skip(WireType::SGroup).is_ok());
        assert!(c.is_empty());

        // A stray group-end is malformed.
        assert_eq!(
            Cursor::new(&[]).skip(WireType::EGroup).unwrap_err().kind(),
            ErrorKind::InvalidWireType
        );
    }

    #[test]
    fn skip_group_depth_is_capped() {
        // A run of group-start tags with no ends must hit the depth cap, not recurse unboundedly.
        let starts = vec![0x0b_u8; (MAX_SKIP_DEPTH as usize) + 5];
        assert_eq!(
            Cursor::new(&starts)
                .skip(WireType::SGroup)
                .unwrap_err()
                .kind(),
            ErrorKind::DepthLimitExceeded
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
                while let Ok(Some(tag)) = c.read_tag_or_end() {
                    if c.skip(tag.wire).is_err() {
                        break;
                    }
                }
            }
        }
    }
}
