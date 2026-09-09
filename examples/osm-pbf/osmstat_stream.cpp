// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// osmstat, STREAMING model: decode an OpenStreetMap .osm.pbf file and print statistics about
// it. Nothing is materialized: each PrimitiveBlock is walked once, every field's value handed
// to a callback as it is decoded -- delta accumulators live in locals, and the only per-block
// allocation is the stringtable index (a vector of string_views borrowed from the inflated
// buffer). The sibling osmstat_arena.cpp computes the same statistics with the arena model;
// the two print identical stdout.
//
// One structural difference from the arena walk: tag keys reference the block's stringtable by
// index, and wire order does not guarantee the stringtable arrives before the groups that use
// it. A materializing decoder does not care; a single-pass one does. So each block is decoded
// TWICE -- a cheap first pass that collects only the stringtable (and the coordinate scaling
// fields), then the real walk with every other field skipped in pass one and the stringtable
// skipped in pass two. Skipping is what streaming decoders are good at; both passes together
// still allocate nothing beyond the stringtable index.
//
// Timing goes to stderr; decode and walk are ONE fused number here -- with no materialized
// tree there is nothing to re-walk.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "fileformat.rp.stream.hpp"  // OSMPBF::BlobHeader / OSMPBF::Blob (streaming)
#include "osm_stats.hpp"
#include "osmformat.rp.stream.hpp"  // OSMPBF::HeaderBlock / OSMPBF::PrimitiveBlock (streaming)
#include "pbf_input.hpp"
#include "rapidproto/runtime.hpp"

namespace pbf = rp::stream::OSMPBF;
using osmstat::Stats;

