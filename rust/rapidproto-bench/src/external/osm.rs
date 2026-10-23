//! `osm_blocks` / `osm_file`: the C++ arena bench's OSM PBF scenarios (`tests/bench_arm_osm.cpp`; the
//! scenario design and the checksum convention are `tests/bench_arm_osm.hpp`'s), over the pinned
//! extract its dump writes.
//!
//! - `osm_blocks`: every PrimitiveBlock, pre-inflated by the dump, decoded and walked per call -- the
//!   protobuf layer alone. Arms: the arena decoder cold (baseline) and warm, and the streaming decoder
//!   (the C++ `arena-cold`, `arena-warm` and `streamgen`). The blocks sit back to back in one batch
//!   buffer, where C++ holds each in its own; the decoders read bytes, so alignment does not enter.
//! - `osm_file`: the raw file per call -- framing, the BlobHeader/Blob envelopes (through the arena
//!   model, as in C++), zlib inflate, decode, walk. Arms: the arena decoder warm (baseline) and the
//!   streaming decoder (the C++ `arena-warm` and `streamgen`). Inflate is zlib's `uncompress`, the call
//!   the C++ arms make -- through libz-sys, from the same system zlib where one is installed.
//!
//! Like the C++ arms, the streaming walk reads each block twice (stringtable first: wire order
//! guarantees nothing) and consumes every column the arena walk reads, and both streaming arms reuse
//! one string-table vector across blocks.

#![allow(non_upper_case_globals)] // the stream macros' generated `WANT_<field>` consts

#[allow(
    clippy::all,
    unused,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod generated {
    include!(concat!(env!("OUT_DIR"), "/fileformat.rs"));
}

use std::cell::RefCell;

use rapidproto::Arena;

use crate::payloads::frames;
use crate::Arm;
use generated::arena::OSMPBF::{blob::Data, Blob, BlobHeader, HeaderBlock, Info, PrimitiveBlock};

pub fn blocks_arms() -> Vec<Arm> {
    let warm = RefCell::new(Arena::new());
    let strings = RefCell::new(Vec::new());
    vec![
        Arm::new("rapidproto", |b| {
            frames(b).fold(0u64, |h, p| {
                let arena = Arena::new();
                h.wrapping_add(arena_block_sum(decode_block(p, &arena)))
            })
        }),
        Arm::new("rapidproto-warm", move |b| {
            let mut arena = warm.borrow_mut();
            frames(b).fold(0u64, |h, p| {
                arena.reset();
                h.wrapping_add(arena_block_sum(decode_block(p, &arena)))
            })
        }),
        Arm::new("rapidproto-stream", move |b| {
            let mut strings = strings.borrow_mut();
            frames(b).fold(0u64, |h, p| {
                h.wrapping_add(stream_block_sum(p, &mut strings))
            })
        }),
    ]
}

pub fn file_arms() -> Vec<Arm> {
    let warm = RefCell::new(Arena::new());
    let warm_inflated = RefCell::new(Vec::new());
    let strings = RefCell::new(Vec::new());
    let stream_inflated = RefCell::new(Vec::new());
    vec![
        Arm::new("rapidproto-warm", move |b| {
            let mut arena = warm.borrow_mut();
            file_sum(b, &mut warm_inflated.borrow_mut(), |p| {
                arena.reset();
                arena_block_sum(decode_block(p, &arena))
            })
        }),
        Arm::new("rapidproto-stream", move |b| {
            let mut strings = strings.borrow_mut();
            file_sum(b, &mut stream_inflated.borrow_mut(), |p| {
                stream_block_sum(p, &mut strings)
            })
        }),
    ]
}

fn decode_block<'a>(payload: &'a [u8], arena: &'a Arena<'a>) -> &'a PrimitiveBlock<'a> {
    PrimitiveBlock::decode(payload, arena).expect("rapidproto decode")
}

// --- the checksum, one spec for every arm (bench_arm_osm.hpp) -------------------------------------

const NODE: u64 = 3;
const WAY: u64 = 5;
const RELATION: u64 = 7;

// The C++ `u()`: a signed value widened to int64, then reinterpreted -- negative ids/uids fold the
// same bit pattern everywhere.
fn u(v: i64) -> u64 {
    v as u64
}

