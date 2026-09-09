// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// osmstat, ARENA model: decode an OpenStreetMap .osm.pbf file and print statistics about it.
// Each PrimitiveBlock is materialized into an arena, then walked as plain contiguous arrays:
// DenseNodes' packed delta-coded columns decode straight into int64 arrays, and every
// stringtable entry is a string_view borrowed from the inflated buffer. The sibling
// osmstat_stream.cpp computes the same statistics with the streaming model; the two print
// identical stdout. Timing goes to stderr, decode and walk separately -- with a materialized
// tree they are separate steps.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fileformat.rp.hpp"  // OSMPBF::BlobHeader / OSMPBF::Blob (arena)
#include "osm_stats.hpp"
#include "osmformat.rp.hpp"  // OSMPBF::HeaderBlock / OSMPBF::PrimitiveBlock (arena)
#include "pbf_input.hpp"
#include "rapidproto/arena_runtime.hpp"

namespace pbf = rp::arena::OSMPBF;
using osmstat::Stats;

namespace {

bool unsupported(const char* scheme) {
    std::fprintf(stderr, "osmstat: unsupported blob compression '%s'\n", scheme);
    return true;
}

// Walk one materialized PrimitiveBlock. Every accessor below reads arena memory or borrowed
// views; nothing decodes -- decoding already happened in one decode() call per block.
void walk_block(const pbf::PrimitiveBlock* block, Stats& stats,
                std::vector<std::uint64_t>& key_counts) {
    // proto2 `[default=100]` fields have explicit presence, so the accessor is a std::optional
    // and the schema default is applied here, not by the decoder.
    const std::int64_t granularity = block->granularity().value_or(100);
    const std::int64_t lat_offset = block->lat_offset().value_or(0);
    const std::int64_t lon_offset = block->lon_offset().value_or(0);

    const rapidproto::StringArrayView strings = block->stringtable()->s();
    key_counts.assign(strings.size(), 0);
    // Tag keys index the stringtable; indexes come off the wire, so bounds-check them (an
    // out-of-range index still counts as a tag, it just names no key).
    const auto count_key = [&](std::uint64_t k) {
        if (k < key_counts.size()) {
            ++key_counts[static_cast<std::size_t>(k)];
        }
    };

    for (const pbf::PrimitiveGroup& group : block->primitivegroup()) {
        if (const pbf::DenseNodes* dense = group.dense()) {
            // Delta-coded columns, walked independently (per-axis aggregates need no
            // pairing) so both models agree even on ragged column lengths.
            std::uint64_t id = 0;
            for (const std::int64_t d : dense->id()) {
                id += static_cast<std::uint64_t>(d);
                stats.id_sum += id;
            }
            std::int64_t lat = 0, lon = 0;
            for (const std::int64_t d : dense->lat()) {
                lat = osmstat::wrap_add(lat, d);
                stats.see_lat(osmstat::coord_nano(lat_offset, granularity, lat));
            }
            for (const std::int64_t d : dense->lon()) {
                lon = osmstat::wrap_add(lon, d);
                stats.see_lon(osmstat::coord_nano(lon_offset, granularity, lon));
            }
            stats.dense_nodes += dense->id().size();
            // keys_vals: (key val)* pairs per node, 0-terminated. Tags only -- the node boundary
            // marker carries no data.
            const auto kv = dense->keys_vals();
            for (std::size_t i = 0; i + 1 < kv.size(); ++i) {
                if (kv[i] == 0) {
                    continue;  // node boundary
                }
                ++stats.node_tags;
                count_key(static_cast<std::uint32_t>(kv[i]));
                ++i;  // skip the value
            }
            if (const pbf::DenseInfo* info = dense->denseinfo()) {
                stats.info_rows += info->version().size();
            }
        }
        for (const pbf::Node& node : group.nodes()) {
            ++stats.plain_nodes;
            stats.id_sum += static_cast<std::uint64_t>(node.id());
            stats.see_lat(osmstat::coord_nano(lat_offset, granularity, node.lat()));
            stats.see_lon(osmstat::coord_nano(lon_offset, granularity, node.lon()));
            for (const std::uint32_t k : node.keys()) {
                ++stats.node_tags;
                count_key(k);
            }
            stats.info_rows += node.info() != nullptr;
        }
        for (const pbf::Way& way : group.ways()) {
            ++stats.ways;
            stats.id_sum += static_cast<std::uint64_t>(way.id());
            stats.way_refs += way.refs().size();
            for (const std::uint32_t k : way.keys()) {
                ++stats.way_tags;
                count_key(k);
            }
            stats.info_rows += way.info() != nullptr;
        }
        for (const pbf::Relation& rel : group.relations()) {
            ++stats.relations;
            stats.id_sum += static_cast<std::uint64_t>(rel.id());
            stats.relation_members += rel.memids().size();
            for (const std::uint32_t k : rel.keys()) {
                ++stats.relation_tags;
                count_key(k);
            }
            stats.info_rows += rel.info() != nullptr;
        }
    }
    stats.blocks += 1;
    stats.merge_block_keys(key_counts, [&](std::size_t i) { return strings[i]; });
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: osmstat-arena <file.osm.pbf>\n");
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
    std::vector<std::uint64_t> key_counts;
    std::string inflated;  // reused across blocks
    // One arena for the whole run, seeded so typical blocks never malloc; reset() rewinds it
    // per block and KEEPS any chunk a large block forced, so steady-state decoding allocates
    // nothing. Everything decoded from a blob -- tree and borrowed views -- dies at its reset.
    std::vector<char> scratch(8u * 1024 * 1024);
    rapidproto::Arena arena(scratch.data(), scratch.size());
    double t_inflate = 0, t_decode = 0, t_walk = 0;
    std::uint64_t payload_bytes = 0;
    bool header_seen = false;
    bool failed = false;

    std::size_t offset = 0;
    while (const auto header_bytes = osmpbf_input::next_frame(*file, offset)) {
        const std::size_t frame_offset = offset - header_bytes->size() - 4;
        arena.reset();
        rapidproto::ArenaDecodeError err{};

        clock.take();
        const pbf::BlobHeader* header =
            pbf::BlobHeader::decode(rapidproto::ByteView(*header_bytes), arena, &err);
        const auto blob_bytes = header != nullptr
                                    ? osmpbf_input::take_blob(*file, offset, header->datasize())
                                    : std::nullopt;
        if (!blob_bytes) {
            std::fprintf(stderr, "osmstat: bad BlobHeader at offset %zu\n", frame_offset);
            return 1;
        }
        const pbf::Blob* blob = pbf::Blob::decode(rapidproto::ByteView(*blob_bytes), arena, &err);
        if (blob == nullptr) {
            std::fprintf(stderr, "osmstat: bad Blob at offset %zu\n", frame_offset);
            return 1;
        }
        t_decode += clock.take();

        // The payload is a oneof: raw bytes or a compressed encoding. The visitor extracts
        // the two members this reader handles and refuses the rest; blob_payload() then does
        // the shared validate-and-inflate step.
        std::string_view raw, deflated;
        blob->data(
            [&](pbf::Blob::Data::raw, std::string_view v) { raw = v; },
            [&](pbf::Blob::Data::zlib_data, std::string_view v) { deflated = v; },
            [&](pbf::Blob::Data::lzma_data, std::string_view) { failed = unsupported("lzma"); },
            [&](pbf::Blob::Data::OBSOLETE_bzip2_data, std::string_view) {
                failed = unsupported("bzip2");
            },
            [&](pbf::Blob::Data::lz4_data, std::string_view) { failed = unsupported("lz4"); },
            [&](pbf::Blob::Data::zstd_data, std::string_view) { failed = unsupported("zstd"); },
            [&](std::monostate) {
                std::fprintf(stderr, "osmstat: Blob carries no data\n");
                failed = true;
            });
        if (failed) {
            return 1;
        }
        clock.take();
        const auto payload =
            osmpbf_input::blob_payload(raw, deflated, blob->raw_size().value_or(0), inflated);
        if (!payload) {
            return 1;
        }
        t_inflate += clock.take();
        payload_bytes += payload->size();

        if (header->type() == "OSMHeader") {
            clock.take();
            const pbf::HeaderBlock* hb =
                pbf::HeaderBlock::decode(rapidproto::ByteView(*payload), arena, &err);
            t_decode += clock.take();
            if (hb == nullptr) {
                std::fprintf(stderr, "osmstat: bad OSMHeader block\n");
                return 1;
            }
            for (const std::string_view feature : hb->required_features()) {
                if (!osmstat::feature_supported(feature)) {
                    std::fprintf(stderr, "osmstat: file requires unsupported feature '%s'\n",
                                 std::string(feature).c_str());
                    return 1;
                }
            }
            header_seen = true;
        } else if (header->type() == "OSMData") {
            clock.take();
            const pbf::PrimitiveBlock* block =
                pbf::PrimitiveBlock::decode(rapidproto::ByteView(*payload), arena, &err);
            t_decode += clock.take();
            if (block == nullptr) {
                std::fprintf(stderr, "osmstat: bad PrimitiveBlock (code %d at offset %zu)\n",
                             static_cast<int>(err.code), offset);
                return 1;
            }
            walk_block(block, stats, key_counts);
            t_walk += clock.take();
        }
        // Unknown blob types are reserved for future use; a reader skips them.
    }
    if (offset != file->size()) {
        return 1;  // next_frame already reported the framing error
    }
    if (!header_seen) {
        std::fprintf(stderr, "osmstat: no OSMHeader block -- not an .osm.pbf file?\n");
        return 1;
    }

    stats.print(stdout);
    std::fprintf(stderr, "model            arena\n");
    std::fprintf(stderr, "read             %.3fs (%.1f MiB/s, %zu bytes)\n", t_read,
                 osmstat::mib_per_s(file->size(), t_read), file->size());
    std::fprintf(stderr, "inflate          %.3fs (%.1f MiB/s of payload)\n", t_inflate,
                 osmstat::mib_per_s(payload_bytes, t_inflate));
    std::fprintf(stderr, "decode           %.3fs (%.1f MiB/s of payload)\n", t_decode,
                 osmstat::mib_per_s(payload_bytes, t_decode));
    std::fprintf(stderr, "walk             %.3fs (%.1f MiB/s of payload)\n", t_walk,
                 osmstat::mib_per_s(payload_bytes, t_walk));
    return 0;
}
