//! The streaming decoder's scenarios: those of the C++ streaming bench (`rapidproto_bench`,
//! `tests/bench_streamgen.cpp`), under the same names, over the same bytes (`<dir>/stream/<stem>.bin`
//! and the varint sweep's pools, see [`payloads`]). Each has the C++ bench's two RapidProto arms:
//!
//! - `generated` (arm 0, the baseline): the generated streaming decoder driven by its `*_stream!`
//!   macros, each handler summing exactly what its C++ lambda does -- so on the same bytes the checksum
//!   is the C++ one too;
//! - `wire`: a hand-written loop over `rapidproto::wire`, feature-equivalent (dispatch on field number
//!   and wire type, validate, skip the rest; stop at the end or an error).
//!
//! Not mirrored, having no Rust counterpart: the C++ third arm, protozero, and the two `two-tier`
//! scenarios, which time one decode compiled in a tiny translation unit against the same decode in the
//! bench's large one (a C++ inlining effect).

use rapidproto::wire::{self, Tag, WireType};
use rapidproto::ErrorKind;

use crate::payloads::{self, Source};
use crate::runner::Scenario;
use crate::Arm;

/// Every streaming scenario, in the C++ bench's order and under its names.
pub fn scenarios() -> Vec<Scenario> {
    let single = |name: &str, stem: &'static str, arms: crate::ArmsFn| Scenario {
        name: name.to_string(),
        cpp_name: Some(name.to_string()),
        source: Source::Stream(stem),
        arms,
        mem: None,
    };
    let mut out = vec![
        single("varint-1byte", "varint-1byte", || arms(gen_i32, wire_i32)),
        single("varint-multibyte", "varint-multibyte", || {
            arms(gen_i32, wire_i32)
        }),
        single("zigzag-sint64", "zigzag-sint64", || arms(gen_s64, wire_s64)),
        single("fixed32-float", "fixed32-float", || arms(gen_fl, wire_fl)),
        single("fixed64-double", "fixed64-double", || arms(gen_db, wire_db)),
        single("len-string", "len-string", || arms(gen_s, wire_s)),
        single("packed-int32", "packed-int32", || {
            arms(gen_packed, wire_packed)
        }),
    ];
    for dist in payloads::varint_dists() {
        for count in payloads::VARINT_LENGTHS {
            let name = payloads::rv_scenario_name(&dist, count);
            out.push(Scenario {
                source: payloads::pool(&name),
                cpp_name: Some(name.clone()),
                name,
                arms: || arms(gen_rv, wire_rv),
                mem: None,
            });
        }
    }
    out.extend([
        single("skip-heavy", "skip-heavy", || arms(gen_i32, wire_i32)),
        single("mixed", "mixed", || arms(gen_mixed, wire_mixed)),
        single("multibyte-tag", "multibyte-tag", || {
            arms(gen_expanded, wire_expanded)
        }),
        single("nested-msg", "nested-msg", || arms(gen_nested, wire_nested)),
        single("groups", "groups", || arms(gen_groups, wire_groups)),
        single("sparse-skip", "sparse-skip", || arms(gen_i32, wire_i32)),
        single("records dense (stream)", "records", || {
            arms(gen_records_dense, wire_records_dense)
        }),
        single("records sparse (stream)", "records", || {
            arms(gen_records_sparse, wire_records_sparse)
        }),
    ]);
    out
}

// A scenario's two arms: the generated decoder (the baseline) and the hand-written loop. Function
// pointers, as the C++ streaming bench's arms are: a pass makes one indirect call per buffer in both.
fn arms(generated: fn(&[u8]) -> u64, wire: fn(&[u8]) -> u64) -> Vec<Arm> {
    vec![Arm::new("generated", generated), Arm::new("wire", wire)]
}

// --- generated: the streaming decoder ------------------------------------------------------------
// A decode error is ignored, as the C++ arms `(void)` theirs: the payloads are well-formed, and the
// agreement gate would catch a decode that stopped early.

fn gen_i32(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream! {
        i32 => |v| s = s.wrapping_add(u64::from(v as u32)),
    });
    s
}

fn gen_s64(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream! { s64 => |v| s = s.wrapping_add(v as u64) });
    s
}

