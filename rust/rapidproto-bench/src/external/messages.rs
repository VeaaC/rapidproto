//! `google_message1` / `google_message2`: the C++ arena bench's scenarios of the same names
//! (`tests/bench_arm_messages.cpp`), over the payload its dump unwraps from each dataset file. Arms:
//! cold (the baseline), warm and stream, as in C++.
//!
//! The checksum is the C++ one: present fields only, every field of the schema walked (proto2 defaults
//! are non-zero, and a wire-driven streaming walk cannot see an absent field's default), with the C++
//! widenings -- int32 zero-extends through `uint32_t`, int64 sign-extends, a float contributes the bits
//! of its value widened to double, a string its length.

#![allow(non_upper_case_globals)] // the stream macros' generated `WANT_<field>` consts

#[allow(
    clippy::all,
    unused,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod gm1 {
    include!(concat!(env!("OUT_DIR"), "/benchmark_message1_proto2.rs"));
}

#[allow(
    clippy::all,
    unused,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod gm2 {
    include!(concat!(env!("OUT_DIR"), "/benchmark_message2.rs"));
}

use rapidproto::ProtoStr;

use crate::Arm;

fn add(s: &mut u64, v: u64) {
    *s = s.wrapping_add(v);
}
fn len(v: &ProtoStr) -> u64 {
    v.len() as u64
}
fn blen(v: &[u8]) -> u64 {
    v.len() as u64
}
// `static_cast<std::uint32_t>` of an int32: zero-extends.
fn i32w(v: i32) -> u64 {
    u64::from(v as u32)
}
// `static_cast<std::uint64_t>` of an int64: sign-extends.
fn i64w(v: i64) -> u64 {
    v as u64
}
fn fbits(v: f32) -> u64 {
    f64::from(v).to_bits()
}

// --- google_message1 ------------------------------------------------------------------------------

pub fn gm1_arms() -> Vec<Arm> {
    let mut arms = arena_arms!(gm1::arena::benchmarks::proto2::GoogleMessage1, gm1_walk);
    arms.push(Arm::new("rapidproto-stream", gm1_stream));
    arms
}

fn gm1_walk(m: &gm1::arena::benchmarks::proto2::GoogleMessage1) -> u64 {
    let mut s = 0u64;
    let s = &mut s;
    add(s, len(m.field1()));
    add(s, m.field9().map_or(0, len));
    add(s, m.field18().map_or(0, len));
    add(s, m.field80().map_or(0, u64::from));
    add(s, m.field81().map_or(0, u64::from));
    add(s, i32w(m.field2()));
    add(s, i32w(m.field3()));
    add(s, m.field280().map_or(0, i32w));
    add(s, m.field6().map_or(0, i32w));
    add(s, m.field22().map_or(0, i64w));
    add(s, m.field4().map_or(0, len));
    for &v in m.field5() {
        add(s, v);
    }
    add(s, m.field59().map_or(0, u64::from));
    add(s, m.field7().map_or(0, len));
    add(s, m.field16().map_or(0, i32w));
    add(s, m.field130().map_or(0, i32w));
    add(s, m.field12().map_or(0, u64::from));
    add(s, m.field17().map_or(0, u64::from));
    add(s, m.field13().map_or(0, u64::from));
    add(s, m.field14().map_or(0, u64::from));
    add(s, m.field104().map_or(0, i32w));
    add(s, m.field100().map_or(0, i32w));
    add(s, m.field101().map_or(0, i32w));
    add(s, m.field102().map_or(0, len));
    add(s, m.field103().map_or(0, len));
    add(s, m.field29().map_or(0, i32w));
    add(s, m.field30().map_or(0, u64::from));
    add(s, m.field60().map_or(0, i32w));
    add(s, m.field271().map_or(0, i32w));
    add(s, m.field272().map_or(0, i32w));
    add(s, m.field150().map_or(0, i32w));
    add(s, m.field23().map_or(0, i32w));
    add(s, m.field24().map_or(0, u64::from));
    add(s, m.field25().map_or(0, i32w));
    if let Some(sub) = m.field15() {
        add(s, sub.field1().map_or(0, i32w));
        add(s, sub.field2().map_or(0, i32w));
        add(s, sub.field3().map_or(0, i32w));
        add(s, sub.field15().map_or(0, len));
        add(s, sub.field12().map_or(0, u64::from));
        add(s, sub.field13().map_or(0, i64w));
        add(s, sub.field14().map_or(0, i64w));
        add(s, sub.field16().map_or(0, i32w));
        add(s, sub.field19().map_or(0, i32w));
        add(s, sub.field20().map_or(0, u64::from));
        add(s, sub.field28().map_or(0, u64::from));
        add(s, sub.field21().unwrap_or(0));
        add(s, sub.field22().map_or(0, i32w));
        add(s, sub.field23().map_or(0, u64::from));
        add(s, sub.field206().map_or(0, u64::from));
        add(s, sub.field203().map_or(0, u64::from));
        add(s, sub.field204().map_or(0, i32w));
        add(s, sub.field205().map_or(0, len));
        add(s, sub.field207().unwrap_or(0));
        add(s, sub.field300().unwrap_or(0));
    }
    add(s, m.field78().map_or(0, u64::from));
    add(s, m.field67().map_or(0, i32w));
    add(s, m.field68().map_or(0, i32w));
    add(s, m.field128().map_or(0, i32w));
    add(s, m.field129().map_or(0, len));
    add(s, m.field131().map_or(0, i32w));
    *s
}

