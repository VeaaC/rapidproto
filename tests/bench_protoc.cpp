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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include <google/protobuf/arena.h>

#include "bench.pb.h"
#include "bench_pb_arena.hpp"

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

// The fields `records sparse (dropped)` keeps (tests/bench/records_sparse.modes). protoc parses every
// field regardless.
std::uint64_t checksum_protoc_record_sparse(const bench::RecordSet& d) {
    std::uint64_t s = 0;
    for (const bench::Record& r : d.records()) {
        s += static_cast<std::uint64_t>(r.f1()) + static_cast<std::uint64_t>(r.f10()) +
             static_cast<std::uint64_t>(r.f20());
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

// Parse into a fresh protoc Arena and checksum: the whole measured operation, in one place. A failed
// parse returns a checksum no RapidProto arm produces, so the run fails on it.
template <class Message, class Fn>
std::uint64_t parse_and_sum(const std::string& buf, Fn&& sum) {
    google::protobuf::Arena arena;
    auto* message = new_in_arena<Message>(&arena);
    if (!message->ParseFromString(buf)) {
        return ~std::uint64_t{0};
    }
    return sum(*message);
}

// protoc's side of the arena decoder's reused arena: one Arena per message type, Reset() before
// each parse. Reset() frees every block except a caller-owned first one, so the Arena starts on a
// block large enough that no parse here needs a second. protobuf v35 keeps the Arena's cleanup list
// (the destructors Reset() runs) outside its blocks, so Dataset's map entries still take heap
// chunks on every parse.
constexpr std::size_t kWarmBlockBytes = std::size_t{32} << 20;

template <class Message, class Fn>
std::uint64_t parse_and_sum_warm(const std::string& buf, Fn&& sum) {
    static const std::unique_ptr<char[]> block(new char[kWarmBlockBytes]);
    static google::protobuf::Arena arena(block.get(), kWarmBlockBytes);
    arena.Reset();
    auto* message = new_in_arena<Message>(&arena);
    if (!message->ParseFromString(buf)) {
        return ~std::uint64_t{0};
    }
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
std::uint64_t protoc_record_sparse(const std::string& buf) {
    return parse_and_sum<bench::RecordSet>(buf, checksum_protoc_record_sparse);
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
std::uint64_t protoc_dataset_warm(const std::string& buf) {
    return parse_and_sum_warm<bench::Dataset>(buf, checksum_protoc);
}
std::uint64_t protoc_big_set_warm(const std::string& buf) {
    return parse_and_sum_warm<bench::BigSet>(buf, checksum_protoc_big);
}
std::uint64_t protoc_wide_warm(const std::string& buf) {
    return parse_and_sum_warm<bench::WideSet>(buf, checksum_protoc_wide);
}
std::uint64_t protoc_record_warm(const std::string& buf) {
    return parse_and_sum_warm<bench::RecordSet>(buf, checksum_protoc_record);
}

namespace {

// The heap buffers of a parsed message's strings. The Arena holds each std::string object, but a
// string too long for its inline buffer keeps its text in operator new memory, which the Arena's
// SpaceUsed / SpaceAllocated never see. `used` adds what was requested (capacity + 1). `held` adds
// the chunk glibc's malloc carves for that request: the request plus its 8-byte size header,
// rounded up to 16, at least 32. It's computed from the request, not asked of the allocator:
// malloc_usable_size also reports the slack of a reused chunk, which depends on what ran before.
struct StringHeap {
    std::size_t used = 0;
    std::size_t held = 0;
    void add(const std::string& s) {
        const char* object = reinterpret_cast<const char*>(&s);
        const std::less<const char*> before;
        if (!before(s.data(), object) && before(s.data(), object + sizeof s)) {
            return;  // the text sits in the object's inline buffer
        }
        const std::size_t request = s.capacity() + 1;
        used += request;
        held += std::max<std::size_t>(32, (request + sizeof(std::size_t) + 15) & ~std::size_t{15});
    }
};

}  // namespace

ProtocMemory protoc_dataset_memory(const std::string& buf) {
    google::protobuf::Arena arena;
    auto* message = new_in_arena<bench::Dataset>(&arena);
    if (!message->ParseFromString(buf)) {
        return {0, 0};
    }
    StringHeap heap;
    heap.add(message->name());
    for (const bench::Person& p : message->people()) {
        heap.add(p.name());
        heap.add(p.email());
        if (p.has_address()) {
            heap.add(p.address().street());
            heap.add(p.address().city());
        }
        for (const std::string& t : p.tags()) {
            heap.add(t);
        }
        for (const bench::Attribute& a : p.attributes()) {
            heap.add(a.key());
            heap.add(a.value());
        }
        for (const auto& kv : p.counters()) {
            heap.add(kv.first);
        }
    }
    return {static_cast<std::size_t>(arena.SpaceUsed()) + heap.used,
            static_cast<std::size_t>(arena.SpaceAllocated()) + heap.held};
}

}  // namespace rpbaseline