// osmium's coordinate conversion, replicated exactly: (raw*granularity + offset)/100 as int32.
fn loc(raw: i64, granularity: i64, offset: i64) -> u64 {
    u(i64::from(
        (raw.wrapping_mul(granularity).wrapping_add(offset) / 100) as i32,
    ))
}

// The first byte of string-table entry `i` (0 for an empty entry or an index past the table): forces
// the stringtable resolution without measuring strlen.
fn first_byte(strings: &[&[u8]], i: u64) -> u64 {
    usize::try_from(i)
        .ok()
        .and_then(|i| strings.get(i))
        .and_then(|s| s.first())
        .map_or(0, |&b| u64::from(b))
}

fn wadd(h: &mut u64, v: u64) {
    *h = h.wrapping_add(v);
}

// --- arena walk -----------------------------------------------------------------------------------

fn arena_block_sum(b: &PrimitiveBlock) -> u64 {
    let gran = i64::from(b.granularity().unwrap_or(100));
    let lat_off = b.lat_offset().unwrap_or(0);
    let lon_off = b.lon_offset().unwrap_or(0);
    let date_gran = i64::from(b.date_granularity().unwrap_or(1000));
    let st: &[&[u8]] = b.stringtable().map_or(&[], |t| t.s());
    let sv = |i: u64| first_byte(st, i);
    let info_sum = |info: Option<&Info>| {
        info.map_or(0, |info| {
            u(i64::from(info.version().unwrap_or(0)))
                .wrapping_add(u(
                    info.timestamp().unwrap_or(0).wrapping_mul(date_gran) / 1000
                ))
                .wrapping_add(u(info.changeset().unwrap_or(0)))
                .wrapping_add(u(i64::from(info.uid().unwrap_or(0))))
                .wrapping_add(sv(u64::from(info.user_sid().unwrap_or(0))))
        })
    };

    let mut h = 0u64;
    for g in b.primitivegroup() {
        if let Some(d) = g.dense() {
            let (mut id, mut lat, mut lon) = (0i64, 0i64, 0i64);
            for ((&di, &dl), &dn) in d.id().iter().zip(d.lat()).zip(d.lon()) {
                id = id.wrapping_add(di);
                lat = lat.wrapping_add(dl);
                lon = lon.wrapping_add(dn);
                wadd(
                    &mut h,
                    NODE.wrapping_mul(u(id))
                        .wrapping_add(loc(lat, gran, lat_off))
                        .wrapping_add(loc(lon, gran, lon_off)),
                );
            }
            if let Some(di) = d.denseinfo() {
                for &v in di.version() {
                    wadd(&mut h, u(i64::from(v)));
                }
                let mut t = 0i64;
                for &d in di.timestamp() {
                    t = t.wrapping_add(d);
                    wadd(&mut h, u(t.wrapping_mul(date_gran) / 1000));
                }
                let mut c = 0i64;
                for &d in di.changeset() {
                    c = c.wrapping_add(d);
                    wadd(&mut h, u(c));
                }
                let mut ui = 0i64;
                for &d in di.uid() {
                    ui = ui.wrapping_add(i64::from(d));
                    wadd(&mut h, u(ui));
                }
                let mut us = 0i64;
                for &d in di.user_sid() {
                    us = us.wrapping_add(i64::from(d));
                    wadd(&mut h, sv(u(us)));
                }
            }
            let kv = d.keys_vals();
            let mut i = 0;
            while i + 1 < kv.len() {
                if kv[i] != 0 {
                    wadd(&mut h, sv(u64::from(kv[i] as u32)));
                    i += 1; // the value slot
                    wadd(&mut h, sv(u64::from(kv[i] as u32)));
                }
                i += 1;
            }
        }
        for node in g.nodes() {
            wadd(
                &mut h,
                NODE.wrapping_mul(u(node.id()))
                    .wrapping_add(loc(node.lat(), gran, lat_off))
                    .wrapping_add(loc(node.lon(), gran, lon_off))
                    .wrapping_add(info_sum(node.info())),
            );
            for &k in node.keys().iter().chain(node.vals()) {
                wadd(&mut h, sv(u64::from(k)));
            }
        }
        for way in g.ways() {
            wadd(
                &mut h,
                WAY.wrapping_mul(u(way.id()))
                    .wrapping_add(info_sum(way.info())),
            );
            let mut r = 0i64;
            for &d in way.refs() {
                r = r.wrapping_add(d);
                wadd(&mut h, u(r));
            }
            for &k in way.keys().iter().chain(way.vals()) {
                wadd(&mut h, sv(u64::from(k)));
            }
        }
        for rel in g.relations() {
            wadd(
                &mut h,
                RELATION
                    .wrapping_mul(u(rel.id()))
                    .wrapping_add(info_sum(rel.info())),
            );
            let mut m = 0i64;
            for &d in rel.memids() {
                m = m.wrapping_add(d);
                wadd(&mut h, u(m));
            }
            for &k in rel.keys().iter().chain(rel.vals()) {
                wadd(&mut h, sv(u64::from(k)));
            }
            for &r in rel.roles_sid() {
                wadd(&mut h, sv(u64::from(r as u32)));
            }
            for ty in rel.types() {
                wadd(&mut h, u(i64::from(ty.0)));
            }
        }
    }
    h
}

