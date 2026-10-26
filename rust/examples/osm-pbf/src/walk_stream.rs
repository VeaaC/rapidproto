//! The STREAMING walk: compute the same [`Stats`] as [`walk_arena`](crate::walk_arena), but with the streaming
//! visitor model instead of materialized arena views -- nothing is allocated for the message, each
//! field is handled by a callback as it is decoded.
//!
//! Two things this example shows about streaming a real schema:
//!
//! * **Two passes per block.** Tag keys index the stringtable and coordinates need the block's scaling
//!   fields, but the wire does not guarantee those precede the groups that use them -- and a streaming
//!   decoder cannot look back. So pass 1 collects just the stringtable + scaling (skipping everything
//!   else, which is exactly what streaming is cheap at), and pass 2 walks the groups.
//!
//! * **One shared accumulator, not a visitor per message.** Pass 1 fills disjoint locals, so it uses
//!   the plain `*_stream!` macro. Pass 2 aggregates every entity into one [`Agg`], so it uses the
//!   macro's shared-state form, `*_stream!(&mut agg; { field => |agg, value| .. })`: the decoder lends
//!   the same `&mut agg` to every handler at every nesting level (re-threaded with `&mut *agg`), which
//!   exclusive-`&mut` closure captures could not do. The whole walk is one nested expression that
//!   mirrors the message tree -- block -> group -> entity -> metadata. The only explicit struct is
//!   [`Dense`], holding the per-group delta accumulators that reset at each `DenseNodes` (there is no
//!   enclosing scope to park them in, the way a nested closure walk would have).
//!
//! The tag-key histogram is counted index-wise into a `Vec<u64>` (no key strings on the hot path) and
//! resolved against the stringtable once per block, identically to the arena walk.
//!
//! The two models print identical statistics for any protoc-valid file. They can differ only on input
//! protoc never produces: streaming is a raw per-occurrence field visit, so (unlike the arena, which
//! materializes and resolves singular fields) it does not reject a missing `required` field and counts
//! a duplicated singular message field more than once. Neither can crash or read out of bounds.

#![allow(non_upper_case_globals)] // the generated `WANT_<field>` associated consts

use rapidproto::ProtoStr;

use crate::osm::stream::*;
use crate::stats::{coord_nano, feature_supported, wrap_add, Stats};

/// Count one use of stringtable entry `k` as a tag key. Indexes come off the wire, so an out-of-range
/// one still counts as a tag (it just names no key) -- never an out-of-bounds panic.
fn count_key(keys: &mut [u64], k: usize) {
    if let Some(slot) = keys.get_mut(k) {
        *slot += 1;
    }
}

/// Everything the group walk aggregates into: the block's running [`Stats`], the index-wise tag-key
/// histogram, and the coordinate scaling read in pass 1. One `&mut Agg` is lent to every handler.
struct Agg<'s> {
    stats: &'s mut Stats,
    keys: &'s mut [u64],
    gran: i64,
    lat_off: i64,
    lon_off: i64,
}

/// A `DenseNodes` group's per-group state: the shared [`Agg`] plus the delta-coded id/lat/lon
/// accumulators and the `keys_vals` cursor, all of which reset at each group (hence a fresh `Dense` per
/// `on_dense`). `pending_key` holds a tag key awaiting its value in the `(key, val)*` stream.
struct Dense<'a, 's> {
    agg: &'a mut Agg<'s>,
    id: u64,
    lat: i64,
    lon: i64,
    pending_key: Option<usize>,
}

