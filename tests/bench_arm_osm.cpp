// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The OSM PBF scenarios' own translation unit (see bench_arm_osm.hpp for the scenario design,
// the checksum convention, and the threading note). Compiled only under RAPIDPROTO_BENCH_OSM,
// which the build defines when the corpus (libosmium + protozero pins), the pinned dataset,
// zlib, and protobuf are all present.

#ifdef RAPIDPROTO_BENCH_OSM

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "bench_arm_osm.hpp"
#include "bench_harness.hpp"
#include "fileformat.rp.hpp"
#include "fileformat.rp.stream.hpp"
#include "osmformat.pb.h"  // protoc arm (::OSMPBF)
#include "osmformat.rp.hpp"
#include "osmformat.rp.stream.hpp"
#include "pbf_input.hpp"  // the example's framing/inflate helpers (examples/osm-pbf)
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

#include <osmium/handler.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/osm/way.hpp>
#include <osmium/visitor.hpp>

namespace osm_a = rp::arena::OSMPBF;
namespace osm_s = rp::stream::OSMPBF;

namespace rposm {
namespace {

// ── the checksum, one spec for every arm ──────────────────────────────────────────────────────
// Wrapping uint64 sums; see the header. `u()` sign-extends so negative ids/uids fold the same
// bit pattern everywhere.
inline std::uint64_t u(std::int64_t v) {
    return static_cast<std::uint64_t>(v);
}

constexpr std::uint64_t kNode = 3, kWay = 5, kRelation = 7;

// osmium's coordinate/timestamp conversions, replicated exactly (integer division semantics):
// location = (raw*granularity + offset)/100 as int32; timestamp = raw*date_granularity/1000.
inline std::uint64_t loc(std::int64_t raw, std::int64_t granularity, std::int64_t offset) {
    return u(static_cast<std::int32_t>((raw * granularity + offset) / 100));
}

inline std::uint64_t first_byte(std::string_view s) {
    return s.empty() ? 0 : static_cast<unsigned char>(s[0]);
}

// ── arena walk ────────────────────────────────────────────────────────────────────────────────

std::uint64_t arena_block_sum(const osm_a::PrimitiveBlock* b) {
    const std::int64_t gran = b->granularity().value_or(100);
    const std::int64_t lat_off = b->lat_offset().value_or(0);
    const std::int64_t lon_off = b->lon_offset().value_or(0);
    const std::int64_t date_gran = b->date_granularity().value_or(1000);
    const rapidproto::StringArrayView st = b->stringtable()->s();
    const auto sv = [&](std::uint64_t i) {
        return i < st.size() ? st[static_cast<std::size_t>(i)] : std::string_view{};
    };
    const auto info_sum = [&](const osm_a::Info* info) {
        if (info == nullptr) {
            return std::uint64_t{0};
        }
        return u(info->version().value_or(0)) +
               u(info->timestamp().value_or(0) * date_gran / 1000) +
               u(info->changeset().value_or(0)) + u(info->uid().value_or(0)) +
               first_byte(sv(info->user_sid().value_or(0)));
    };

    std::uint64_t h = 0;
    for (const osm_a::PrimitiveGroup& g : b->primitivegroup()) {
        if (const osm_a::DenseNodes* d = g.dense()) {
            const auto ids = d->id();
            const auto lats = d->lat();
            const auto lons = d->lon();
            const std::size_t n = std::min({ids.size(), lats.size(), lons.size()});
            std::int64_t id = 0, lat = 0, lon = 0;
            for (std::size_t i = 0; i < n; ++i) {
                id += ids[i];
                lat += lats[i];
                lon += lons[i];
                h += kNode * u(id) + loc(lat, gran, lat_off) + loc(lon, gran, lon_off);
            }
            if (const osm_a::DenseInfo* di = d->denseinfo()) {
                const auto vers = di->version();
                const auto ts = di->timestamp();
                const auto cs = di->changeset();
                const auto uid = di->uid();
                const auto usid = di->user_sid();
                std::int64_t t = 0, c = 0, ui = 0, us = 0;
                for (std::size_t i = 0; i < vers.size(); ++i) {
                    h += u(vers[i]);
                }
                for (std::size_t i = 0; i < ts.size(); ++i) {
                    t += ts[i];
                    h += u(t * date_gran / 1000);
                }
                for (std::size_t i = 0; i < cs.size(); ++i) {
                    c += cs[i];
                    h += u(c);
                }
                for (std::size_t i = 0; i < uid.size(); ++i) {
                    ui += uid[i];
                    h += u(ui);
                }
                for (std::size_t i = 0; i < usid.size(); ++i) {
                    us += usid[i];
                    h += first_byte(sv(u(us)));
                }
            }
            const auto kv = d->keys_vals();
            for (std::size_t i = 0; i + 1 < kv.size(); ++i) {
                if (kv[i] == 0) {
                    continue;
                }
                h += first_byte(sv(static_cast<std::uint32_t>(kv[i])));
                ++i;
            }
        }
        for (const osm_a::Node& node : g.nodes()) {
            h += kNode * u(node.id()) + loc(node.lat(), gran, lat_off) +
                 loc(node.lon(), gran, lon_off) + info_sum(node.info());
            for (const std::uint32_t k : node.keys()) {
                h += first_byte(sv(k));
            }
        }
        for (const osm_a::Way& way : g.ways()) {
            h += kWay * u(way.id()) + info_sum(way.info());
            std::int64_t ref = 0;
            for (const std::int64_t d : way.refs()) {
                ref += d;
                h += u(ref);
            }
            for (const std::uint32_t k : way.keys()) {
                h += first_byte(sv(k));
            }
        }
        for (const osm_a::Relation& rel : g.relations()) {
            h += kRelation * u(rel.id()) + info_sum(rel.info());
            std::int64_t mem = 0;
            for (const std::int64_t d : rel.memids()) {
                mem += d;
                h += u(mem);
            }
            for (const std::uint32_t k : rel.keys()) {
                h += first_byte(sv(k));
            }
        }
    }
    return h;
}

// ── protoc arm ────────────────────────────────────────────────────────────────────────────────

std::uint64_t protoc_block_sum(const ::OSMPBF::PrimitiveBlock& b) {
    const std::int64_t gran = b.granularity();
    const std::int64_t lat_off = b.lat_offset();
    const std::int64_t lon_off = b.lon_offset();
    const std::int64_t date_gran = b.date_granularity();
    const auto& st = b.stringtable();
    const auto sv = [&](std::uint64_t i) {
        return i < static_cast<std::uint64_t>(st.s_size())
                   ? std::string_view(st.s(static_cast<int>(i)))
                   : std::string_view{};
    };
    // Field-present-or-zero, matching every other arm (protoc's accessor would otherwise apply
    // the schema's -1 defaults).
    const auto info_sum = [&](const ::OSMPBF::Info& info, bool has) {
        if (!has) {
            return std::uint64_t{0};
        }
        return u(info.has_version() ? info.version() : 0) +
               u(info.has_timestamp() ? info.timestamp() * date_gran / 1000 : 0) +
               u(info.has_changeset() ? info.changeset() : 0) + u(info.has_uid() ? info.uid() : 0) +
               first_byte(sv(info.has_user_sid() ? info.user_sid() : 0));
    };

    std::uint64_t h = 0;
    for (const ::OSMPBF::PrimitiveGroup& g : b.primitivegroup()) {
        if (g.has_dense()) {
            const ::OSMPBF::DenseNodes& d = g.dense();
            const int n = std::min({d.id_size(), d.lat_size(), d.lon_size()});
            std::int64_t id = 0, lat = 0, lon = 0;
            for (int i = 0; i < n; ++i) {
                id += d.id(i);
                lat += d.lat(i);
                lon += d.lon(i);
                h += kNode * u(id) + loc(lat, gran, lat_off) + loc(lon, gran, lon_off);
            }
            if (d.has_denseinfo()) {
                const ::OSMPBF::DenseInfo& di = d.denseinfo();
                std::int64_t t = 0, c = 0, ui = 0, us = 0;
                for (int i = 0; i < di.version_size(); ++i) {
                    h += u(di.version(i));
                }
                for (int i = 0; i < di.timestamp_size(); ++i) {
                    t += di.timestamp(i);
                    h += u(t * date_gran / 1000);
                }
                for (int i = 0; i < di.changeset_size(); ++i) {
                    c += di.changeset(i);
                    h += u(c);
                }
                for (int i = 0; i < di.uid_size(); ++i) {
                    ui += di.uid(i);
                    h += u(ui);
                }
                for (int i = 0; i < di.user_sid_size(); ++i) {
                    us += di.user_sid(i);
                    h += first_byte(sv(u(us)));
                }
            }
            for (int i = 0; i + 1 < d.keys_vals_size(); ++i) {
                if (d.keys_vals(i) == 0) {
                    continue;
                }
                h += first_byte(sv(static_cast<std::uint32_t>(d.keys_vals(i))));
                ++i;
            }
        }
        for (const ::OSMPBF::Node& node : g.nodes()) {
            h += kNode * u(node.id()) + loc(node.lat(), gran, lat_off) +
                 loc(node.lon(), gran, lon_off) + info_sum(node.info(), node.has_info());
            for (int i = 0; i < node.keys_size(); ++i) {
                h += first_byte(sv(node.keys(i)));
            }
        }
        for (const ::OSMPBF::Way& way : g.ways()) {
            h += kWay * u(way.id()) + info_sum(way.info(), way.has_info());
            std::int64_t ref = 0;
            for (int i = 0; i < way.refs_size(); ++i) {
                ref += way.refs(i);
                h += u(ref);
            }
            for (int i = 0; i < way.keys_size(); ++i) {
                h += first_byte(sv(way.keys(i)));
            }
        }
        for (const ::OSMPBF::Relation& rel : g.relations()) {
            h += kRelation * u(rel.id()) + info_sum(rel.info(), rel.has_info());
            std::int64_t mem = 0;
            for (int i = 0; i < rel.memids_size(); ++i) {
                mem += rel.memids(i);
                h += u(mem);
            }
            for (int i = 0; i < rel.keys_size(); ++i) {
                h += first_byte(sv(rel.keys(i)));
            }
        }
    }
    return h;
}

// ── streaming walk ────────────────────────────────────────────────────────────────────────────
// Two passes per block (stringtable first -- wire order guarantees nothing), and the metadata
// columns are consumed through callbacks so this arm does not skip work the others parse.

std::uint64_t stream_block_sum(std::string_view payload, std::vector<std::string_view>& strings,
                               bool* ok) {
    strings.clear();
    std::int64_t gran = 100, lat_off = 0, lon_off = 0, date_gran = 1000;
    rapidproto::DecodeStatus st = osm_s::PrimitiveBlock{payload}.decode(
        [&](osm_s::PrimitiveBlock::stringtable, osm_s::StringTable t) {
            return t.decode(
                [&](osm_s::StringTable::s, std::string_view s) { strings.push_back(s); });
        },
        [&](osm_s::PrimitiveBlock::granularity, std::int32_t v) { gran = v; },
        [&](osm_s::PrimitiveBlock::lat_offset, std::int64_t v) { lat_off = v; },
        [&](osm_s::PrimitiveBlock::lon_offset, std::int64_t v) { lon_off = v; },
        [&](osm_s::PrimitiveBlock::date_granularity, std::int32_t v) { date_gran = v; });
    if (!st.ok()) {
        *ok = false;
        return 0;
    }
    const auto sv = [&](std::uint64_t i) {
        return i < strings.size() ? strings[static_cast<std::size_t>(i)] : std::string_view{};
    };

    std::uint64_t h = 0;
    const auto info_cb = [&](osm_s::Info info) {
        return info.decode(
            [&](osm_s::Info::version, std::int32_t v) { h += u(v); },
            [&](osm_s::Info::timestamp, std::int64_t v) { h += u(v * date_gran / 1000); },
            [&](osm_s::Info::changeset, std::int64_t v) { h += u(v); },
            [&](osm_s::Info::uid, std::int32_t v) { h += u(v); },
            [&](osm_s::Info::user_sid, std::uint32_t v) { h += first_byte(sv(v)); });
    };
    st = osm_s::PrimitiveBlock{payload}.decode(
        [&](osm_s::PrimitiveBlock::primitivegroup, osm_s::PrimitiveGroup group) {
            std::int64_t dn_id = 0, dn_lat = 0, dn_lon = 0;
            std::int64_t di_t = 0, di_c = 0, di_ui = 0, di_us = 0;
            std::int64_t pending = -1, way_ref = 0, rel_mem = 0;
            return group.decode(
                [&](osm_s::PrimitiveGroup::dense, osm_s::DenseNodes dense) {
                    return dense.decode(
                        [&](osm_s::DenseNodes::id, std::int64_t d) {
                            dn_id += d;
                            h += kNode * u(dn_id);
                        },
                        [&](osm_s::DenseNodes::lat, std::int64_t d) {
                            dn_lat += d;
                            h += loc(dn_lat, gran, lat_off);
                        },
                        [&](osm_s::DenseNodes::lon, std::int64_t d) {
                            dn_lon += d;
                            h += loc(dn_lon, gran, lon_off);
                        },
                        [&](osm_s::DenseNodes::keys_vals, std::int32_t v) {
                            if (pending >= 0) {
                                h += first_byte(sv(static_cast<std::uint32_t>(pending)));
                                pending = -1;
                            } else if (v != 0) {
                                pending = v;
                            }
                        },
                        [&](osm_s::DenseNodes::denseinfo, osm_s::DenseInfo di) {
                            return di.decode(
                                [&](osm_s::DenseInfo::version, std::int32_t v) { h += u(v); },
                                [&](osm_s::DenseInfo::timestamp, std::int64_t d) {
                                    di_t += d;
                                    h += u(di_t * date_gran / 1000);
                                },
                                [&](osm_s::DenseInfo::changeset, std::int64_t d) {
                                    di_c += d;
                                    h += u(di_c);
                                },
                                [&](osm_s::DenseInfo::uid, std::int32_t d) {
                                    di_ui += d;
                                    h += u(di_ui);
                                },
                                [&](osm_s::DenseInfo::user_sid, std::int32_t d) {
                                    di_us += d;
                                    h += first_byte(sv(u(di_us)));
                                });
                        });
                },
                [&](osm_s::PrimitiveGroup::nodes, osm_s::Node node) {
                    return node.decode(
                        [&](osm_s::Node::id, std::int64_t id) { h += kNode * u(id); },
                        [&](osm_s::Node::lat, std::int64_t v) { h += loc(v, gran, lat_off); },
                        [&](osm_s::Node::lon, std::int64_t v) { h += loc(v, gran, lon_off); },
                        [&](osm_s::Node::keys, std::uint32_t k) { h += first_byte(sv(k)); },
                        [&](osm_s::Node::info, osm_s::Info info) { return info_cb(info); });
                },
                [&](osm_s::PrimitiveGroup::ways, osm_s::Way way) {
                    way_ref = 0;
                    return way.decode(
                        [&](osm_s::Way::id, std::int64_t id) { h += kWay * u(id); },
                        [&](osm_s::Way::refs, std::int64_t d) {
                            way_ref += d;
                            h += u(way_ref);
                        },
                        [&](osm_s::Way::keys, std::uint32_t k) { h += first_byte(sv(k)); },
                        [&](osm_s::Way::info, osm_s::Info info) { return info_cb(info); });
                },
                [&](osm_s::PrimitiveGroup::relations, osm_s::Relation rel) {
                    rel_mem = 0;
                    return rel.decode(
                        [&](osm_s::Relation::id, std::int64_t id) { h += kRelation * u(id); },
                        [&](osm_s::Relation::memids, std::int64_t d) {
                            rel_mem += d;
                            h += u(rel_mem);
                        },
                        [&](osm_s::Relation::keys, std::uint32_t k) { h += first_byte(sv(k)); },
                        [&](osm_s::Relation::info, osm_s::Info info) { return info_cb(info); });
                });
        });
    if (!st.ok()) {
        *ok = false;
    }
    return h;
}

// ── libosmium arm ─────────────────────────────────────────────────────────────────────────────

struct SumHandler : public osmium::handler::Handler {
    std::uint64_t h = 0;