// --- streaming walk -------------------------------------------------------------------------------

/// The second pass's state, lent to every handler: the block's scaling and string table, the sum,
/// and the delta accumulators -- a group's reset at each group, a way's / relation's at each entity.
struct Walk<'s, 'a> {
    strings: &'s [&'a [u8]],
    gran: i64,
    lat_off: i64,
    lon_off: i64,
    date_gran: i64,
    h: u64,
    dn_id: i64,
    dn_lat: i64,
    dn_lon: i64,
    di_t: i64,
    di_c: i64,
    di_ui: i64,
    di_us: i64,
    // A key awaiting its value in `keys_vals` (explicit: a NEGATIVE key is valid and must still pair).
    pending: Option<i32>,
    way_ref: i64,
    rel_mem: i64,
}

impl Walk<'_, '_> {
    fn add(&mut self, v: u64) {
        self.h = self.h.wrapping_add(v);
    }
    fn sv(&self, i: u64) -> u64 {
        first_byte(self.strings, i)
    }
}

/// `scratch` keeps its allocation across blocks: emptied and re-borrowed per block, as the C++ arms
/// clear one vector.
fn stream_block_sum(payload: &[u8], scratch: &mut Vec<&'static [u8]>) -> u64 {
    use generated::stream::OSMPBF::*;
    let mut strings = recycle(std::mem::take(scratch));
    let (mut gran, mut lat_off, mut lon_off, mut date_gran) = (100i64, 0i64, 0i64, 1000i64);
    PrimitiveBlock::new(payload)
        .decode(primitive_block_stream! {
            stringtable => |t| t.decode(string_table_stream! { s => |s| strings.push(s) }),
            granularity => |v| gran = i64::from(v),
            lat_offset => |v| lat_off = v,
            lon_offset => |v| lon_off = v,
            date_granularity => |v| date_gran = i64::from(v),
        })
        .expect("rapidproto stream decode");

    let mut w = Walk {
        strings: &strings,
        gran,
        lat_off,
        lon_off,
        date_gran,
        h: 0,
        dn_id: 0,
        dn_lat: 0,
        dn_lon: 0,
        di_t: 0,
        di_c: 0,
        di_ui: 0,
        di_us: 0,
        pending: None,
        way_ref: 0,
        rel_mem: 0,
    };
    PrimitiveBlock::new(payload)
        .decode(primitive_block_stream!(&mut w; {
            primitivegroup => |w, group| {
                (w.dn_id, w.dn_lat, w.dn_lon) = (0, 0, 0);
                (w.di_t, w.di_c, w.di_ui, w.di_us) = (0, 0, 0, 0);
                (w.pending, w.way_ref, w.rel_mem) = (None, 0, 0);
                group.decode(primitive_group_stream!(&mut *w; {
                    dense => |w, dense| dense.decode(dense_nodes_stream!(&mut *w; {
                        id => |w, d| {
                            w.dn_id = w.dn_id.wrapping_add(d);
                            w.add(NODE.wrapping_mul(u(w.dn_id)));
                        },
                        lat => |w, d| {
                            w.dn_lat = w.dn_lat.wrapping_add(d);
                            w.add(loc(w.dn_lat, w.gran, w.lat_off));
                        },
                        lon => |w, d| {
                            w.dn_lon = w.dn_lon.wrapping_add(d);
                            w.add(loc(w.dn_lon, w.gran, w.lon_off));
                        },
                        keys_vals => |w, v| match w.pending.take() {
                            // v is the value of the pending key
                            Some(k) => {
                                let key = w.sv(u64::from(k as u32));
                                w.add(key.wrapping_add(w.sv(u64::from(v as u32))));
                            }
                            None if v != 0 => w.pending = Some(v),
                            None => {}
                        },
                        denseinfo => |w, di| di.decode(dense_info_stream!(&mut *w; {
                            version => |w, v| w.add(u(i64::from(v))),
                            timestamp => |w, d| {
                                w.di_t = w.di_t.wrapping_add(d);
                                w.add(u(w.di_t.wrapping_mul(w.date_gran) / 1000));
                            },
                            changeset => |w, d| {
                                w.di_c = w.di_c.wrapping_add(d);
                                w.add(u(w.di_c));
                            },
                            uid => |w, d| {
                                w.di_ui = w.di_ui.wrapping_add(i64::from(d));
                                w.add(u(w.di_ui));
                            },
                            user_sid => |w, d| {
                                w.di_us = w.di_us.wrapping_add(i64::from(d));
                                w.add(w.sv(u(w.di_us)));
                            },
                        })),
                    })),
                    nodes => |w, node| node.decode(node_stream!(&mut *w; {
                        id => |w, id| w.add(NODE.wrapping_mul(u(id))),
                        lat => |w, v| w.add(loc(v, w.gran, w.lat_off)),
                        lon => |w, v| w.add(loc(v, w.gran, w.lon_off)),
                        keys => |w, k| w.add(w.sv(u64::from(k))),
                        vals => |w, v| w.add(w.sv(u64::from(v))),
                        info => |w, info| stream_info(w, info),
                    })),
                    ways => |w, way| {
                        w.way_ref = 0;
                        way.decode(way_stream!(&mut *w; {
                            id => |w, id| w.add(WAY.wrapping_mul(u(id))),
                            refs => |w, d| {
                                w.way_ref = w.way_ref.wrapping_add(d);
                                w.add(u(w.way_ref));
                            },
                            keys => |w, k| w.add(w.sv(u64::from(k))),
                            vals => |w, v| w.add(w.sv(u64::from(v))),
                            info => |w, info| stream_info(w, info),
                        }))
                    },
                    relations => |w, rel| {
                        w.rel_mem = 0;
                        rel.decode(relation_stream!(&mut *w; {
                            id => |w, id| w.add(RELATION.wrapping_mul(u(id))),
                            memids => |w, d| {
                                w.rel_mem = w.rel_mem.wrapping_add(d);
                                w.add(u(w.rel_mem));
                            },
                            keys => |w, k| w.add(w.sv(u64::from(k))),
                            vals => |w, v| w.add(w.sv(u64::from(v))),
                            roles_sid => |w, r| w.add(w.sv(u64::from(r as u32))),
                            types => |w, ty| w.add(u(i64::from(ty.0))),
                            info => |w, info| stream_info(w, info),
                        }))
                    },
                }))
            },
        }))
        .expect("rapidproto stream decode");
    let h = w.h;
    *scratch = recycle(strings);
    h
}

