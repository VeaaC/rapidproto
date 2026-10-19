//! Read one already-decoded `PrimitiveBlock` into a [`Stats`]. This is the part that shows how you
//! consume RapidProto's arena views: decoding happened once in the caller's `decode()`, and every
//! accessor here just reads the arena -- repeated scalar columns come back as `&[i64]`, stringtable
//! entries as borrowed `&[u8]`, so the walk copies nothing. It returns a per-block `Stats` so the
//! caller can fold blocks on one thread or reduce them across many.

use crate::osm::OSMPBF::PrimitiveBlock;
use crate::stats::{coord_nano, wrap_add, Stats};

pub fn walk_block(block: &PrimitiveBlock) -> Stats {
    let mut stats = Stats::default();

    // proto2 `[default=100]`/`[default=0]` fields have explicit presence, so the accessor is an
    // `Option` and the schema default is applied here, not by the decoder.
    let granularity = block.granularity().unwrap_or(100) as i64;
    let lat_offset = block.lat_offset().unwrap_or(0);
    let lon_offset = block.lon_offset().unwrap_or(0);

    let strings: &[&[u8]] = block.stringtable().map(|st| st.s()).unwrap_or(&[]);
    let mut key_counts = vec![0u64; strings.len()];
    // Tag keys index the stringtable; indexes come off the wire, so bounds-check them (an
    // out-of-range index still counts as a tag, it just names no key).
    let mut count_key = |k: usize| {
        if k < key_counts.len() {
            key_counts[k] += 1;
        }
    };

    for group in block.primitivegroup() {
        if let Some(dense) = group.dense() {
            // Delta-coded columns, walked independently (per-axis aggregates need no pairing) so both
            // execution modes agree even on ragged column lengths.
            let mut id = 0u64;
            for &d in dense.id() {
                id = id.wrapping_add(d as u64);
                stats.id_sum = stats.id_sum.wrapping_add(id);
            }
            let mut lat = 0i64;
            for &d in dense.lat() {
                lat = wrap_add(lat, d);
                stats.see_lat(coord_nano(lat_offset, granularity, lat));
            }
            let mut lon = 0i64;
            for &d in dense.lon() {
                lon = wrap_add(lon, d);
                stats.see_lon(coord_nano(lon_offset, granularity, lon));
            }
            stats.dense_nodes += dense.id().len() as u64;
            // keys_vals: (key val)* pairs per node, 0-terminated. Tags only -- the node boundary
            // marker carries no data.
            let kv = dense.keys_vals();
            let mut i = 0;
            while i + 1 < kv.len() {
                if kv[i] == 0 {
                    i += 1; // node boundary
                    continue;
                }
                stats.node_tags += 1;
                count_key(kv[i] as usize);
                i += 2; // skip the value
            }
            if let Some(info) = dense.denseinfo() {
                stats.info_rows += info.version().len() as u64;
            }
        }
        for node in group.nodes() {
            stats.plain_nodes += 1;
            stats.id_sum = stats.id_sum.wrapping_add(node.id() as u64);
            stats.see_lat(coord_nano(lat_offset, granularity, node.lat()));
            stats.see_lon(coord_nano(lon_offset, granularity, node.lon()));
            for &k in node.keys() {
                stats.node_tags += 1;
                count_key(k as usize);
            }
            stats.info_rows += node.info().is_some() as u64;
        }
        for way in group.ways() {
            stats.ways += 1;
            stats.id_sum = stats.id_sum.wrapping_add(way.id() as u64);
            stats.way_refs += way.refs().len() as u64;
            for &k in way.keys() {
                stats.way_tags += 1;
                count_key(k as usize);
            }
            stats.info_rows += way.info().is_some() as u64;
        }
        for rel in group.relations() {
            stats.relations += 1;
            stats.id_sum = stats.id_sum.wrapping_add(rel.id() as u64);
            stats.relation_members += rel.memids().len() as u64;
            for &k in rel.keys() {
                stats.relation_tags += 1;
                count_key(k as usize);
            }
            stats.info_rows += rel.info().is_some() as u64;
        }
    }
    stats.blocks = 1;
    stats.merge_block_keys(&key_counts, |i| strings[i]);
    stats
}
