//! RapidProto's arms on the shared shapes, over the golden crate's generated code (what a consumer
//! compiles): the cold and warm arena arms, plus the streaming decoder on `dataset`, all summing the
//! same fields ([`checksum`](crate::checksum)).

use rapidproto::Arena;
use rustgen_golden::bench::arena::bench::{Big, BigSet, Dataset, ParticleSet, RecordSet, WideSet};

use crate::{checksum, Arm};

pub fn dataset() -> Vec<Arm> {
    let mut arms = arena_arms!(Dataset, checksum::checksum_dataset);
    arms.push(Arm::new("rapidproto-stream", stream_dataset));
    arms
}

/// `dataset`'s memory report (see [`MemFn`](crate::runner::MemFn)), the C++ arena bench's `mem` record.
pub fn dataset_memory(b: &[u8]) -> (usize, usize) {
    let arena = Arena::new();
    Dataset::decode(b, &arena).expect("rapidproto decode");
    (arena.bytes_used(), arena.bytes_reserved())
}

pub fn bigset() -> Vec<Arm> {
    arena_arms!(BigSet, checksum::checksum_bigset)
}

pub fn big() -> Vec<Arm> {
    arena_arms!(Big, checksum::checksum_big)
}

pub fn wideset() -> Vec<Arm> {
    arena_arms!(WideSet, checksum::checksum_wideset)
}

pub fn recordset() -> Vec<Arm> {
    arena_arms!(RecordSet, checksum::checksum_recordset)
}

/// `records sparse (dropped)`: the `records` payload through the `bench_records_sparse` golden, the
/// bench schema under a drop profile that keeps 3 of each Record's 20 fields.
pub fn recordset_sparse() -> Vec<Arm> {
    use rustgen_golden::bench_records_sparse::arena::bench::RecordSet;
    arena_arms!(RecordSet, checksum::checksum_recordset_sparse)
}

pub fn particleset() -> Vec<Arm> {
    arena_arms!(ParticleSet, checksum::checksum_particleset)
}

// `checksum_dataset` over the streaming decoder, visiting every field the C++ arena bench's
// `streamgen` arm does (each Address and Attribute decoded whole). Every term is added, so the order
// the fields arrive in does not matter.
fn stream_dataset(b: &[u8]) -> u64 {
    use rustgen_golden::bench::stream::bench::*;
    let add = |s: &mut u64, v: u64| *s = s.wrapping_add(v);
    let mut sum = 0u64;
    Dataset::new(b)
        .unwrap()
        .decode(dataset_stream!(&mut sum; {
            name => |s, v| add(s, v.len() as u64),
            version => |s, v| add(s, v as u64),
            people => |s, p| p.decode(person_stream!(&mut *s; {
                id => |s, v| add(s, v as u64),
                name => |s, v| add(s, v.len() as u64),
                email => |s, v| add(s, v.len() as u64),
                active => |s, v| add(s, u64::from(v)),
                score => |s, v| add(s, v.to_bits()),
                created => |s, v| add(s, v),
                address => |s, ad| ad.decode(address_stream!(&mut *s; {
                    street => |s, v| add(s, v.len() as u64),
                    city => |s, v| add(s, v.len() as u64),
                    zip => |s, v| add(s, u64::from(v)),
                })),
                tags => |s, v| add(s, v.len() as u64),
                history => |s, v| add(s, u64::from(v as u32)),
                attributes => |s, at| at.decode(attribute_stream!(&mut *s; {
                    key => |s, v| add(s, v.len() as u64),
                    value => |s, v| add(s, v.len() as u64),
                })),
                counters => |s, (k, v)| {
                    add(s, k.len() as u64);
                    add(s, u64::from(v as u32));
                },
            })),
        }))
        .expect("rapidproto stream decode");
    sum
}