fn gm1_stream(b: &[u8]) -> u64 {
    use gm1::stream::benchmarks::proto2::*;
    let mut s = 0u64;
    GoogleMessage1::new(b)
        .decode(google_message1_stream!(&mut s; {
            field1 => |s, v| add(s, len(v)),
            field9 => |s, v| add(s, len(v)),
            field18 => |s, v| add(s, len(v)),
            field80 => |s, v| add(s, u64::from(v)),
            field81 => |s, v| add(s, u64::from(v)),
            field2 => |s, v| add(s, i32w(v)),
            field3 => |s, v| add(s, i32w(v)),
            field280 => |s, v| add(s, i32w(v)),
            field6 => |s, v| add(s, i32w(v)),
            field22 => |s, v| add(s, i64w(v)),
            field4 => |s, v| add(s, len(v)),
            field5 => add,
            field59 => |s, v| add(s, u64::from(v)),
            field7 => |s, v| add(s, len(v)),
            field16 => |s, v| add(s, i32w(v)),
            field130 => |s, v| add(s, i32w(v)),
            field12 => |s, v| add(s, u64::from(v)),
            field17 => |s, v| add(s, u64::from(v)),
            field13 => |s, v| add(s, u64::from(v)),
            field14 => |s, v| add(s, u64::from(v)),
            field104 => |s, v| add(s, i32w(v)),
            field100 => |s, v| add(s, i32w(v)),
            field101 => |s, v| add(s, i32w(v)),
            field102 => |s, v| add(s, len(v)),
            field103 => |s, v| add(s, len(v)),
            field29 => |s, v| add(s, i32w(v)),
            field30 => |s, v| add(s, u64::from(v)),
            field60 => |s, v| add(s, i32w(v)),
            field271 => |s, v| add(s, i32w(v)),
            field272 => |s, v| add(s, i32w(v)),
            field150 => |s, v| add(s, i32w(v)),
            field23 => |s, v| add(s, i32w(v)),
            field24 => |s, v| add(s, u64::from(v)),
            field25 => |s, v| add(s, i32w(v)),
            field15 => |s, sub| sub.decode(google_message1_sub_message_stream!(&mut *s; {
                field1 => |s, v| add(s, i32w(v)),
                field2 => |s, v| add(s, i32w(v)),
                field3 => |s, v| add(s, i32w(v)),
                field15 => |s, v| add(s, len(v)),
                field12 => |s, v| add(s, u64::from(v)),
                field13 => |s, v| add(s, i64w(v)),
                field14 => |s, v| add(s, i64w(v)),
                field16 => |s, v| add(s, i32w(v)),
                field19 => |s, v| add(s, i32w(v)),
                field20 => |s, v| add(s, u64::from(v)),
                field28 => |s, v| add(s, u64::from(v)),
                field21 => add,
                field22 => |s, v| add(s, i32w(v)),
                field23 => |s, v| add(s, u64::from(v)),
                field206 => |s, v| add(s, u64::from(v)),
                field203 => |s, v| add(s, u64::from(v)),
                field204 => |s, v| add(s, i32w(v)),
                field205 => |s, v| add(s, len(v)),
                field207 => add,
                field300 => add,
            })),
            field78 => |s, v| add(s, u64::from(v)),
            field67 => |s, v| add(s, i32w(v)),
            field68 => |s, v| add(s, i32w(v)),
            field128 => |s, v| add(s, i32w(v)),
            field129 => |s, v| add(s, len(v)),
            field131 => |s, v| add(s, i32w(v)),
        }))
        .expect("rapidproto stream decode");
    s
}