fn stream_info(
    w: &mut Walk,
    info: generated::stream::OSMPBF::Info,
) -> Result<(), rapidproto::DecodeError> {
    use generated::stream::OSMPBF::*;
    info.decode(info_stream!(&mut *w; {
        version => |w, v| w.add(u(i64::from(v))),
        timestamp => |w, v| w.add(u(v.wrapping_mul(w.date_gran) / 1000)),
        changeset => |w, v| w.add(u(v)),
        uid => |w, v| w.add(u(i64::from(v))),
        user_sid => |w, v| w.add(w.sv(u64::from(v))),
    }))
}

/// `v`, emptied and re-borrowed for another lifetime, keeping its allocation (an in-place `collect`:
/// the element layout is unchanged), so a string-table vector outlives the blocks it borrows from.
fn recycle<'b>(mut v: Vec<&[u8]>) -> Vec<&'b [u8]> {
    v.clear();
    v.into_iter().map(|_| unreachable!()).collect()
}

// --- the whole-file driver (osm_file) -------------------------------------------------------------

/// Frame, envelope-decode and inflate every blob of the raw `file`, feeding each OSMData payload to
/// `block` and summing what it returns (`inflated` is the reused inflate buffer). An OSMHeader block is
/// decoded and discarded, the work libosmium's reader does too. 0 on any malformed input.
pub fn file_sum(file: &[u8], inflated: &mut Vec<u8>, mut block: impl FnMut(&[u8]) -> u64) -> u64 {
    let mut h = 0u64;
    let mut offset = 0usize;
    while offset < file.len() {
        let arena = Arena::new();
        let Some(header) =
            next_frame(file, &mut offset).and_then(|b| BlobHeader::decode(b, &arena).ok())
        else {
            return 0;
        };
        let Some(blob) = take_blob(file, &mut offset, header.datasize())
            .and_then(|b| Blob::decode(b, &arena).ok())
        else {
            return 0;
        };
        let payload: &[u8] = match blob.data() {
            Some(&Data::Raw(raw)) => raw,
            Some(&Data::ZlibData(deflated)) => {
                let raw_size = blob.raw_size().unwrap_or(0);
                if raw_size <= 0
                    || i64::from(raw_size) > MAX_RAW_SIZE
                    || !inflate(deflated, raw_size as usize, inflated)
                {
                    return 0;
                }
                &inflated[..]
            }
            _ => return 0,
        };
        if header.r#type() == "OSMData" {
            h = h.wrapping_add(block(payload));
        } else if header.r#type() == "OSMHeader"
            && HeaderBlock::decode(payload, &Arena::new()).is_err()
        {
            return 0;
        }
    }
    h
}