fn gen_fl(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream! {
        fl => |v| s = s.wrapping_add(u64::from(v.to_bits())),
    });
    s
}

fn gen_db(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream! { db => |v| s = s.wrapping_add(v.to_bits()) });
    s
}

fn gen_s(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream! { s => |v| s = s.wrapping_add(v.len() as u64) });
    s
}

fn gen_packed(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream! {
        packed_nums => |v| s = s.wrapping_add(u64::from(v as u32)),
    });
    s
}

fn gen_expanded(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream! {
        expanded_nums => |v| s = s.wrapping_add(u64::from(v as u32)),
    });
    s
}

fn gen_rv(b: &[u8]) -> u64 {
    use rustgen_golden::bench::stream::bench::*;
    let mut s = 0u64;
    let _ = Big::new(b).decode(big_stream! { numbers => |v| s = s.wrapping_add(v as u64) });
    s
}

fn gen_mixed(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Scalars::new(b).decode(scalars_stream!(&mut s; {
        i32 => |s, v| *s = s.wrapping_add(u64::from(v as u32)),
        s64 => |s, v| *s = s.wrapping_add(v as u64),
        db => |s, v| *s = s.wrapping_add(v.to_bits()),
        s => |s, v| *s = s.wrapping_add(v.len() as u64),
    }));
    s
}

fn gen_nested(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::container::*;
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = Container::new(b).decode(container_stream!(&mut s; {
        items => |s, sub| {
            let _ = sub.decode(nested_stream!(&mut *s; {
                x => |s, v| *s = s.wrapping_add(u64::from(v as u32)),
            }));
        },
    }));
    s
}

fn gen_groups(b: &[u8]) -> u64 {
    use rustgen_golden::proto2::stream::p2::with_group::*;
    use rustgen_golden::proto2::stream::p2::*;
    let mut s = 0u64;
    let _ = WithGroup::new(b).decode(with_group_stream!(&mut s; {
        mygroup => |s, sub| {
            let _ = sub.decode(my_group_stream!(&mut *s; {
                a => |s, v| *s = s.wrapping_add(u64::from(v as u32)),
            }));
        },
    }));
    s
}

// Every Record field and both nested Samples (dense-materialize). An int32 / sint32 is sign-extended
// into the sum and a uint32 zero-extended, as the C++ lambdas' `static_cast<std::uint64_t>` do.
fn gen_records_dense(b: &[u8]) -> u64 {
    use rustgen_golden::bench::stream::bench::*;
    let mut s = 0u64;
    let _ = RecordSet::new(b).decode(record_set_stream!(&mut s; {
        records => |s, r| {
            let _ = r.decode(record_stream!(&mut *s; {
                f1 => |s, v| *s = s.wrapping_add(v as u64),
                f2 => |s, v| *s = s.wrapping_add(v as u64),
                f3 => |s, v| *s = s.wrapping_add(v as u64),
                f4 => |s, v| *s = s.wrapping_add(v as u64),
                f5 => |s, v| *s = s.wrapping_add(u64::from(v)),
                f6 => |s, v| *s = s.wrapping_add(v as u64),
                f7 => |s, v| *s = s.wrapping_add(u64::from(v)),
                f8 => |s, v| *s = s.wrapping_add(v),
                f9 => |s, v| *s = s.wrapping_add(v as u64),
                f10 => |s, v| *s = s.wrapping_add(v as u64),
                f11 => |s, v| *s = s.wrapping_add(v as u64),
                f12 => |s, v| *s = s.wrapping_add(v as u64),
                f13 => |s, v| *s = s.wrapping_add(v as u64),
                f14 => |s, v| *s = s.wrapping_add(v as u64),
                f15 => |s, v| *s = s.wrapping_add(v as u64),
                s1 => |s, sub| sample_dense(s, sub),
                s2 => |s, sub| sample_dense(s, sub),
                f18 => |s, v| *s = s.wrapping_add(v as u64),
                f19 => |s, v| *s = s.wrapping_add(v as u64),
                f20 => |s, v| *s = s.wrapping_add(v as u64),
            }));
        },
    }));
    s
}

