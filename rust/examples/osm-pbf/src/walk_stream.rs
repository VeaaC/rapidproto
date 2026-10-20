//! The STREAMING walk: compute the same [`Stats`] as [`walk`](crate::walk), but with the streaming
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
//! * **Macro for simple captures, hand-implemented visitors for aggregation.** Pass 1 uses the
//!   generated `*_stream!` macro: each selected field sets a disjoint local. The group and entity
//!   walks instead implement the generated `*Visitor` traits by hand, because every handler mutates the
//!   same `Stats` + key-count state -- one `&mut self` shared across all of them, which the macro's
//!   separate per-field closures (each needing `&mut stats`) could not express.
//!
//! The tag-key histogram is counted index-wise into a `Vec<u64>` (no key strings on the hot path) and
//! resolved against the stringtable once per block, identically to the arena walk.
//!
//! The two models print identical statistics for any protoc-valid file. They can differ only on input
//! protoc never produces: streaming is a raw per-occurrence field visit, so (unlike the arena, which
//! materializes and resolves singular fields) it does not reject a missing `required` field and counts
//! a duplicated singular message field more than once. Neither can crash or read out of bounds.

#![allow(non_upper_case_globals)] // the generated `WANT_<field>` associated consts

use rapidproto::stream::Flow;
use rapidproto::{DecodeError, ProtoStr};

use crate::osm::stream::*;
use crate::stats::{coord_nano, feature_supported, wrap_add, Stats};

/// Count one use of stringtable entry `k` as a tag key. Indexes come off the wire, so an out-of-range
/// one still counts as a tag (it just names no key) -- never an out-of-bounds panic.
fn count_key(keys: &mut [u64], k: usize) {
    if let Some(slot) = keys.get_mut(k) {
        *slot += 1;
    }
}

/// Decode one OSMData `PrimitiveBlock` payload into a per-block [`Stats`] via the streaming model.
pub fn walk_block_stream(payload: &[u8]) -> Result<Stats, String> {
    let bad_block = |e| format!("bad PrimitiveBlock ({e:?})");

    // Pass 1: the stringtable (tag keys index it) and the coordinate scaling. Each field sets a
    // disjoint local, so the `*_stream!` macro fits; absent fields keep their proto2 schema defaults.
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

    // Pass 2: walk the groups into the stats (hand-implemented visitors, below).
    let mut stats = Stats::default();
    let mut key_counts = vec![0u64; strings.len()];
    PrimitiveBlock::new(payload)
        .decode(primitive_block_stream! {
            primitivegroup => |g| g.decode(GroupWalk {
                stats: &mut stats,
                keys: &mut key_counts,
                gran: granularity,
                lat_off: lat_offset,
                lon_off: lon_offset,
            }),
        })
        .map_err(bad_block)?;

    stats.blocks = 1;
    stats.merge_block_keys(&key_counts, |i| strings[i]);
    Ok(stats)
}

