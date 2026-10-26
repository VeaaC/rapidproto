//! A borrowed view over a `map` field.

use crate::PackedSlice;

/// A borrowed view over a protobuf `map`'s decoded entries.
///
/// Like the C++ runtime, entries are stored as a flat slice and looked up by a linear scan (maps
/// are usually small, and this keeps decoding zero-copy with no index to build). The entries
/// borrow the arena/input for `'a`. A `string` key or value is a [`PackedStr`](crate::PackedStr)
/// and a `bytes` value a `PackedSlice<'a, u8>`, so `get("key")` looks up a string key directly.
///
/// Lookups follow protobuf's last-one-wins rule for a repeated key on the wire:
/// [`get`](MapView::get) scans from the end, and [`iter`](MapView::iter) yields entries in wire
/// order (so a duplicated key may appear more than once).
pub struct MapView<'a, K, V> {
    entries: PackedSlice<'a, (K, V)>,
}

// Manual `Copy`/`Clone` so the impls don't spuriously require `K: Copy` / `V: Copy` (the view only
// holds a shared slice reference).
impl<K, V> Clone for MapView<'_, K, V> {
    #[inline]
    fn clone(&self) -> Self {
        *self
    }
}
impl<K, V> Copy for MapView<'_, K, V> {}

impl<'a, K, V> MapView<'a, K, V> {
    /// Wrap a slice of decoded entries. The count truncates to 32 bits, as in [`PackedSlice`].
    #[doc(hidden)]
    #[inline]
    pub fn new(entries: &'a [(K, V)]) -> Self {
        Self {
            entries: PackedSlice::new(entries),
        }
    }

    /// Number of entries as decoded (duplicated keys counted).
    #[inline]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether the map has no entries.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// The raw entry slice, in wire order.
    #[inline]
    pub fn entries(&self) -> &'a [(K, V)] {
        self.entries.as_slice()
    }

    /// Iterate `(key, value)` pairs in wire order.
    #[inline]
    pub fn iter(&self) -> impl Iterator<Item = (&'a K, &'a V)> {
        let entries = self.entries.as_slice();
        entries.iter().map(|e| (&e.0, &e.1))
    }

    /// The value for `key`, or `None`. Last-one-wins: scans from the end, so a key repeated on the
    /// wire resolves to its final value. `O(n)`.
    #[inline]
    pub fn get<Q>(&self, key: &Q) -> Option<&'a V>
    where
        K: PartialEq<Q>,
        Q: ?Sized,
    {
        let entries = self.entries.as_slice();
        entries.iter().rev().find(|e| e.0.eq(key)).map(|e| &e.1)
    }
}

#[cfg(test)]
mod tests {
    extern crate alloc;
    use super::*;

    #[test]
    fn lookup_len_and_iter() {
        let entries = [(1i32, "a"), (2, "b"), (3, "c")];
        let m = MapView::new(&entries);
        assert_eq!(m.len(), 3);
        assert!(!m.is_empty());
        assert_eq!(m.get(&2), Some(&"b"));
        assert_eq!(m.get(&9), None);
        let collected: alloc::vec::Vec<_> = m.iter().map(|(k, v)| (*k, *v)).collect();
        assert_eq!(collected, [(1, "a"), (2, "b"), (3, "c")]);
    }

    #[test]
    fn duplicate_key_is_last_wins() {
        let entries = [(1i32, "first"), (1, "second")];
        let m = MapView::new(&entries);
        assert_eq!(m.get(&1), Some(&"second"));
    }

    #[test]
    fn empty() {
        let m: MapView<i32, i32> = MapView::new(&[]);
        assert!(m.is_empty());
        assert_eq!(m.get(&0), None);
    }
}