fn sample_dense(s: &mut u64, sub: rustgen_golden::bench::stream::bench::Sample<'_>) {
    use rustgen_golden::bench::stream::bench::*;
    let _ = sub.decode(sample_stream!(&mut *s; {
        a => |s, v| *s = s.wrapping_add(v as u64),
        b => |s, v| *s = s.wrapping_add(v as u64),
        c => |s, v| *s = s.wrapping_add(v as u64),
        d => |s, v| *s = s.wrapping_add(v as u64),
        e => |s, v| *s = s.wrapping_add(u64::from(v)),
        f => |s, v| *s = s.wrapping_add(u64::from(v)),
    }));
}

// Only f1, f10 and f20 (sparse-extract): the decoder skips the other ~17 fields and never descends
// into s1/s2.
fn gen_records_sparse(b: &[u8]) -> u64 {
    use rustgen_golden::bench::stream::bench::*;
    let mut s = 0u64;
    let _ = RecordSet::new(b).decode(record_set_stream!(&mut s; {
        records => |s, r| {
            let _ = r.decode(record_stream!(&mut *s; {
                f1 => |s, v| *s = s.wrapping_add(v as u64),
                f10 => |s, v| *s = s.wrapping_add(v as u64),
                f20 => |s, v| *s = s.wrapping_add(v as u64),
            }));
        },
    }));
    s
}

// --- wire: the hand-written loops ----------------------------------------------------------------

// The hand loop every wire arm runs: read a tag; `handle` consumes the field's value and returns
// `true` when it is one the arm reads, `false` to skip it; stop at the end or on an error.
#[inline(always)]
fn fields<'a>(
    mut rest: &'a [u8],
    mut handle: impl FnMut(Tag, &mut &'a [u8]) -> Result<bool, ErrorKind>,
) {
    loop {
        let mut tag = Tag {
            field: 0,
            wire: WireType::Varint,
        };
        rest = match wire::read_tag_or_end(rest, &mut tag) {
            Ok(Some(r)) => r,
            Ok(None) | Err(_) => return,
        };
        match handle(tag, &mut rest) {
            Ok(true) => {}
            Ok(false) => match wire::skip(rest, tag) {
                Ok(r) => rest = r,
                Err(_) => return,
            },
            Err(_) => return,
        }
    }
}

#[inline(always)]
fn varint(rest: &mut &[u8]) -> Result<u64, ErrorKind> {
    let mut v = 0;
    *rest = wire::read_varint(rest, &mut v)?;
    Ok(v)
}

#[inline(always)]
fn len<'a>(rest: &mut &'a [u8]) -> Result<&'a [u8], ErrorKind> {
    let mut payload: &[u8] = &[];
    *rest = wire::read_length_delimited(rest, &mut payload)?;
    Ok(payload)
}

// Every varint of a packed run, until its end or a malformed one.
#[inline(always)]
fn packed(run: &[u8], mut each: impl FnMut(u64)) {
    let mut varints = wire::PackedVarints::new(run);
    while let Ok(Some(v)) = varints.read() {
        each(v);
    }
}

// One varint field `number`, summed through `conv`.
#[inline(always)]
fn wire_varint_field(b: &[u8], number: u32, conv: impl Fn(u64) -> u64) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == number && t.wire == WireType::Varint {
            s = s.wrapping_add(conv(varint(rest)?));
            return Ok(true);
        }
        Ok(false)
    });
    s
}

fn wire_i32(b: &[u8]) -> u64 {
    wire_varint_field(b, 1, |v| u64::from(v as u32))
}

fn wire_s64(b: &[u8]) -> u64 {
    wire_varint_field(b, 6, |v| wire::zigzag64(v) as u64)
}

fn wire_expanded(b: &[u8]) -> u64 {
    wire_varint_field(b, 18, |v| u64::from(v as u32))
}

fn wire_fl(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == 14 && t.wire == WireType::I32 {
            let mut v = 0u32;
            *rest = wire::read_fixed32(rest, &mut v)?;
            s = s.wrapping_add(u64::from(v));
            return Ok(true);
        }
        Ok(false)
    });
    s
}

fn wire_db(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == 15 && t.wire == WireType::I64 {
            let mut v = 0u64;
            *rest = wire::read_fixed64(rest, &mut v)?;
            s = s.wrapping_add(v);
            return Ok(true);
        }
        Ok(false)
    });
    s
}