// --- google_message2 ------------------------------------------------------------------------------

pub fn gm2_arms() -> Vec<Arm> {
    let mut arms = arena_arms!(gm2::arena::benchmarks::proto2::GoogleMessage2, gm2_walk);
    arms.push(Arm::new("rapidproto-stream", gm2_stream));
    arms
}

fn gm2_walk(m: &gm2::arena::benchmarks::proto2::GoogleMessage2) -> u64 {
    let mut s = 0u64;
    let s = &mut s;
    add(s, m.field1().map_or(0, len));
    add(s, m.field3().map_or(0, i64w));
    add(s, m.field4().map_or(0, i64w));
    add(s, m.field30().map_or(0, i64w));
    add(s, m.field75().map_or(0, u64::from));
    add(s, m.field6().map_or(0, len));
    add(s, m.field2().map_or(0, blen));
    add(s, m.field21().map_or(0, i32w));
    add(s, m.field71().map_or(0, i32w));
    add(s, m.field25().map_or(0, fbits));
    add(s, m.field109().map_or(0, i32w));
    add(s, m.field210().map_or(0, i32w));
    add(s, m.field211().map_or(0, i32w));
    add(s, m.field212().map_or(0, i32w));
    add(s, m.field213().map_or(0, i32w));
    add(s, m.field216().map_or(0, i32w));
    add(s, m.field217().map_or(0, i32w));
    add(s, m.field218().map_or(0, i32w));
    add(s, m.field220().map_or(0, i32w));
    add(s, m.field221().map_or(0, i32w));
    add(s, m.field222().map_or(0, fbits));
    add(s, m.field63().map_or(0, i32w));
    for g in m.group1() {
        add(s, fbits(g.field11()));
        add(s, g.field26().map_or(0, fbits));
        add(s, g.field12().map_or(0, len));
        add(s, g.field13().map_or(0, len));
        for v in g.field14() {
            add(s, len(v));
        }
        add(s, g.field15());
        add(s, g.field5().map_or(0, i32w));
        add(s, g.field27().map_or(0, len));
        add(s, g.field28().map_or(0, i32w));
        add(s, g.field29().map_or(0, len));
        add(s, g.field16().map_or(0, len));
        for v in g.field22() {
            add(s, len(v));
        }
        for &v in g.field73() {
            add(s, i32w(v));
        }
        add(s, g.field20().map_or(0, i32w));
        add(s, g.field24().map_or(0, len));
        if let Some(gm) = g.field31() {
            add(s, gm.field1().map_or(0, fbits));
            add(s, gm.field2().map_or(0, fbits));
            add(s, gm.field3().map_or(0, fbits));
            add(s, gm.field4().map_or(0, u64::from));
            add(s, gm.field5().map_or(0, u64::from));
            add(s, gm.field6().map_or(0, u64::from));
            add(s, gm.field7().map_or(0, u64::from));
            add(s, gm.field8().map_or(0, fbits));
            add(s, gm.field9().map_or(0, u64::from));
            add(s, gm.field10().map_or(0, fbits));
            add(s, gm.field11().map_or(0, i64w));
        }
    }
    for v in m.field128() {
        add(s, len(v));
    }
    add(s, m.field131().map_or(0, i64w));
    for v in m.field127() {
        add(s, len(v));
    }
    add(s, m.field129().map_or(0, i32w));
    for &v in m.field130() {
        add(s, i64w(v));
    }
    add(s, m.field205().map_or(0, u64::from));
    add(s, m.field206().map_or(0, u64::from));
    *s
}