/// Decode one OSMData `PrimitiveBlock` payload into a per-block [`Stats`] via the streaming model.
pub fn walk_block_stream(payload: &[u8]) -> Result<Stats, String> {
    let bad_block = |e| format!("bad PrimitiveBlock ({e:?})");

    // Pass 1: the stringtable (tag keys index it) and the coordinate scaling. Each field sets a
    // disjoint local, so the plain macro fits; absent fields keep their proto2 schema defaults.
    let mut strings: Vec<&[u8]> = Vec::new();
    let mut granularity = 100i64;
    let mut lat_offset = 0i64;
    let mut lon_offset = 0i64;
    PrimitiveBlock::new(payload)
        .decode(primitive_block_stream! {
            stringtable => |st| st.decode(string_table_stream! { s => |b| strings.push(b) }),
            granularity => |v| granularity = v as i64,
            lat_offset => |v| lat_offset = v,
            lon_offset => |v| lon_offset = v,
        })
        .map_err(bad_block)?;

    // Pass 2: walk the groups into one shared `Agg` via the macro's shared-state form.
    let mut stats = Stats::default();
    let mut key_counts = vec![0u64; strings.len()];
    let mut agg = Agg {
        stats: &mut stats,
        keys: &mut key_counts,
        gran: granularity,
        lat_off: lat_offset,
        lon_off: lon_offset,
    };
    PrimitiveBlock::new(payload)
        .decode(primitive_block_stream!(&mut agg; {
            primitivegroup => |agg, g| g.decode(primitive_group_stream!(&mut *agg; {
                // Plain Node: a single id/lat/lon (not delta-coded) and packed tag-key ids.
                nodes => |agg, node| {
                    agg.stats.plain_nodes += 1;
                    node.decode(node_stream!(&mut *agg; {
                        id => |agg, id| agg.stats.id_sum = agg.stats.id_sum.wrapping_add(id as u64),
                        lat => |agg, lat| agg.stats.see_lat(coord_nano(agg.lat_off, agg.gran, lat)),
                        lon => |agg, lon| agg.stats.see_lon(coord_nano(agg.lon_off, agg.gran, lon)),
                        keys => |agg, k| { agg.stats.node_tags += 1; count_key(agg.keys, k as usize); },
                        info => |agg, _info| agg.stats.info_rows += 1, // presence only, like the arena walk
                    }))
                },
                // Way: an id, packed node refs (counted, not resolved), and tag keys.
                ways => |agg, way| {
                    agg.stats.ways += 1;
                    way.decode(way_stream!(&mut *agg; {
                        id => |agg, id| agg.stats.id_sum = agg.stats.id_sum.wrapping_add(id as u64),
                        refs => |agg, _ref| agg.stats.way_refs += 1,
                        keys => |agg, k| { agg.stats.way_tags += 1; count_key(agg.keys, k as usize); },
                        info => |agg, _info| agg.stats.info_rows += 1,
                    }))
                },
                // Relation: an id, packed member ids (counted), and tag keys.
                relations => |agg, rel| {
                    agg.stats.relations += 1;
                    rel.decode(relation_stream!(&mut *agg; {
                        id => |agg, id| agg.stats.id_sum = agg.stats.id_sum.wrapping_add(id as u64),
                        memids => |agg, _memid| agg.stats.relation_members += 1,
                        keys => |agg, k| { agg.stats.relation_tags += 1; count_key(agg.keys, k as usize); },
                        info => |agg, _info| agg.stats.info_rows += 1,
                    }))
                },
                // DenseNodes: delta-coded id/lat/lon columns and a 0-terminated (key, val) tag stream,
                // accumulated in a per-group `Dense` (the columns arrive one whole packed array at a
                // time; per-axis aggregates need no lat/lon pairing, matching the arena walk).
                dense => |agg, d| {
                    let mut den = Dense { agg, id: 0, lat: 0, lon: 0, pending_key: None };
                    d.decode(dense_nodes_stream!(&mut den; {
                        id => |den, delta| {
                            den.id = den.id.wrapping_add(delta as u64);
                            den.agg.stats.id_sum = den.agg.stats.id_sum.wrapping_add(den.id);
                            den.agg.stats.dense_nodes += 1;
                        },
                        lat => |den, delta| {
                            den.lat = wrap_add(den.lat, delta);
                            let nano = coord_nano(den.agg.lat_off, den.agg.gran, den.lat);
                            den.agg.stats.see_lat(nano);
                        },
                        lon => |den, delta| {
                            den.lon = wrap_add(den.lon, delta);
                            let nano = coord_nano(den.agg.lon_off, den.agg.gran, den.lon);
                            den.agg.stats.see_lon(nano);
                        },
                        keys_vals => |den, x| match den.pending_key.take() {
                            // This element is the value for the pending key; count the key now.
                            Some(k) => {
                                den.agg.stats.node_tags += 1;
                                count_key(den.agg.keys, k);
                            }
                            // 0 is a node boundary; any other value starts a (key, val) pair.
                            None if x != 0 => den.pending_key = Some(x as usize),
                            None => {}
                        },
                        // One metadata row per `version` column entry.
                        denseinfo => |den, info| info.decode(dense_info_stream!(&mut *den.agg; {
                            version => |agg, _v| agg.stats.info_rows += 1,
                        })),
                    }))
                },
            })),
        }))
        .map_err(bad_block)?;

    stats.blocks = 1;
    stats.merge_block_keys(&key_counts, |i| strings[i]);
    Ok(stats)
}

/// Decode an OSMHeader `HeaderBlock` payload via streaming and refuse any required feature we do not
/// implement -- the streaming counterpart of the arena header check. One disjoint local, so the plain
/// macro fits.
pub fn check_header_stream(payload: &[u8]) -> Result<(), String> {
    let mut unsupported: Option<String> = None;
    HeaderBlock::new(payload)
        .decode(header_block_stream! {
            required_features => |f: &ProtoStr| {
                if unsupported.is_none() {
                    let name = String::from_utf8_lossy(f.as_bytes());
                    if !feature_supported(&name) {
                        unsupported = Some(name.into_owned());
                    }
                }
            },
        })
        .map_err(|e| format!("bad OSMHeader block ({e:?})"))?;
    match unsupported {
        Some(name) => Err(format!("file requires unsupported feature '{name}'")),
        None => Ok(()),
    }
}
