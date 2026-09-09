// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The statistics both osmstat programs accumulate and print. The STATS go to stdout and are
// byte-identical between the two models for the same input (the fixture test holds them to it);
// TIMING goes to stderr, because the two models legitimately time differently.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace osmstat {

struct Stats {
    std::uint64_t blocks = 0;
    std::uint64_t dense_nodes = 0, plain_nodes = 0, ways = 0, relations = 0;
    std::uint64_t node_tags = 0, way_tags = 0, relation_tags = 0;
    std::uint64_t way_refs = 0, relation_members = 0;
    std::uint64_t info_rows = 0;   // per-entity metadata rows seen (Info + DenseInfo columns)
    std::uint64_t id_sum = 0;      // wrapping sum of every entity id -- a cheap correctness pin
    // Bounding box in nanodegrees, per-axis: a bbox needs no lat/lon pairing, which matters
    // to the streaming model (parallel packed arrays arrive one whole array after another).
    std::int64_t min_lat = 0, max_lat = 0, min_lon = 0, max_lon = 0;
    bool have_lat = false, have_lon = false;
    // Tag-key histogram. Per-block counting is index-wise (a vector, no strings); the merge
    // touches each DISTINCT key once per block, so this map is off the per-tag path -- on the
    // 20 MB Bremen extract it costs ~4 ms of a ~360 ms run.
    std::unordered_map<std::string, std::uint64_t> key_counts;

    void see_lat(std::int64_t nano) {
        min_lat = have_lat ? std::min(min_lat, nano) : nano;
        max_lat = have_lat ? std::max(max_lat, nano) : nano;
        have_lat = true;
    }

    void see_lon(std::int64_t nano) {
        min_lon = have_lon ? std::min(min_lon, nano) : nano;
        max_lon = have_lon ? std::max(max_lon, nano) : nano;
        have_lon = true;
    }

    // Fold one block's index-wise key counts into the histogram: `counts[i]` = uses of
    // stringtable entry i as a tag key, `string_at(i)` resolves the entry (copied only here).
    template <class StringAt>
    void merge_block_keys(const std::vector<std::uint64_t>& counts, StringAt&& string_at) {
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] == 0) continue;
            const std::string_view key = string_at(i);
            key_counts[std::string(key)] += counts[i];
        }
    }

    void print(std::FILE* out) const {
        std::fprintf(out, "blocks           %" PRIu64 "\n", blocks);
        std::fprintf(out, "nodes            %" PRIu64 " (dense %" PRIu64 ", plain %" PRIu64 ")\n",
                     dense_nodes + plain_nodes, dense_nodes, plain_nodes);
        std::fprintf(out, "ways             %" PRIu64 " (refs %" PRIu64 ")\n", ways, way_refs);
        std::fprintf(out, "relations        %" PRIu64 " (members %" PRIu64 ")\n", relations,
                     relation_members);
        std::fprintf(out, "tags             %" PRIu64 " (nodes %" PRIu64 ", ways %" PRIu64
                          ", relations %" PRIu64 ")\n",
                     node_tags + way_tags + relation_tags, node_tags, way_tags, relation_tags);
        std::fprintf(out, "metadata rows    %" PRIu64 "\n", info_rows);
        std::fprintf(out, "id sum           %016" PRIx64 "\n", id_sum);
        if (have_lat && have_lon) {
            std::fprintf(out, "bbox             lat [%.7f, %.7f]  lon [%.7f, %.7f]\n",
                         double(min_lat) / 1e9, double(max_lat) / 1e9, double(min_lon) / 1e9,
                         double(max_lon) / 1e9);
        }
        // Top tag keys, count-descending, key as tiebreak: deterministic across runs and models.
        std::vector<std::pair<std::string_view, std::uint64_t>> top(key_counts.begin(),
                                                                   key_counts.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        const std::size_t n = std::min<std::size_t>(10, top.size());
        std::fprintf(out, "distinct keys    %zu\n", top.size());
        for (std::size_t i = 0; i < n; ++i) {
            std::fprintf(out, "  key %-24s %" PRIu64 "\n", std::string(top[i].first).c_str(),
                         top[i].second);
        }
    }
};

inline double mib_per_s(std::uint64_t bytes, double seconds) {
    return seconds > 0 ? double(bytes) / (1024.0 * 1024.0) / seconds : 0.0;
}

// a + b with wraparound instead of signed overflow (hostile deltas are protoc-valid).
inline std::int64_t wrap_add(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

// offset + granularity * raw, in nanodegrees. Computed in uint64 so a hostile (still
// protoc-valid) value wraps instead of overflowing signed arithmetic: garbage in, garbage
// out, never UB.
inline std::int64_t coord_nano(std::int64_t offset, std::int64_t granularity, std::int64_t raw) {
    const std::uint64_t v = static_cast<std::uint64_t>(offset) +
                            static_cast<std::uint64_t>(granularity) * static_cast<std::uint64_t>(raw);
    return static_cast<std::int64_t>(v);
}

}  // namespace osmstat