/// Decode an OSMHeader `HeaderBlock` payload via streaming and refuse any required feature we do not
/// implement -- the streaming counterpart of the arena header check.
pub fn check_header_stream(payload: &[u8]) -> Result<(), String> {
    let mut unsupported: Option<String> = None;
    HeaderBlock::new(payload)
        .decode(header_block_stream! {
            required_features => |f: ProtoStr| {
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

/// One `PrimitiveGroup`: dispatch each entity kind to its own walker, counting the entity itself here
/// (so it is counted even if its fields are absent, matching the arena walk's per-message loop).
struct GroupWalk<'s> {
    stats: &'s mut Stats,
    keys: &'s mut [u64],
    gran: i64,
    lat_off: i64,
    lon_off: i64,
}

impl<'a> PrimitiveGroupVisitor<'a> for GroupWalk<'_> {
    const WANT_dense: bool = true;
    const WANT_nodes: bool = true;
    const WANT_ways: bool = true;
    const WANT_relations: bool = true;

    fn on_dense(&mut self, dense: DenseNodes<'a>) -> Result<Flow, DecodeError> {
        dense.decode(DenseWalk {
            stats: self.stats,
            keys: self.keys,
            gran: self.gran,
            lat_off: self.lat_off,
            lon_off: self.lon_off,
            id: 0,
            lat: 0,
            lon: 0,
            pending_key: None,
        })?;
        Ok(Flow::Continue)
    }

    fn on_nodes(&mut self, node: Node<'a>) -> Result<Flow, DecodeError> {
        self.stats.plain_nodes += 1;
        node.decode(NodeWalk {
            stats: self.stats,
            keys: self.keys,
            gran: self.gran,
            lat_off: self.lat_off,
            lon_off: self.lon_off,
        })?;
        Ok(Flow::Continue)
    }

    fn on_ways(&mut self, way: Way<'a>) -> Result<Flow, DecodeError> {
        self.stats.ways += 1;
        way.decode(WayWalk {
            stats: self.stats,
            keys: self.keys,
        })?;
        Ok(Flow::Continue)
    }

    fn on_relations(&mut self, rel: Relation<'a>) -> Result<Flow, DecodeError> {
        self.stats.relations += 1;
        rel.decode(RelationWalk {
            stats: self.stats,
            keys: self.keys,
        })?;
        Ok(Flow::Continue)
    }
}

/// `DenseNodes`: delta-coded id/lat/lon columns accumulated in locals, tags as a 0-terminated
/// (key, val) stream. The columns arrive one whole packed array at a time; per-axis aggregates need no
/// lat/lon pairing, so walking them independently matches the arena walk.
struct DenseWalk<'s> {
    stats: &'s mut Stats,
    keys: &'s mut [u64],
    gran: i64,
    lat_off: i64,
    lon_off: i64,
    id: u64,
    lat: i64,
    lon: i64,
    /// A tag key awaiting its value in the `keys_vals` stream (so the key is counted only once a value
    /// slot follows, as the arena walk's `i + 1 < len` requires).
    pending_key: Option<usize>,
}

impl<'a> DenseNodesVisitor<'a> for DenseWalk<'_> {
    const WANT_id: bool = true;
    const WANT_lat: bool = true;
    const WANT_lon: bool = true;
    const WANT_keys_vals: bool = true;
    const WANT_denseinfo: bool = true;

    fn on_id(&mut self, d: i64) -> Result<Flow, DecodeError> {
        self.id = self.id.wrapping_add(d as u64);
        self.stats.id_sum = self.stats.id_sum.wrapping_add(self.id);
        self.stats.dense_nodes += 1;
        Ok(Flow::Continue)
    }

    fn on_lat(&mut self, d: i64) -> Result<Flow, DecodeError> {
        self.lat = wrap_add(self.lat, d);
        self.stats
            .see_lat(coord_nano(self.lat_off, self.gran, self.lat));
        Ok(Flow::Continue)
    }

    fn on_lon(&mut self, d: i64) -> Result<Flow, DecodeError> {
        self.lon = wrap_add(self.lon, d);
        self.stats
            .see_lon(coord_nano(self.lon_off, self.gran, self.lon));
        Ok(Flow::Continue)
    }

    fn on_keys_vals(&mut self, x: i32) -> Result<Flow, DecodeError> {
        match self.pending_key.take() {
            // This element is the value for the pending key; count the key now.
            Some(k) => {
                self.stats.node_tags += 1;
                count_key(self.keys, k);
            }
            // 0 is a node boundary; any other value starts a (key, val) pair.
            None if x != 0 => self.pending_key = Some(x as usize),
            None => {}
        }
        Ok(Flow::Continue)
    }

    fn on_denseinfo(&mut self, info: DenseInfo<'a>) -> Result<Flow, DecodeError> {
        info.decode(DenseInfoCount { stats: self.stats })?;
        Ok(Flow::Continue)
    }
}

/// `DenseInfo`: one metadata row per `version` column entry.
struct DenseInfoCount<'s> {
    stats: &'s mut Stats,
}

impl<'a> DenseInfoVisitor<'a> for DenseInfoCount<'_> {
    const WANT_version: bool = true;

    fn on_version(&mut self, _v: i32) -> Result<Flow, DecodeError> {
        self.stats.info_rows += 1;
        Ok(Flow::Continue)
    }
}

