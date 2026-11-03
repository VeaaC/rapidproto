//! `serde::Serialize` support for the runtime's borrowed types, plus helpers generated code uses. The
//! crate root documents the output.
//!
//! These impls live here (not in generated code) because the orphan rule requires `impl Serialize for`
//! a runtime type to be in this crate; the generated per-message impls (and the enum newtype impls) are
//! emitted by `rustgen` and reference `::rapidproto::serde` + these helpers.
//!
//! Generated code wraps `bytes` in [`Bytes`] because serde's blanket `impl Serialize for [T]` would call
//! `serialize_seq`, and a format with a byte type could not use it.

use serde::ser::{Serialize, SerializeMap, Serializer};

use crate::{MapView, PackedSlice, PackedStr, ProtoStr};

/// A protobuf `bytes` value. Serializes via `serialize_bytes`, so a format with a byte type can use it,
/// rather than serde's default sequence of `u8` for `&[u8]`.
pub struct Bytes<'a>(pub &'a [u8]);

impl Serialize for Bytes<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_bytes(self.0)
    }
}

/// A repeated `bytes` field: a sequence whose elements each serialize as [`Bytes`].
pub struct BytesSeq<'a>(pub &'a [PackedSlice<'a, u8>]);

impl Serialize for BytesSeq<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.collect_seq(self.0.iter().map(|b| Bytes(b)))
    }
}

impl Serialize for PackedStr<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        (**self).serialize(serializer)
    }
}

impl Serialize for ProtoStr {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        // A protobuf `string` is UTF-8 by spec; a decoder borrows the bytes without validating, so a
        // hostile payload could carry non-UTF-8. Serialize valid text as a string; fall back to the
        // raw bytes rather than lose data or panic. (No allocation on either path.)
        match self.to_str() {
            Ok(text) => serializer.serialize_str(text),
            Err(_) => serializer.serialize_bytes(self.as_bytes()),
        }
    }
}

impl<K: Serialize, V: Serialize> Serialize for MapView<'_, K, V> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(self.len()))?;
        for (key, value) in self.iter() {
            map.serialize_entry(key, value)?;
        }
        map.end()
    }
}

/// A `map` with `bytes` values: each value serializes as [`Bytes`] (opaque). Generated code uses
/// this for a bytes-valued map, whose `PackedSlice<u8>` values the plain `MapView` impl can't
/// serialize.
pub struct BytesMapView<'a, K>(pub MapView<'a, K, PackedSlice<'a, u8>>);

impl<K: Serialize> Serialize for BytesMapView<'_, K> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(self.0.len()))?;
        for (key, value) in self.0.iter() {
            map.serialize_entry(key, &Bytes(value))?;
        }
        map.end()
    }
}