fn wire_s(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == 12 && t.wire == WireType::Len {
            s = s.wrapping_add(len(rest)?.len() as u64);
            return Ok(true);
        }
        Ok(false)
    });
    s
}

// A packed varint field `number`, each element summed through `conv`.
#[inline(always)]
fn wire_packed_field(b: &[u8], number: u32, conv: impl Fn(u64) -> u64) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == number && t.wire == WireType::Len {
            packed(len(rest)?, |v| s = s.wrapping_add(conv(v)));
            return Ok(true);
        }
        Ok(false)
    });
    s
}

fn wire_packed(b: &[u8]) -> u64 {
    wire_packed_field(b, 17, |v| u64::from(v as u32))
}

fn wire_rv(b: &[u8]) -> u64 {
    wire_packed_field(b, 1, |v| v)
}

fn wire_mixed(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        let add = match (t.field, t.wire) {
            (1, WireType::Varint) => u64::from(varint(rest)? as u32),
            (6, WireType::Varint) => wire::zigzag64(varint(rest)?) as u64,
            (15, WireType::I64) => {
                let mut v = 0u64;
                *rest = wire::read_fixed64(rest, &mut v)?;
                v
            }
            (12, WireType::Len) => len(rest)?.len() as u64,
            _ => return Ok(false),
        };
        s = s.wrapping_add(add);
        Ok(true)
    });
    s
}

fn wire_nested(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == 5 && t.wire == WireType::Len {
            s = s.wrapping_add(wire_varint_field(len(rest)?, 1, |v| u64::from(v as u32)));
            return Ok(true);
        }
        Ok(false)
    });
    s
}

fn wire_groups(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == 1 && t.wire == WireType::SGroup {
            let mut body: &[u8] = &[];
            *rest = wire::read_group(rest, t.field, &mut body)?;
            s = s.wrapping_add(wire_varint_field(body, 2, |v| u64::from(v as u32)));
            return Ok(true);
        }
        Ok(false)
    });
    s
}

// A RecordSet's records, each summed by `record`.
#[inline(always)]
fn wire_records(b: &[u8], record: impl Fn(&[u8]) -> u64) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.field == 1 && t.wire == WireType::Len {
            s = s.wrapping_add(record(len(rest)?));
            return Ok(true);
        }
        Ok(false)
    });
    s
}

// Every Sample field, widened as the C++ `wire_sample_sum`.
fn wire_sample(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        if t.wire != WireType::Varint {
            return Ok(false);
        }
        let v = varint(rest)?;
        s = s.wrapping_add(match t.field {
            1 | 2 => v as i32 as u64,             // a, b int32
            3 => v,                               // c int64
            4 => wire::zigzag32(v as u32) as u64, // d sint32
            5 => u64::from(v != 0),               // e bool
            6 => u64::from(v as u32),             // f uint32
            _ => 0,
        });
        Ok(true)
    });
    s
}

// Every Record field and both Samples, widened as the C++ `wire_record_sum_dense`.
fn wire_record_dense(b: &[u8]) -> u64 {
    let mut s = 0u64;
    fields(b, |t, rest| {
        match (t.field, t.wire) {
            (field, WireType::Varint) => {
                let v = varint(rest)?;
                s = s.wrapping_add(match field {
                    6 => wire::zigzag32(v as u32) as u64, // f6 sint32
                    7 => u64::from(v != 0),               // f7 bool
                    5 => u64::from(v as u32),             // f5 uint32
                    8 | 4 => v,                           // f8 uint64, f4 int64
                    _ => v as i32 as u64,                 // the int32 fields
                });
            }
            (16 | 17, WireType::Len) => s = s.wrapping_add(wire_sample(len(rest)?)), // s1 / s2
            _ => return Ok(false),
        }
        Ok(true)
    });
    s
}

fn wire_records_dense(b: &[u8]) -> u64 {
    wire_records(b, wire_record_dense)
}

fn wire_records_sparse(b: &[u8]) -> u64 {
    wire_records(b, |rec| {
        let mut s = 0u64;
        fields(rec, |t, rest| {
            if matches!(t.field, 1 | 10 | 20) && t.wire == WireType::Varint {
                s = s.wrapping_add(varint(rest)? as i32 as u64);
                return Ok(true);
            }
            Ok(false)
        });
        s
    })
}