namespace {

bool feature_supported(std::string_view f) {
    return f == "OsmSchema-V0.6" || f == "DenseNodes" || f == "HistoricalInformation";
}

// Walk one PrimitiveBlock payload (both passes). Returns false on a malformed block.
bool walk_block(std::string_view payload, Stats& stats, std::vector<std::string_view>& strings,
                std::vector<std::uint64_t>& key_counts) {
    // Pass 1: the stringtable and the coordinate scaling fields, everything else skipped.
    // proto2 defaults are not delivered by a streaming decoder (an absent field fires no
    // callback), so the schema's defaults are the initial values here.
    strings.clear();
    std::int64_t granularity = 100, lat_offset = 0, lon_offset = 0;
    rapidproto::DecodeStatus st = pbf::PrimitiveBlock{payload}.decode(
        [&](pbf::PrimitiveBlock::stringtable, pbf::StringTable t) {
            return t.decode([&](pbf::StringTable::s, std::string_view s) { strings.push_back(s); });
        },
        [&](pbf::PrimitiveBlock::granularity, std::int32_t v) { granularity = v; },
        [&](pbf::PrimitiveBlock::lat_offset, std::int64_t v) { lat_offset = v; },
        [&](pbf::PrimitiveBlock::lon_offset, std::int64_t v) { lon_offset = v; });
    if (!st.ok()) {
        return false;
    }

    key_counts.assign(strings.size(), 0);
    const auto count_key = [&](std::uint64_t k) {
        if (k < key_counts.size()) {
            ++key_counts[static_cast<std::size_t>(k)];
        }
    };

    // Pass 2: the groups. Per-entity state lives in the enclosing lambdas' locals; every
    // packed array fires its callback once per element, in wire order.
    st = pbf::PrimitiveBlock{payload}.decode(
        [&](pbf::PrimitiveBlock::primitivegroup, pbf::PrimitiveGroup group) {
            std::int64_t dense_id = 0, dense_lat = 0, dense_lon = 0;
            std::int64_t pending_key = -1;  // keys_vals state: a key waiting for its value
            return group.decode(
                [&](pbf::PrimitiveGroup::dense, pbf::DenseNodes dense) {
                    return dense.decode(
                        [&](pbf::DenseNodes::id, std::int64_t d) {
                            dense_id += d;
                            stats.id_sum += static_cast<std::uint64_t>(dense_id);
                            ++stats.dense_nodes;
                        },
                        [&](pbf::DenseNodes::lat, std::int64_t d) {
                            dense_lat += d;
                            stats.see_lat(lat_offset + granularity * dense_lat);
                        },
                        [&](pbf::DenseNodes::lon, std::int64_t d) {
                            dense_lon += d;
                            stats.see_lon(lon_offset + granularity * dense_lon);
                        },
                        [&](pbf::DenseNodes::keys_vals, std::int32_t v) {
                            if (pending_key >= 0) {  // v is the value of the pending key
                                ++stats.node_tags;
                                count_key(static_cast<std::uint32_t>(pending_key));
                                pending_key = -1;
                            } else if (v != 0) {  // v is a key (0 is the node terminator)
                                pending_key = v;
                            }
                        },
                        [&](pbf::DenseNodes::denseinfo, pbf::DenseInfo info) {
                            return info.decode(
                                [&](pbf::DenseInfo::version, std::int32_t) { ++stats.info_rows; });
                        });
                },
                [&](pbf::PrimitiveGroup::nodes, pbf::Node node) {
                    return node.decode(
                        [&](pbf::Node::id, std::int64_t id) {
                            ++stats.plain_nodes;
                            stats.id_sum += static_cast<std::uint64_t>(id);
                        },
                        [&](pbf::Node::lat, std::int64_t lat) {
                            stats.see_lat(lat_offset + granularity * lat);
                        },
                        [&](pbf::Node::lon, std::int64_t lon) {
                            stats.see_lon(lon_offset + granularity * lon);
                        },
                        [&](pbf::Node::keys, std::uint32_t k) {
                            ++stats.node_tags;
                            count_key(k);
                        },
                        [&](pbf::Node::info, pbf::Info info) {
                            ++stats.info_rows;
                            return info.decode();
                        });
                },
                [&](pbf::PrimitiveGroup::ways, pbf::Way way) {
                    ++stats.ways;
                    return way.decode(
                        [&](pbf::Way::id, std::int64_t id) {
                            stats.id_sum += static_cast<std::uint64_t>(id);
                        },
                        [&](pbf::Way::refs, std::int64_t) { ++stats.way_refs; },
                        [&](pbf::Way::keys, std::uint32_t k) {
                            ++stats.way_tags;
                            count_key(k);
                        },
                        [&](pbf::Way::info, pbf::Info info) {
                            ++stats.info_rows;
                            return info.decode();
                        });
                },
                [&](pbf::PrimitiveGroup::relations, pbf::Relation rel) {
                    ++stats.relations;
                    return rel.decode(
                        [&](pbf::Relation::id, std::int64_t id) {
                            stats.id_sum += static_cast<std::uint64_t>(id);
                        },
                        [&](pbf::Relation::memids, std::int64_t) { ++stats.relation_members; },
                        [&](pbf::Relation::keys, std::uint32_t k) {
                            ++stats.relation_tags;
                            count_key(k);
                        },
                        [&](pbf::Relation::info, pbf::Info info) {
                            ++stats.info_rows;
                            return info.decode();
                        });
                });
        });
    if (!st.ok()) {
        return false;
    }

    stats.blocks += 1;
    stats.merge_block_keys(key_counts, [&](std::size_t i) { return strings[i]; });
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: osmstat-stream <file.osm.pbf>\n");
        return 2;
    }

    osmpbf_input::Stopwatch clock;
    const auto file = osmpbf_input::read_file(argv[1]);
    if (!file) {
        std::fprintf(stderr, "osmstat: cannot read %s\n", argv[1]);
        return 1;
    }
    const double t_read = clock.take();

    Stats stats;
    std::vector<std::string_view> strings;
    std::vector<std::uint64_t> key_counts;
    std::string inflated;  // reused across blocks
    double t_inflate = 0, t_walk = 0;
    std::uint64_t payload_bytes = 0;
    bool header_seen = false;
    bool failed = false;

