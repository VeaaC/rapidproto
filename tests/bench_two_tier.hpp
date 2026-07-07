// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Two-tier benchmark support: the SAME generated decode, compiled in two translation-unit contexts.
//
//   MICRO tier  -- the decode alone in a small TU (bench_stream_isolated.cpp): the compiler inlines
//                  the hot wire primitives (read_varint etc.), so this is the code's CEILING.
//   LARGE tier  -- the decode amid the whole streaming bench TU (many decoders / instantiations): the
//                  inliner's cost model keeps the primitives out-of-line and calls them per element,
//                  the REALISTIC number a big real program sees.
//
// The gap between the two tiers is the out-of-line-primitive penalty -- invisible in a micro-only
// benchmark. The decode body is defined once here (a macro) and instantiated under two distinct
// function names, one per TU, so the two tiers are byte-identical source and differ ONLY in context.

#include <cstdint>
#include <string>
#include <string_view>

#include "proto2.rp.stream.hpp"  // generated p2::stream::Scalars (comprehensive: 18 field kinds)
#include "rapidproto/runtime.hpp"

// A comprehensive, varint-heavy decode: a mix of varint scalars + a large packed varint array (where
// the out-of-line read_varint penalty bites) + a string. `fn` is the generated function name.
#define RP_BENCH_DEFINE_SCALARS_DECODE(fn)                                           \
    std::uint64_t fn(::rapidproto::ByteView bytes) {                                 \
        std::uint64_t sum = 0;                                                       \
        (void)::p2::stream::Scalars{bytes}.decode(                                   \
            [&](::p2::stream::Scalars::i32, std::int32_t v) {                        \
                sum += static_cast<std::uint32_t>(v);                                \
            },                                                                       \
            [&](::p2::stream::Scalars::u64, std::uint64_t v) { sum += v; },          \
            [&](::p2::stream::Scalars::s32, std::int32_t v) {                        \
                sum += static_cast<std::uint32_t>(v);                                \
            },                                                                       \
            [&](::p2::stream::Scalars::packed_nums, std::int32_t v) {                \
                sum += static_cast<std::uint32_t>(v);                                \
            },                                                                       \
            [&](::p2::stream::Scalars::s, std::string_view v) { sum += v.size(); }); \
        return sum;                                                                  \
    }

// A multi-byte-tag-heavy decode: every record is field 18 (tag 0x90 0x01, a 2-byte tag) + a 1-byte
// varint. This is the worst case for the tag path -- it exercises read_tag_or_end's multi-byte
// fallback (read_tag) on every record. Used to isolate, in one binary, whether inlining read_varint
// helps or HURTS the tag path (micro=inlined vs large=out-of-line).
#define RP_BENCH_DEFINE_MBTAG_DECODE(fn)                                \
    std::uint64_t fn(::rapidproto::ByteView bytes) {                    \
        std::uint64_t sum = 0;                                          \
        (void)::p2::stream::Scalars{bytes}.decode(                      \
            [&](::p2::stream::Scalars::expanded_nums, std::int32_t v) { \
                sum += static_cast<std::uint32_t>(v);                   \
            });                                                         \
        return sum;                                                     \
    }

// Build a buffer matching the decode above: a few varint scalars, a large packed_nums array, a string.
inline std::string rp_bench_scalars_buffer(int packed_count) {
    const auto put_varint = [](std::string& b, std::uint64_t v) {
        while (v >= 0x80U) {
            b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
            v >>= 7U;
        }
        b.push_back(static_cast<char>(v));
    };
    const auto put_tag = [&](std::string& b, std::uint32_t field, std::uint32_t wire) {
        put_varint(b, (static_cast<std::uint64_t>(field) << 3U) | wire);
    };
    std::string buf;
    put_tag(buf, 1, 0);
    put_varint(buf, 1234567);  // i32 (field 1)
    put_tag(buf, 4, 0);
    put_varint(buf, 0xDEADBEEFULL);  // u64 (field 4)
    put_tag(buf, 5, 0);
    put_varint(buf, 84);  // s32 (field 5): zigzag(42)
    std::string packed;
    for (int i = 0; i < packed_count; ++i) {
        put_varint(packed, static_cast<std::uint64_t>(i) & 0x3FFFFU);  // 2-3 byte varints
    }
    put_tag(buf, 17, 2);  // packed_nums (field 17, LEN)
    put_varint(buf, packed.size());
    buf += packed;
    put_tag(buf, 12, 2);  // s (field 12, LEN string)
    const std::string str = "the quick brown fox jumps over the lazy dog";
    put_varint(buf, str.size());
    buf += str;
    return buf;
}

// Build a buffer for RP_BENCH_DEFINE_MBTAG_DECODE: `count` records of {2-byte tag, 1-byte varint}.
inline std::string rp_bench_mbtag_buffer(int count) {
    const auto put_varint = [](std::string& b, std::uint64_t v) {
        while (v >= 0x80U) {
            b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
            v >>= 7U;
        }
        b.push_back(static_cast<char>(v));
    };
    std::string buf;
    for (int i = 0; i < count; ++i) {
        put_varint(buf,
                   (static_cast<std::uint64_t>(18) << 3U) | 0U);  // field 18, Varint: 0x90 0x01
        put_varint(buf, static_cast<std::uint64_t>(i) & 0x7FU);
    }
    return buf;
}
