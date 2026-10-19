//! The statistics the `osmstat` tool accumulates and prints. Everything here is plain aggregation; it
//! knows nothing about protobuf or the file format, so it is not RapidProto-specific.
//!
//! All aggregates are order-independent (counts sum, bbox is per-axis min/max, the key histogram
//! merges by key), which is what lets the parallel path reduce per-block partials in any order and
//! still match the sequential run -- [`Stats::merge`] folds one block's (or one worker's) partial into
//! another.

use std::collections::HashMap;
use std::io::{self, Write};

#[derive(Default)]
pub struct Stats {
    pub blocks: u64,
    pub dense_nodes: u64,
    pub plain_nodes: u64,
    pub ways: u64,
    pub relations: u64,
    pub node_tags: u64,
    pub way_tags: u64,
    pub relation_tags: u64,
    pub way_refs: u64,
    pub relation_members: u64,
    /// Per-entity metadata rows seen (`Info` messages + `DenseInfo` columns).
    pub info_rows: u64,
    /// Wrapping sum of every entity id -- a cheap correctness pin.
    pub id_sum: u64,
    /// Total decoded payload bytes (post-inflate) -- a timing denominator, not printed to stdout.
    pub payload_bytes: u64,
    /// Bounding box in nanodegrees, per-axis (a bbox needs no lat/lon pairing).
    pub min_lat: i64,
    pub max_lat: i64,
    pub min_lon: i64,
    pub max_lon: i64,
    pub have_lat: bool,
    pub have_lon: bool,
    /// Tag-key histogram, resolved to strings. Per-block counting is index-wise (see
    /// [`merge_block_keys`](Stats::merge_block_keys)); this map is off the per-tag path.
    pub key_counts: HashMap<String, u64>,
}

impl Stats {
    pub fn see_lat(&mut self, nano: i64) {
        self.min_lat = if self.have_lat {
            self.min_lat.min(nano)
        } else {
            nano
        };
        self.max_lat = if self.have_lat {
            self.max_lat.max(nano)
        } else {
            nano
        };
        self.have_lat = true;
    }

    pub fn see_lon(&mut self, nano: i64) {
        self.min_lon = if self.have_lon {
            self.min_lon.min(nano)
        } else {
            nano
        };
        self.max_lon = if self.have_lon {
            self.max_lon.max(nano)
        } else {
            nano
        };
        self.have_lon = true;
    }

    /// Fold one block's index-wise key counts into the histogram: `counts[i]` = uses of stringtable
    /// entry `i` as a tag key, `string_at(i)` resolves the entry (copied only here).
    pub fn merge_block_keys<'a>(&mut self, counts: &[u64], string_at: impl Fn(usize) -> &'a [u8]) {
        for (i, &c) in counts.iter().enumerate() {
            if c == 0 {
                continue;
            }
            let key = String::from_utf8_lossy(string_at(i)).into_owned();
            *self.key_counts.entry(key).or_insert(0) += c;
        }
    }

    /// Combine another partial into this one (parallel reduce / per-block fold). Order-independent.
    pub fn merge(&mut self, other: Stats) {
        self.blocks += other.blocks;
        self.dense_nodes += other.dense_nodes;
        self.plain_nodes += other.plain_nodes;
        self.ways += other.ways;
        self.relations += other.relations;
        self.node_tags += other.node_tags;
        self.way_tags += other.way_tags;
        self.relation_tags += other.relation_tags;
        self.way_refs += other.way_refs;
        self.relation_members += other.relation_members;
        self.info_rows += other.info_rows;
        self.id_sum = self.id_sum.wrapping_add(other.id_sum);
        self.payload_bytes += other.payload_bytes;
        if other.have_lat {
            self.see_lat(other.min_lat);
            self.see_lat(other.max_lat);
        }
        if other.have_lon {
            self.see_lon(other.min_lon);
            self.see_lon(other.max_lon);
        }
        for (k, c) in other.key_counts {
            *self.key_counts.entry(k).or_insert(0) += c;
        }
    }

    /// Print the statistics (labels left-justified to width 17); the fixture test pins this to a golden.
    pub fn print(&self, out: &mut impl Write) -> io::Result<()> {
        writeln!(out, "{:<17}{}", "blocks", self.blocks)?;
        writeln!(
            out,
            "{:<17}{} (dense {}, plain {})",
            "nodes",
            self.dense_nodes + self.plain_nodes,
            self.dense_nodes,
            self.plain_nodes
        )?;
        writeln!(out, "{:<17}{} (refs {})", "ways", self.ways, self.way_refs)?;
        writeln!(
            out,
            "{:<17}{} (members {})",
            "relations", self.relations, self.relation_members
        )?;
        writeln!(
            out,
            "{:<17}{} (nodes {}, ways {}, relations {})",
            "tags",
            self.node_tags + self.way_tags + self.relation_tags,
            self.node_tags,
            self.way_tags,
            self.relation_tags
        )?;
        writeln!(out, "{:<17}{}", "metadata rows", self.info_rows)?;
        writeln!(out, "{:<17}{:016x}", "id sum", self.id_sum)?;
        if self.have_lat && self.have_lon {
            writeln!(
                out,
                "{:<17}lat [{:.7}, {:.7}]  lon [{:.7}, {:.7}]",
                "bbox",
                self.min_lat as f64 / 1e9,
                self.max_lat as f64 / 1e9,
                self.min_lon as f64 / 1e9,
                self.max_lon as f64 / 1e9
            )?;
        }
        // Top tag keys, count-descending, key as tiebreak: deterministic across runs and models.
        let mut top: Vec<(&String, &u64)> = self.key_counts.iter().collect();
        top.sort_by(|a, b| b.1.cmp(a.1).then_with(|| a.0.cmp(b.0)));
        writeln!(out, "{:<17}{}", "distinct keys", top.len())?;
        for (key, count) in top.iter().take(10) {
            writeln!(out, "  key {:<24} {}", key, count)?;
        }
        Ok(())
    }
}

/// A reader must refuse a file whose `required_features` it does not implement; these three are what
/// `osmstat` handles.
pub fn feature_supported(f: &str) -> bool {
    matches!(f, "OsmSchema-V0.6" | "DenseNodes" | "HistoricalInformation")
}

pub fn mib_per_s(bytes: u64, seconds: f64) -> f64 {
    if seconds > 0.0 {
        bytes as f64 / (1024.0 * 1024.0) / seconds
    } else {
        0.0
    }
}

/// `a + b` with wraparound instead of signed overflow: the deltas come straight off the wire, so an
/// adversarial file must wrap rather than panic in a release build.
pub fn wrap_add(a: i64, b: i64) -> i64 {
    (a as u64).wrapping_add(b as u64) as i64
}

/// `offset + granularity * raw`, in nanodegrees, computed in `u64` so untrusted wire values wrap
/// instead of overflowing signed arithmetic (bogus input yields a bogus coordinate, never a panic).
pub fn coord_nano(offset: i64, granularity: i64, raw: i64) -> i64 {
    (offset as u64).wrapping_add((granularity as u64).wrapping_mul(raw as u64)) as i64
}
