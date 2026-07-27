// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The protoc baseline arms. See bench_baselines.hpp for why these live in their own translation
// unit rather than beside the decoders they are a baseline for.
//
// Each entry point PARSES AND CHECKSUMS in this TU: exporting only the checksum would leave the
// ParseFromString call -- the measured work -- back in the caller's TU, which is exactly the
// coupling this separation exists to remove.
#include "bench_baselines.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include <google/protobuf/arena.h>

#include "bench.pb.h"

namespace rpbaseline {
namespace {

// A local copy; see the note in bench_baselines.cpp on why this is not shared through a header.
std::uint64_t bits(double d) {
    std::uint64_t b = 0;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

std::uint64_t checksum_protoc(const bench::Dataset& d) {
    std::uint64_t s = static_cast<std::uint64_t>(d.version()) + d.name().size();
    for (const bench::Person& p : d.people()) {
        s += static_cast<std::uint64_t>(p.id()) + p.name().size() + p.email().size() +
             (p.active() ? 1U : 0U) + bits(p.score()) + p.created();
        if (p.has_address()) {
            s += p.address().street().size() + p.address().city().size() + p.address().zip();
        }
        for (const std::string& t : p.tags()) {
            s += t.size();
        }
        for (const std::int32_t h : p.history()) {
            s += static_cast<std::uint32_t>(h);
        }
        for (const bench::Attribute& a : p.attributes()) {
            s += a.key().size() + a.value().size();
        }
        for (const auto& kv : p.counters()) {
            s += kv.first.size() + static_cast<std::uint32_t>(kv.second);
        }
    }
    return s;
}

// protoc-side mirrors of checksum_arena_big / checksum_arena_wide, for the repeated-shape runs' protoc
// arm (each run's arena and protoc arms decode the same bytes, so their checksums must agree).
std::uint64_t checksum_protoc_big(const bench::BigSet& b) {
    std::uint64_t s = 0;
    for (const bench::Big& it : b.items()) {
        for (const std::int64_t v : it.numbers()) {
            s += static_cast<std::uint64_t>(v);
        }
        for (const double r : it.reals()) {
            s += bits(r);
        }
        for (const std::int64_t v : it.zz()) {
            s += static_cast<std::uint64_t>(v);
        }
        for (const int k : it.kinds()) {
            s += static_cast<std::uint64_t>(k);
        }
    }
    return s;
}

std::uint64_t checksum_protoc_wide(const bench::WideSet& w) {
    std::uint64_t s = 0;
    for (const bench::Wide& it : w.items()) {
        for (const std::int32_t v : it.a()) {
            s += static_cast<std::uint32_t>(v);
        }
        for (const std::int32_t v : it.b()) {
            s += static_cast<std::uint32_t>(v);
        }
        for (const std::int32_t v : it.c()) {
            s += static_cast<std::uint32_t>(v);
        }
        for (const std::int32_t v : it.d()) {
            s += static_cast<std::uint32_t>(v);
        }
        for (const auto& x : it.s()) {
            s += x.size();
        }
        for (const auto& x : it.t()) {
            s += x.size();
        }
    }
    return s;
}

std::uint64_t checksum_protoc_record(const bench::RecordSet& d) {
    std::uint64_t s = 0;
    const auto sample = [&](const bench::Sample& x) {
        s += static_cast<std::uint64_t>(x.a()) + static_cast<std::uint64_t>(x.b()) +
             static_cast<std::uint64_t>(x.c()) + static_cast<std::uint64_t>(x.d()) +
             (x.e() ? 1U : 0U) + x.f();
    };
    for (const bench::Record& r : d.records()) {
        s += static_cast<std::uint64_t>(r.f1()) + static_cast<std::uint64_t>(r.f2()) +
             static_cast<std::uint64_t>(r.f3()) + static_cast<std::uint64_t>(r.f4()) + r.f5() +
             static_cast<std::uint64_t>(r.f6()) + (r.f7() ? 1U : 0U) + r.f8() +
             static_cast<std::uint64_t>(r.f9()) + static_cast<std::uint64_t>(r.f10()) +
             static_cast<std::uint64_t>(r.f11()) + static_cast<std::uint64_t>(r.f12()) +
             static_cast<std::uint64_t>(r.f13()) + static_cast<std::uint64_t>(r.f14()) +
             static_cast<std::uint64_t>(r.f15()) + static_cast<std::uint64_t>(r.f18()) +
             static_cast<std::uint64_t>(r.f19()) + static_cast<std::uint64_t>(r.f20());
        if (r.has_s1()) {
            sample(r.s1());
        }
        if (r.has_s2()) {
            sample(r.s2());
        }
    }
    return s;
}

std::uint64_t checksum_big_protoc(const bench::Big& b) {
    std::uint64_t s = 0;
    for (const std::int64_t v : b.numbers()) {
        s += static_cast<std::uint64_t>(v);
    }
    return s;
}

std::uint64_t checksum_big_protoc_zz(const bench::Big& b) {
    std::uint64_t s = 0;
    for (const std::int64_t v : b.zz()) {
        s += static_cast<std::uint64_t>(v);
    }
    return s;
}

std::uint64_t checksum_big_protoc_kinds(const bench::Big& b) {
    std::uint64_t s = 0;
    for (const int k : b.kinds()) {
        s += static_cast<std::uint64_t>(static_cast<std::int32_t>(k));
    }
    return s;
}

// Parse into a fresh protoc Arena and checksum: the whole measured operation, in one place.
template <class Message, class Fn>
std::uint64_t parse_and_sum(const std::string& buf, Fn&& sum) {
    google::protobuf::Arena arena;
    auto* message = google::protobuf::Arena::CreateMessage<Message>(&arena);
    message->ParseFromString(buf);
    return sum(*message);
}

}  // namespace

std::uint64_t protoc_dataset(const std::string& buf) {
    return parse_and_sum<bench::Dataset>(buf, checksum_protoc);
}
std::uint64_t protoc_big_set(const std::string& buf) {
    return parse_and_sum<bench::BigSet>(buf, checksum_protoc_big);
}
std::uint64_t protoc_wide(const std::string& buf) {
    return parse_and_sum<bench::WideSet>(buf, checksum_protoc_wide);
}
std::uint64_t protoc_record(const std::string& buf) {
    return parse_and_sum<bench::RecordSet>(buf, checksum_protoc_record);
}
std::uint64_t protoc_big(const std::string& buf) {
    return parse_and_sum<bench::Big>(buf, checksum_big_protoc);
}
std::uint64_t protoc_big_zz(const std::string& buf) {
    return parse_and_sum<bench::Big>(buf, checksum_big_protoc_zz);
}
std::uint64_t protoc_big_kinds(const std::string& buf) {
    return parse_and_sum<bench::Big>(buf, checksum_big_protoc_kinds);
}

ProtocMemory protoc_dataset_memory(const std::string& buf) {
    google::protobuf::Arena arena;
    auto* message = google::protobuf::Arena::CreateMessage<bench::Dataset>(&arena);
    message->ParseFromString(buf);
    return {static_cast<std::size_t>(arena.SpaceUsed()),
            static_cast<std::size_t>(arena.SpaceAllocated())};
}

}  // namespace rpbaseline