fn gm2_stream(b: &[u8]) -> u64 {
    use gm2::stream::benchmarks::proto2::google_message2::*;
    use gm2::stream::benchmarks::proto2::*;
    let mut s = 0u64;
    GoogleMessage2::new(b)
        .decode(google_message2_stream!(&mut s; {
            field1 => |s, v| add(s, len(v)),
            field3 => |s, v| add(s, i64w(v)),
            field4 => |s, v| add(s, i64w(v)),
            field30 => |s, v| add(s, i64w(v)),
            field75 => |s, v| add(s, u64::from(v)),
            field6 => |s, v| add(s, len(v)),
            field2 => |s, v| add(s, blen(v)),
            field21 => |s, v| add(s, i32w(v)),
            field71 => |s, v| add(s, i32w(v)),
            field25 => |s, v| add(s, fbits(v)),
            field109 => |s, v| add(s, i32w(v)),
            field210 => |s, v| add(s, i32w(v)),
            field211 => |s, v| add(s, i32w(v)),
            field212 => |s, v| add(s, i32w(v)),
            field213 => |s, v| add(s, i32w(v)),
            field216 => |s, v| add(s, i32w(v)),
            field217 => |s, v| add(s, i32w(v)),
            field218 => |s, v| add(s, i32w(v)),
            field220 => |s, v| add(s, i32w(v)),
            field221 => |s, v| add(s, i32w(v)),
            field222 => |s, v| add(s, fbits(v)),
            field63 => |s, v| add(s, i32w(v)),
            group1 => |s, g| g.decode(google_message2::group1_stream!(&mut *s; {
                field11 => |s, v| add(s, fbits(v)),
                field26 => |s, v| add(s, fbits(v)),
                field12 => |s, v| add(s, len(v)),
                field13 => |s, v| add(s, len(v)),
                field14 => |s, v| add(s, len(v)),
                field15 => add,
                field5 => |s, v| add(s, i32w(v)),
                field27 => |s, v| add(s, len(v)),
                field28 => |s, v| add(s, i32w(v)),
                field29 => |s, v| add(s, len(v)),
                field16 => |s, v| add(s, len(v)),
                field22 => |s, v| add(s, len(v)),
                field73 => |s, v| add(s, i32w(v)),
                field20 => |s, v| add(s, i32w(v)),
                field24 => |s, v| add(s, len(v)),
                field31 => |s, gm| gm.decode(google_message2_grouped_message_stream!(&mut *s; {
                    field1 => |s, v| add(s, fbits(v)),
                    field2 => |s, v| add(s, fbits(v)),
                    field3 => |s, v| add(s, fbits(v)),
                    field4 => |s, v| add(s, u64::from(v)),
                    field5 => |s, v| add(s, u64::from(v)),
                    field6 => |s, v| add(s, u64::from(v)),
                    field7 => |s, v| add(s, u64::from(v)),
                    field8 => |s, v| add(s, fbits(v)),
                    field9 => |s, v| add(s, u64::from(v)),
                    field10 => |s, v| add(s, fbits(v)),
                    field11 => |s, v| add(s, i64w(v)),
                })),
            })),
            field128 => |s, v| add(s, len(v)),
            field131 => |s, v| add(s, i64w(v)),
            field127 => |s, v| add(s, len(v)),
            field129 => |s, v| add(s, i32w(v)),
            field130 => |s, v| add(s, i64w(v)),
            field205 => |s, v| add(s, u64::from(v)),
            field206 => |s, v| add(s, u64::from(v)),
        }))
        .expect("rapidproto stream decode");
    s
}