    std::uint64_t meta(const osmium::OSMObject& o) {
        return u(o.version()) + u(o.timestamp().seconds_since_epoch()) + u(o.changeset()) +
               u(o.uid()) + first_byte(o.user());
    }

    std::uint64_t tags(const osmium::OSMObject& o) {
        std::uint64_t t = 0;
        for (const osmium::Tag& tag : o.tags()) {
            t += static_cast<unsigned char>(tag.key()[0]);
        }
        return t;
    }

    void node(const osmium::Node& n) {
        h += kNode * u(n.id()) + u(n.location().x()) + u(n.location().y()) + meta(n) + tags(n);
    }

    void way(const osmium::Way& w) {
        h += kWay * u(w.id()) + meta(w) + tags(w);
        for (const osmium::NodeRef& ref : w.nodes()) {
            h += u(ref.ref());
        }
    }

    void relation(const osmium::Relation& r) {
        h += kRelation * u(r.id()) + meta(r) + tags(r);
        for (const osmium::RelationMember& m : r.members()) {
            h += u(m.ref());
        }
    }
};

std::uint64_t osmium_file_sum(const std::string& file_bytes) {
    const osmium::io::File file{file_bytes.data(), file_bytes.size(), "pbf"};
    osmium::io::Reader reader{file, osmium::osm_entity_bits::nwr};
    SumHandler handler;
    while (osmium::memory::Buffer buffer = reader.read()) {
        osmium::apply(buffer, handler);
    }
    reader.close();
    return handler.h;
}

// ── setup: framing + inflate, once ────────────────────────────────────────────────────────────

struct Loaded {
    std::string file;                   // the raw bytes (osm_file scenario input)
    std::vector<std::string> payloads;  // inflated PrimitiveBlock payloads (osm_blocks input)
    std::uint64_t payload_bytes = 0;
    bool ok = false;
};

Loaded load_dataset(const char* path) {
    Loaded out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return out;
    }
    out.file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    std::size_t offset = 0;
    osmpbf_input::Framed frame;
    while (osmpbf_input::next_frame(out.file, offset, frame)) {
        rapidproto::Arena arena;
        const osm_a::BlobHeader* header =
            osm_a::BlobHeader::decode(rapidproto::ByteView(frame.header_bytes), arena);
        if (header == nullptr ||
            !osmpbf_input::take_blob(out.file, offset, header->datasize(), frame)) {
            return out;
        }
        const osm_a::Blob* blob =
            osm_a::Blob::decode(rapidproto::ByteView(frame.blob_bytes), arena);
        if (blob == nullptr) {
            return out;
        }
        bool bad = false;
        std::string payload;
        blob->data([&](osm_a::Blob::Data::raw, std::string_view raw) { payload.assign(raw); },
                   [&](osm_a::Blob::Data::zlib_data, std::string_view deflated) {
                       const std::int64_t raw_size = blob->raw_size().value_or(0);
                       if (raw_size <= 0 || raw_size > 2 * 32 * 1024 * 1024 ||
                           !osmpbf_input::inflate_blob(deflated, static_cast<std::size_t>(raw_size),
                                                       payload)) {
                           bad = true;
                       }
                   },
                   [&](osm_a::Blob::Data::lzma_data, std::string_view) { bad = true; },
                   [&](osm_a::Blob::Data::OBSOLETE_bzip2_data, std::string_view) { bad = true; },
                   [&](osm_a::Blob::Data::lz4_data, std::string_view) { bad = true; },
                   [&](osm_a::Blob::Data::zstd_data, std::string_view) { bad = true; },
                   [&](std::monostate) { bad = true; });
        if (bad) {
            return out;
        }
        if (header->type() == "OSMData") {
            out.payload_bytes += payload.size();
            out.payloads.push_back(std::move(payload));
        }
    }
    out.ok = offset == out.file.size() && !out.payloads.empty();
    return out;
}

// ── the whole-file rapidproto drivers (osm_file scenario) ────────────────────────────────────

template <class BlockFn>
std::uint64_t rp_file_sum(const std::string& file_bytes, std::string& inflated, BlockFn block) {
    std::uint64_t h = 0;
    std::size_t offset = 0;
    osmpbf_input::Framed frame;
    while (osmpbf_input::next_frame(file_bytes, offset, frame)) {
        rapidproto::Arena arena;
        const osm_a::BlobHeader* header =
            osm_a::BlobHeader::decode(rapidproto::ByteView(frame.header_bytes), arena);
        if (header == nullptr ||
            !osmpbf_input::take_blob(file_bytes, offset, header->datasize(), frame)) {
            return 0;
        }
        const osm_a::Blob* blob =
            osm_a::Blob::decode(rapidproto::ByteView(frame.blob_bytes), arena);
        if (blob == nullptr) {
            return 0;
        }
        std::string_view payload;
        bool bad = false;
        blob->data([&](osm_a::Blob::Data::raw, std::string_view raw) { payload = raw; },
                   [&](osm_a::Blob::Data::zlib_data, std::string_view deflated) {
                       const std::int64_t raw_size = blob->raw_size().value_or(0);
                       if (raw_size <= 0 || raw_size > 2 * 32 * 1024 * 1024 ||
                           !osmpbf_input::inflate_blob(deflated, static_cast<std::size_t>(raw_size),
                                                       inflated)) {
                           bad = true;
                           return;
                       }
                       payload = inflated;
                   },
                   [&](osm_a::Blob::Data::lzma_data, std::string_view) { bad = true; },
                   [&](osm_a::Blob::Data::OBSOLETE_bzip2_data, std::string_view) { bad = true; },
                   [&](osm_a::Blob::Data::lz4_data, std::string_view) { bad = true; },
                   [&](osm_a::Blob::Data::zstd_data, std::string_view) { bad = true; },
                   [&](std::monostate) { bad = true; });
        if (bad) {
            return 0;
        }
        if (header->type() == "OSMData") {
            h += block(payload);
        }
    }
    return h;
}

}  // namespace

