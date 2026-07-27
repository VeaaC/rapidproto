// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The protozero baseline arms. See bench_baselines.hpp for why these live in their own
// translation unit rather than beside the decoders they are a baseline for.
#include "bench_baselines.hpp"

#include <cstdint>
#include <cstring>

#include "rapidproto/runtime.hpp"

#if __has_include(<protozero/pbf_reader.hpp>)
#include <protozero/pbf_reader.hpp>

namespace rpbaseline {
namespace {

// A local copy, deliberately: sharing it with bench_arena.cpp through a header would put an
// inline function in both TUs and reintroduce exactly the coupling this separation removes.
std::uint64_t bits(double d) {
    std::uint64_t b = 0;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

// protozero (mapbox pbf_reader) yardstick: a zero-materialization pull parse of the SAME Dataset,
// summed identically to checksum_stream so the cross-check holds. get_view() is the zero-copy
// string path (matches the streaming decoder's string_view). protozero's wire-type checks are
// protozero_assert()s compiled out under NDEBUG, so it validates marginally less than we do.
std::uint64_t checksum_protozero(rapidproto::ByteView buf) {
    std::uint64_t s = 0;
    protozero::pbf_reader ds{buf.data(), buf.size()};
    while (ds.next()) {
        switch (ds.tag()) {
            case 1:
                s += ds.get_view().size();  // Dataset.name
                break;
            case 2:
                s += static_cast<std::uint64_t>(ds.get_int64());  // Dataset.version
                break;
            case 3: {  // Dataset.people (repeated Person)
                protozero::pbf_reader p = ds.get_message();
                while (p.next()) {
                    switch (p.tag()) {
                        case 1:
                            s += static_cast<std::uint64_t>(p.get_int64());  // id
                            break;
                        case 2:
                            s += p.get_view().size();  // name
                            break;
                        case 3:
                            s += p.get_view().size();  // email
                            break;
                        case 4:
                            s += p.get_bool() ? 1U : 0U;  // active
                            break;
                        case 5:
                            s += bits(p.get_double());  // score
                            break;
                        case 6:
                            s += p.get_fixed64();  // created
                            break;
                        case 7: {  // address (nested)
                            protozero::pbf_reader a = p.get_message();
                            while (a.next()) {
                                switch (a.tag()) {
                                    case 1:
                                        s += a.get_view().size();  // street
                                        break;
                                    case 2:
                                        s += a.get_view().size();  // city
                                        break;
                                    case 3:
                                        s += a.get_uint32();  // zip
                                        break;
                                    default:
                                        a.skip();
                                }
                            }
                            break;
                        }
                        case 8:
                            s += p.get_view().size();  // tags (repeated string)
                            break;
                        case 9: {  // history (packed int32)
                            const auto packed = p.get_packed_int32();
                            for (const auto v : packed) {
                                s += static_cast<std::uint32_t>(v);
                            }
                            break;
                        }
                        case 10: {  // attributes (repeated Attribute)
                            protozero::pbf_reader a = p.get_message();
                            while (a.next()) {
                                switch (a.tag()) {
                                    case 1:
                                        s += a.get_view().size();  // key
                                        break;
                                    case 2:
                                        s += a.get_view().size();  // value
                                        break;
                                    default:
                                        a.skip();
                                }
                            }
                            break;
                        }
                        case 11: {  // counters (map<string,int32> == repeated {key=1,value=2})
                            protozero::pbf_reader e = p.get_message();
                            std::size_t klen = 0;
                            std::int32_t val = 0;
                            while (e.next()) {
                                switch (e.tag()) {
                                    case 1:
                                        klen = e.get_view().size();
                                        break;
                                    case 2:
                                        val = e.get_int32();
                                        break;
                                    default:
                                        e.skip();
                                }
                            }
                            s += klen + static_cast<std::uint32_t>(val);
                            break;
                        }
                        default:
                            p.skip();
                    }
                }
                break;
            }
            default:
                ds.skip();
        }
    }
    return s;
}

std::uint64_t checksum_big_protozero(rapidproto::ByteView buf) {
    std::uint64_t s = 0;
    protozero::pbf_reader r{buf.data(), buf.size()};
    while (r.next()) {
        if (r.tag() == 1) {
            auto packed = r.get_packed_int64();
            for (const auto v : packed) {
                s += static_cast<std::uint64_t>(v);
            }
        } else {
            r.skip();
        }
    }
    return s;
}

std::uint64_t checksum_big_protozero_zz(rapidproto::ByteView buf) {
    std::uint64_t s = 0;
    protozero::pbf_reader r{buf.data(), buf.size()};
    while (r.next()) {
        if (r.tag() == 3) {
            auto packed = r.get_packed_sint64();
            for (const auto v : packed) {
                s += static_cast<std::uint64_t>(v);
            }
        } else {
            r.skip();
        }
    }
    return s;
}

std::uint64_t checksum_big_protozero_kinds(rapidproto::ByteView buf) {
    std::uint64_t s = 0;
    protozero::pbf_reader r{buf.data(), buf.size()};
    while (r.next()) {
        if (r.tag() == 4) {
            auto packed = r.get_packed_enum();
            for (const auto v : packed) {
                s += static_cast<std::uint64_t>(static_cast<std::int32_t>(v));
            }
        } else {
            r.skip();
        }
    }
    return s;
}

}  // namespace

std::uint64_t protozero_dataset(::rapidproto::ByteView buf) {
    return checksum_protozero(buf);
}
std::uint64_t protozero_big(::rapidproto::ByteView buf) {
    return checksum_big_protozero(buf);
}
std::uint64_t protozero_big_zz(::rapidproto::ByteView buf) {
    return checksum_big_protozero_zz(buf);
}
std::uint64_t protozero_big_kinds(::rapidproto::ByteView buf) {
    return checksum_big_protozero_kinds(buf);
}

}  // namespace rpbaseline

#endif  // __has_include(<protozero/pbf_reader.hpp>)