/// A plain `Node`: a single id/lat/lon (not delta-coded) and packed tag-key ids.
struct NodeWalk<'s> {
    stats: &'s mut Stats,
    keys: &'s mut [u64],
    gran: i64,
    lat_off: i64,
    lon_off: i64,
}

impl<'a> NodeVisitor<'a> for NodeWalk<'_> {
    const WANT_id: bool = true;
    const WANT_lat: bool = true;
    const WANT_lon: bool = true;
    const WANT_keys: bool = true;
    const WANT_info: bool = true;

    fn on_id(&mut self, id: i64) -> Result<Flow, DecodeError> {
        self.stats.id_sum = self.stats.id_sum.wrapping_add(id as u64);
        Ok(Flow::Continue)
    }

    fn on_lat(&mut self, lat: i64) -> Result<Flow, DecodeError> {
        self.stats.see_lat(coord_nano(self.lat_off, self.gran, lat));
        Ok(Flow::Continue)
    }

    fn on_lon(&mut self, lon: i64) -> Result<Flow, DecodeError> {
        self.stats.see_lon(coord_nano(self.lon_off, self.gran, lon));
        Ok(Flow::Continue)
    }

    fn on_keys(&mut self, k: u32) -> Result<Flow, DecodeError> {
        self.stats.node_tags += 1;
        count_key(self.keys, k as usize);
        Ok(Flow::Continue)
    }

    fn on_info(&mut self, _info: Info<'a>) -> Result<Flow, DecodeError> {
        self.stats.info_rows += 1; // presence only, like the arena walk's `info().is_some()`
        Ok(Flow::Continue)
    }
}

/// A `Way`: an id, packed node refs (counted, not resolved), and tag keys.
struct WayWalk<'s> {
    stats: &'s mut Stats,
    keys: &'s mut [u64],
}

impl<'a> WayVisitor<'a> for WayWalk<'_> {
    const WANT_id: bool = true;
    const WANT_refs: bool = true;
    const WANT_keys: bool = true;
    const WANT_info: bool = true;

    fn on_id(&mut self, id: i64) -> Result<Flow, DecodeError> {
        self.stats.id_sum = self.stats.id_sum.wrapping_add(id as u64);
        Ok(Flow::Continue)
    }

    fn on_refs(&mut self, _ref: i64) -> Result<Flow, DecodeError> {
        self.stats.way_refs += 1;
        Ok(Flow::Continue)
    }

    fn on_keys(&mut self, k: u32) -> Result<Flow, DecodeError> {
        self.stats.way_tags += 1;
        count_key(self.keys, k as usize);
        Ok(Flow::Continue)
    }

    fn on_info(&mut self, _info: Info<'a>) -> Result<Flow, DecodeError> {
        self.stats.info_rows += 1;
        Ok(Flow::Continue)
    }
}

/// A `Relation`: an id, packed member ids (counted), and tag keys.
struct RelationWalk<'s> {
    stats: &'s mut Stats,
    keys: &'s mut [u64],
}

impl<'a> RelationVisitor<'a> for RelationWalk<'_> {
    const WANT_id: bool = true;
    const WANT_memids: bool = true;
    const WANT_keys: bool = true;
    const WANT_info: bool = true;

    fn on_id(&mut self, id: i64) -> Result<Flow, DecodeError> {
        self.stats.id_sum = self.stats.id_sum.wrapping_add(id as u64);
        Ok(Flow::Continue)
    }

    fn on_memids(&mut self, _memid: i64) -> Result<Flow, DecodeError> {
        self.stats.relation_members += 1;
        Ok(Flow::Continue)
    }

    fn on_keys(&mut self, k: u32) -> Result<Flow, DecodeError> {
        self.stats.relation_tags += 1;
        count_key(self.keys, k as usize);
        Ok(Flow::Continue)
    }

    fn on_info(&mut self, _info: Info<'a>) -> Result<Flow, DecodeError> {
        self.stats.info_rows += 1;
        Ok(Flow::Continue)
    }
}