int run_arm() {
    const Loaded data = load_dataset(RAPIDPROTO_OSM_DATASET);
    if (!data.ok) {
        // Degrade, don't fail: the dataset is fetched separately (tests/fetch_osm_dataset.py)
        // and its absence should cost the SCENARIOS, not the whole bench run.
        std::fprintf(stderr,
                     "note: osm scenarios SKIPPED -- dataset not readable (run "
                     "tests/fetch_osm_dataset.py)\n");
        return 0;
    }

    // Validate every arm's checksum before timing anything. The block-level and file-level
    // rapidproto paths must agree with each other AND with protoc and libosmium.
    std::uint64_t c_protoc = 0, c_arena = 0, c_stream = 0;
    {
        std::vector<std::string_view> strings;
        bool stream_ok = true;
        rapidproto::Arena arena;
        for (const std::string& p : data.payloads) {
            ::OSMPBF::PrimitiveBlock pb;
            if (!pb.ParseFromArray(p.data(), static_cast<int>(p.size()))) {
                std::fprintf(stderr, "CHECKSUM MISMATCH (osm): protoc failed to parse a block\n");
                return -1;
            }
            c_protoc += protoc_block_sum(pb);
            arena.reset();
            const osm_a::PrimitiveBlock* block =
                osm_a::PrimitiveBlock::decode(rapidproto::ByteView(p), arena);
            if (block == nullptr) {
                std::fprintf(stderr, "CHECKSUM MISMATCH (osm): arena failed to decode a block\n");
                return -1;
            }
            c_arena += arena_block_sum(block);
            c_stream += stream_block_sum(p, strings, &stream_ok);
        }
        std::string scratch;
        const std::uint64_t c_arena_file = rp_file_sum(data.file, scratch, [&](std::string_view p) {
            rapidproto::Arena a;
            return arena_block_sum(osm_a::PrimitiveBlock::decode(rapidproto::ByteView(p), a));
        });
        std::vector<std::string_view> s2;
        bool s2_ok = true;
        const std::uint64_t c_stream_file =
            rp_file_sum(data.file, scratch,
                        [&](std::string_view p) { return stream_block_sum(p, s2, &s2_ok); });
        const std::uint64_t c_osmium = osmium_file_sum(data.file);
        if (!stream_ok || !s2_ok || c_arena != c_protoc || c_arena != c_stream ||
            c_arena != c_arena_file || c_arena != c_stream_file || c_arena != c_osmium) {
            std::fprintf(stderr,
                         "CHECKSUM MISMATCH (osm): arena=%llu protoc=%llu stream=%llu "
                         "arena-file=%llu stream-file=%llu osmium=%llu\n",
                         static_cast<unsigned long long>(c_arena),
                         static_cast<unsigned long long>(c_protoc),
                         static_cast<unsigned long long>(c_stream),
                         static_cast<unsigned long long>(c_arena_file),
                         static_cast<unsigned long long>(c_stream_file),
                         static_cast<unsigned long long>(c_osmium));
            return -1;
        }
    }

    int bad = 0;

    // osm_blocks: the protobuf layer alone (payloads pre-inflated, untimed).
    {
        rapidproto::Arena warm;
        std::vector<std::string_view> strings;
        std::vector<rpbench::Arm> arms = {
            {"protoc",
             [&]() {
                 std::uint64_t h = 0;
                 for (const std::string& p : data.payloads) {
                     ::OSMPBF::PrimitiveBlock pb;
                     if (pb.ParseFromArray(p.data(), static_cast<int>(p.size()))) {
                         h += protoc_block_sum(pb);
                     }
                 }
                 return h;
             }},
            {"arena",
             [&]() {
                 std::uint64_t h = 0;
                 for (const std::string& p : data.payloads) {
                     warm.reset();
                     h += arena_block_sum(
                         osm_a::PrimitiveBlock::decode(rapidproto::ByteView(p), warm));
                 }
                 return h;
             }},
            {"streamgen",
             [&]() {
                 std::uint64_t h = 0;
                 bool ok = true;
                 for (const std::string& p : data.payloads) {
                     h += stream_block_sum(p, strings, &ok);
                 }
                 return ok ? h : 0;
             }},
        };
        const int r = rpbench::run("osm_blocks", static_cast<double>(data.payload_bytes), arms);
        if (r < 0) {
            return -1;
        }
        bad += r;
    }

    // osm_file: everything from the raw bytes -- framing, blob decode, inflate, decode, walk.
    {
        rapidproto::Arena warm;
        std::string scratch;
        std::vector<std::string_view> strings;
        bool ok = true;
        std::vector<rpbench::Arm> arms = {
            {"arena",
             [&]() {
                 return rp_file_sum(data.file, scratch, [&](std::string_view p) {
                     warm.reset();
                     return arena_block_sum(
                         osm_a::PrimitiveBlock::decode(rapidproto::ByteView(p), warm));
                 });
             }},
            {"streamgen",
             [&]() {
                 return rp_file_sum(data.file, scratch, [&](std::string_view p) {
                     return stream_block_sum(p, strings, &ok);
                 });
             }},
            {"libosmium", [&]() { return osmium_file_sum(data.file); },
             /*threaded=*/true},  // its reader decompresses/parses on worker threads
        };
        const int r = rpbench::run("osm_file", static_cast<double>(data.file.size()), arms);
        if (r < 0) {
            return -1;
        }
        bad += r;
    }
    return bad;
}

}  // namespace rposm

#endif  // RAPIDPROTO_BENCH_OSM