    std::size_t offset = 0;
    osmpbf_input::Framed frame;
    while (!failed && osmpbf_input::next_frame(*file, offset, frame)) {
        clock.take();
        // BlobHeader: two fields wanted, the rest skipped.
        std::string_view blob_type;
        std::int32_t datasize = -1;
        rapidproto::DecodeStatus st = pbf::BlobHeader{frame.header_bytes}.decode(
            [&](pbf::BlobHeader::type, std::string_view v) { blob_type = v; },
            [&](pbf::BlobHeader::datasize, std::int32_t v) { datasize = v; });
        if (!st.ok() || !osmpbf_input::take_blob(*file, offset, datasize, frame)) {
            std::fprintf(stderr, "osmstat: bad BlobHeader at offset %zu\n", offset);
            return 1;
        }

        // Blob: the payload oneof's members are plain fields to a streaming decoder -- whichever
        // is on the wire fires. Unsupported compressions abort via `failed`.
        std::string_view payload;
        std::int64_t raw_size = 0;
        st = pbf::Blob{frame.blob_bytes}.decode(
            [&](pbf::Blob::raw_size, std::int32_t v) { raw_size = v; },
            [&](pbf::Blob::raw, std::string_view raw) { payload = raw; },
            [&](pbf::Blob::zlib_data, std::string_view deflated) {
                clock.take();
                if (raw_size < 0 || raw_size > 2 * 32 * 1024 * 1024 ||
                    !osmpbf_input::inflate_blob(deflated, static_cast<std::size_t>(raw_size),
                                                inflated)) {
                    failed = true;
                    return;
                }
                payload = inflated;
                t_inflate += clock.take();
            },
            [&](pbf::Blob::lzma_data, std::string_view) { failed = true; },
            [&](pbf::Blob::OBSOLETE_bzip2_data, std::string_view) { failed = true; },
            [&](pbf::Blob::lz4_data, std::string_view) { failed = true; },
            [&](pbf::Blob::zstd_data, std::string_view) { failed = true; });
        if (!st.ok() || failed) {
            std::fprintf(stderr, "osmstat: bad or unsupported Blob at offset %zu\n", offset);
            return 1;
        }
        payload_bytes += payload.size();

        if (blob_type == "OSMHeader") {
            st = pbf::HeaderBlock{payload}.decode(
                [&](pbf::HeaderBlock::required_features, std::string_view f) {
                    if (!feature_supported(f)) {
                        std::fprintf(stderr, "osmstat: file requires unsupported feature '%s'\n",
                                     std::string(f).c_str());
                        failed = true;
                    }
                });
            if (!st.ok() || failed) {
                return 1;
            }
            header_seen = true;
        } else if (blob_type == "OSMData") {
            if (!walk_block(payload, stats, strings, key_counts)) {
                std::fprintf(stderr, "osmstat: bad PrimitiveBlock at offset %zu\n", offset);
                return 1;
            }
        }
        t_walk += clock.take();
    }
    if (offset != file->size()) {
        return 1;  // next_frame already reported the framing error
    }
    if (!header_seen) {
        std::fprintf(stderr, "osmstat: no OSMHeader block -- not an .osm.pbf file?\n");
        return 1;
    }

    stats.print(stdout);
    std::fprintf(stderr, "model            stream\n");
    std::fprintf(stderr, "read             %.3fs (%.1f MiB/s, %zu bytes)\n", t_read,
                 osmstat::mib_per_s(file->size(), t_read), file->size());
    std::fprintf(stderr, "inflate          %.3fs (%.1f MiB/s of payload)\n", t_inflate,
                 osmstat::mib_per_s(payload_bytes, t_inflate));
    std::fprintf(stderr, "decode+walk      %.3fs (%.1f MiB/s of payload)\n", t_walk,
                 osmstat::mib_per_s(payload_bytes, t_walk));
    return 0;
}
