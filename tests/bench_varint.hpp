// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Shared builders for the repeated-varint decode sweep, used by BOTH benches (arena and streaming) so
// the two measure the SAME wire shape. The sweep characterises the packed-varint decode path across
// two axes:
//   - element byte width: fixed 1..10 bytes per element, uniform(1..10), and a 90%-1-byte / 10%-uniform
//     skew (the common real-world shape: mostly small ids with an occasional large value);
//   - element count: 10, 100, ... up to 1,000,000.
// The wire bytes are a single message carrying only a packed `repeated int64` field number 1 -- the
// bench schema's `Big.numbers` -- so the decode is dominated by the packed-varint fill and nothing else.
// Value generation is deterministic (a fixed-seed PRNG), so ins/B is reproducible run to run.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace rpbench {

// An int64 whose protobuf varint encoding is EXACTLY `width` bytes (1..10). Widths 1..9 are positive
// values in the band [2^(7*(width-1)), 2^(7*width)-1]; width 10 needs the 64th bit, i.e. a negative
// int64 (protobuf sign-extends it to the full 10-byte form). `salt` varies the value within the band
// without changing the byte count.
inline std::int64_t varint_of_width(int width, std::uint64_t salt) {
    if (width <= 1) {
        return static_cast<std::int64_t>(salt % 128U);  // 0..127 -> 1 byte
    }
    if (width >= 10) {
        return -1 - static_cast<std::int64_t>(salt % 128U);  // negative -> sign-extended 10 bytes
    }
    const std::uint64_t lo = std::uint64_t{1} << (7U * static_cast<unsigned>(width - 1));
    return static_cast<std::int64_t>(lo + (salt % lo));  // [lo, 2*lo-1] stays inside the width band
}

// One element-width distribution: `mode` is 1..10 for a fixed width, 0 for uniform(1..10), or -1 for
// the 90%-1-byte / 10%-uniform(1..10) skew. `label` is the compact scenario tag ("fx1".."fx10",
// "unif", "skew").
struct VarintDist {
    std::string label;
    int mode;
};

// The 12 distributions the sweep covers: fixed 1..10, uniform, and the 90/10 skew.
inline std::vector<VarintDist> varint_dists() {
    std::vector<VarintDist> out;
    out.reserve(12);
    for (int w = 1; w <= 10; ++w) {
        out.push_back({"fx" + std::to_string(w), w});
    }
    out.push_back({"unif", 0});
    out.push_back({"skew", -1});
    return out;
}

// The element-count axis: 10, 100, ... 1,000,000.
inline std::vector<int> varint_lengths() {
    return {10, 100, 1'000, 10'000, 100'000, 1'000'000};
}

// A compact tag for a count, for scenario names: 10 -> "10", 1000 -> "1k", 1000000 -> "1M".
inline std::string length_tag(int count) {
    if (count >= 1'000'000 && count % 1'000'000 == 0) {
        return std::to_string(count / 1'000'000) + "M";
    }
    if (count >= 1'000 && count % 1'000 == 0) {
        return std::to_string(count / 1'000) + "k";
    }
    return std::to_string(count);
}

// `count` int64 element values under `dist`, deterministic (seeded from the mode, so a given
// distribution always yields the same sequence; a longer count extends the same prefix).
inline std::vector<std::int64_t> varint_values(const VarintDist& dist, int count) {
    std::vector<std::int64_t> out;
    out.reserve(static_cast<std::size_t>(count));
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ static_cast<std::uint64_t>(dist.mode + 32));
    std::uniform_int_distribution<int> any_width(1, 10);
    std::uniform_int_distribution<int> tenth(0, 9);
    for (int i = 0; i < count; ++i) {
        int width = 0;
        if (dist.mode > 0) {
            width = dist.mode;  // fixed
        } else if (dist.mode == 0) {
            width = any_width(rng);  // uniform 1..10
        } else {
            width = (tenth(rng) == 0) ? any_width(rng) : 1;  // 90% 1-byte, 10% uniform 1..10
        }
        out.push_back(varint_of_width(width, rng()));
    }
    return out;
}

// The wire bytes of a message carrying only `numbers` (packed repeated int64, field 1) with these
// values -- exactly what bench.Big / rp::bench::Big serialises, so protoc, the arena decoder, the
// streaming decoder, and protozero all decode the identical buffer.
inline std::string make_packed_i64(const std::vector<std::int64_t>& values) {
    const auto put_varint = [](std::string& b, std::uint64_t v) {
        while (v >= 0x80U) {
            b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
            v >>= 7U;
        }
        b.push_back(static_cast<char>(v));
    };
    std::string payload;
    for (const std::int64_t v : values) {
        put_varint(payload, static_cast<std::uint64_t>(v));
    }
    std::string buf;
    put_varint(buf, (std::uint64_t{1} << 3U) | 2U);  // tag: field 1, wire type 2 (LEN)
    put_varint(buf, payload.size());
    buf += payload;
    return buf;
}

}  // namespace rpbench