// The PBF spec's bounds (examples/osm-pbf/pbf_input.hpp): a BlobHeader under 64 KiB, a Blob under
// 32 MiB, and an inflated payload under twice that.
const MAX_BLOB_HEADER: usize = 64 * 1024;
const MAX_BLOB: usize = 32 * 1024 * 1024;
const MAX_RAW_SIZE: i64 = 2 * MAX_BLOB as i64;

/// The BlobHeader bytes at `*offset` (after the 4-byte big-endian length), advancing past them.
fn next_frame<'a>(file: &'a [u8], offset: &mut usize) -> Option<&'a [u8]> {
    let (len, rest) = file.get(*offset..)?.split_first_chunk::<4>()?;
    let len = u32::from_be_bytes(*len) as usize;
    if len >= MAX_BLOB_HEADER || rest.len() < len {
        return None;
    }
    *offset += 4 + len;
    Some(&rest[..len])
}

/// The Blob bytes after a frame's header, sized by the BlobHeader's `datasize`.
fn take_blob<'a>(file: &'a [u8], offset: &mut usize, datasize: i32) -> Option<&'a [u8]> {
    let len = usize::try_from(datasize).ok().filter(|&n| n <= MAX_BLOB)?;
    let blob = file.get(*offset..)?.get(..len)?;
    *offset += len;
    Some(blob)
}

/// Inflate `deflated` into `out`, resized to `raw_size`, with zlib's one-shot `uncompress` -- what the
/// C++ arms call (examples/osm-pbf/pbf_input.hpp). False unless it yields exactly `raw_size` bytes.
fn inflate(deflated: &[u8], raw_size: usize, out: &mut Vec<u8>) -> bool {
    let (Ok(mut len), Ok(src_len)) = (
        libz_sys::uLongf::try_from(raw_size),
        libz_sys::uLong::try_from(deflated.len()),
    ) else {
        return false;
    };
    out.resize(raw_size, 0);
    // SAFETY: `out` holds `raw_size` initialized bytes and `len` says so (zlib writes at most `len` and
    // reports how many it wrote); `deflated` is a live slice of the length passed.
    let rc =
        unsafe { libz_sys::uncompress(out.as_mut_ptr(), &mut len, deflated.as_ptr(), src_len) };
    rc == libz_sys::Z_OK && usize::try_from(len) == Ok(raw_size)
}
