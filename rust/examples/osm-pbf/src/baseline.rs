//! Optional `--baseline`: decode the same file with the `osmpbf` crate (a materializing reader) and
//! print its counts + throughput to STDERR, beside ours. It is a comparison aside, not part of the
//! golden stdout -- and it covers the subset osmpbf exposes directly: `osmpbf` hands back already
//! delta-decoded ids and already offset/granularity-applied nanodegree coordinates, and iterates
//! elements rather than blocks, so `blocks` and the per-entity `metadata rows` have no clean analog
//! and are omitted here. The entity/tag/ref/member counts, id sum and bbox line up with our stats.

use std::collections::HashMap;

use osmpbf::{Element, ElementReader};

use crate::pbf_input::Stopwatch;
use crate::stats::mib_per_s;

#[derive(Default)]
struct B {
    dense: u64,
    plain: u64,
    ways: u64,
    relations: u64,
    node_tags: u64,
    way_tags: u64,
    relation_tags: u64,
    way_refs: u64,
    relation_members: u64,
    id_sum: u64,
    min_lat: i64,
    max_lat: i64,
    min_lon: i64,
    max_lon: i64,
    have_coord: bool,
    keys: HashMap<String, u64>,
}

impl B {
    fn coord(&mut self, lat: i64, lon: i64) {
        if self.have_coord {
            self.min_lat = self.min_lat.min(lat);
            self.max_lat = self.max_lat.max(lat);
            self.min_lon = self.min_lon.min(lon);
            self.max_lon = self.max_lon.max(lon);
        } else {
            self.min_lat = lat;
            self.max_lat = lat;
            self.min_lon = lon;
            self.max_lon = lon;
            self.have_coord = true;
        }
    }
    fn key(&mut self, k: &str) {
        *self.keys.entry(k.to_owned()).or_insert(0) += 1;
    }
}

pub fn run(path: &str) -> Result<(), String> {
    let mut clock = Stopwatch::new();
    let reader = ElementReader::from_path(path).map_err(|e| format!("osmpbf: open {path}: {e}"))?;
    let mut b = B::default();
    reader
        .for_each(|element| match element {
            Element::DenseNode(n) => {
                b.dense += 1;
                b.id_sum = b.id_sum.wrapping_add(n.id() as u64);
                b.coord(n.nano_lat(), n.nano_lon());
                for (k, _) in n.tags() {
                    b.node_tags += 1;
                    b.key(k);
                }
            }
            Element::Node(n) => {
                b.plain += 1;
                b.id_sum = b.id_sum.wrapping_add(n.id() as u64);
                b.coord(n.nano_lat(), n.nano_lon());
                for (k, _) in n.tags() {
                    b.node_tags += 1;
                    b.key(k);
                }
            }
            Element::Way(w) => {
                b.ways += 1;
                b.id_sum = b.id_sum.wrapping_add(w.id() as u64);
                b.way_refs += w.refs().count() as u64;
                for (k, _) in w.tags() {
                    b.way_tags += 1;
                    b.key(k);
                }
            }
            Element::Relation(r) => {
                b.relations += 1;
                b.id_sum = b.id_sum.wrapping_add(r.id() as u64);
                b.relation_members += r.members().count() as u64;
                for (k, _) in r.tags() {
                    b.relation_tags += 1;
                    b.key(k);
                }
            }
        })
        .map_err(|e| format!("osmpbf: read {path}: {e}"))?;
    let t = clock.take();

    let file_len = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    eprintln!("\nbaseline         osmpbf (for_each, 1 thread)");
    eprintln!(
        "  nodes {} (dense {}, plain {}), ways {} (refs {}), relations {} (members {})",
        b.dense + b.plain,
        b.dense,
        b.plain,
        b.ways,
        b.way_refs,
        b.relations,
        b.relation_members
    );
    eprintln!(
        "  tags {} (nodes {}, ways {}, relations {}), id sum {:016x}, distinct keys {}",
        b.node_tags + b.way_tags + b.relation_tags,
        b.node_tags,
        b.way_tags,
        b.relation_tags,
        b.id_sum,
        b.keys.len()
    );
    if b.have_coord {
        eprintln!(
            "  bbox lat [{:.7}, {:.7}]  lon [{:.7}, {:.7}]",
            b.min_lat as f64 / 1e9,
            b.max_lat as f64 / 1e9,
            b.min_lon as f64 / 1e9,
            b.max_lon as f64 / 1e9
        );
    }
    eprintln!(
        "  read+decode      {t:.3}s ({:.1} MiB/s)",
        mib_per_s(file_len, t)
    );
    Ok(())
}
