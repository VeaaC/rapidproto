//! `serde::Serialize` for the runtime's borrowed types. The crate root documents the output.
//!
//! These impls live here, not in generated code, because the orphan rule requires `impl Serialize for`
//! a runtime type to be in this crate. Generated code emits the per-message and enum impls, which
//! reach serde through the `::rapidproto::serde` re-export.

use serde::ser::{Serialize, SerializeMap, Serializer};

use crate::{MapView, ProtoBytes, ProtoStr};

impl Serialize for ProtoStr<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        // A decoder borrows the bytes without validating them, so a hostile payload can carry invalid
        // UTF-8. Serialize valid text as a string, and fall back to the raw bytes rather than lose data
        // or panic. Neither path allocates.
        match self.to_str() {
            Ok(text) => serializer.serialize_str(text),
            Err(_) => serializer.serialize_bytes(self.as_bytes()),
        }
    }
}

// `serialize_bytes`, so a format with a byte type can use it, rather than serde's sequence of `u8`.
impl Serialize for ProtoBytes<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_bytes(self.as_slice())
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
