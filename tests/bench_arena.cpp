// Arena benchmark: a standalone -O3 -DNDEBUG executable (layout-stable, like rapidproto_bench).
// Two parts:
//   1. A realistic bench::Dataset (2000 records) decoded four ways and compared apples-to-apples:
//        arena:     the arena decoder materializing a read-only tree in a bump Arena (under test)
//        protoc:    protoc-generated C++ + google::protobuf::Arena (a materializing baseline)
//        stream:    our streaming callback decoder (zero-materialization)
//        protozero: mapbox pbf_reader (zero-materialization yardstick; absent -> that row is skipped)
//      Reports parse TIME on the shared harness (bench_harness.hpp: drift-invariant ratios vs the
//      protoc baseline + cycles/byte; arena measured "cold" = fresh arena and "warm" = reset+reuse)
//      and peak MEMORY for the two MATERIALIZERS like-with-like: payload (arena bytes_used vs protoc
//      SpaceUsed) and total-malloc'd (arena bytes_reserved vs protoc SpaceAllocated). All four decoders
//      must agree on a checksum.
//   2. Repeated / packed-array shapes (arena-warm vs protoc): packed varint vs fixed-width elements
//      (same element count) and many-messages-few-elements vs few-messages-many (same element count),
//      isolating the arena's repeated-field decode cost across the axes that stress it.
//   3. A chunk-cap sweep (arena only): three payload shapes (a mixed Dataset, a many-small-arrays
//      WideSet, and a few-big-arrays BigSet), each grown from ~0.4 to ~32 MB. For each it reports
//      the held/used ratio (the arena's growth + chunk-tail waste) and cold/warm parse time, so the
//      Arena's chunk-growth policy can be tuned against varied sizes and allocation patterns.
//
// Reading the Dataset numbers (interpretation lives here, not in the program's output):
//   - The time advantage is partly a feature gap: protoc validates UTF-8 on every proto3 string
//     (~20% of its parse); the arena does not. Discount that and the time win is ~2x, not ~2.5x.
//   - The multiple varies with payload shape: more strings favor the arena, more packed scalars
//     favor protoc. This payload is a realistic mix, so do not read one ratio as universal.
//   - The held-memory gap (total malloc'd) is the single-pass-growable repeated-array realloc waste
//     plus the arena's chunk-tail waste; the sweep's held/used column isolates the latter by size.
//
// Knob tuning. The three tuned knobs are compile-time constants, so each was validated by
// recompiling at several values and re-running this benchmark (one binary cannot sweep them); the
// shapes below exist to make each knob matter. The chosen values and the evidence:
//   - Arena chunk cap (arena_runtime.hpp kMaxChunk = 96 KiB): swept none / 64 / 96 / 128 KiB. 96 KiB
//     holds held/used to ~1.0-1.2x across every shape and size here, vs ~1.4-1.9x uncapped (the
//     uncapped final chunk is ~half empty, ~2x more held memory at 32 MB). Warm time is cap-
//     independent; it stays under glibc's 128 KiB mmap threshold.
//   - SSO inline capacity (arena_runtime.hpp ArenaString = 15 chars / 16 bytes): swept a 16 / 24 /
//     32-byte ArenaString on the Dataset shape (also bumping the planner's kStringSize to match).
//     16 wins on both memory and time -- the payload is mostly short strings already inlined, so a
//     wider SSO only widens every string field (used 0.68x -> 0.82x -> 1.00x for 16/24/32).
//   - Inline-submsg cutoff (arenagen layout.hpp = 16 bytes): swept 16 / 24 / 32 on the Particle shape
//     (24-byte Vec3, boxed at 16 and inlined at >=24). 16 wins: inlining a fixed-size sub-message of
//     size S into a growable-array parent costs ~2S of array memory (the struct plus its single-pass
//     realloc copy) vs ~16+S for a pointer, so inlining only pays up to S = 16; inlining the 24-byte
//     Vec3 raised used-memory ~16% (57.6 -> 67.1 MB at 32 MB) with no time benefit.
//
// RUN PINNED to one performance core (hybrid-core scheduling adds 30%+ noise); never run two pinned
// copies on the same core at once:
//   taskset -c 0 ./build/gcc/rapidproto_arena_bench

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/arena.h>

#include "bench.pb.h"           // protoc: bench::Dataset / WideSet / BigSet
#include "bench.rp.hpp"         // arenagen: rp::bench::Dataset / WideSet / BigSet
#include "bench.rp.stream.hpp"  // streamgen: rp::bench::stream::Dataset
#include "bench_harness.hpp"  // rpbench: the shared measurement harness (also used by rapidproto_bench)
#include "bench_varint.hpp"  // repeated-varint sweep builders (shared with the streaming bench)
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

#if __has_include(<protozero/pbf_reader.hpp>)
#include <protozero/pbf_reader.hpp>
#define RAPIDPROTO_HAVE_PROTOZERO 1
#endif

namespace {

constexpr int kPeople = 2000;  // base size for the 3-way headline

std::uint64_t bits(double d) {
    std::uint64_t b = 0;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

// ── builders (protoc serializes the bytes every decoder then parses) ──────────────────────────────

// The realistic mixed shape: records with scalars, strings, a nested message, repeated scalars/
// strings/sub-messages, and a map. `people` scales the payload size.
std::string make_dataset(int people) {
    bench::Dataset ds;
    ds.set_name("benchmark-dataset");
    ds.set_version(7);
    for (int i = 0; i < people; ++i) {
        bench::Person* p = ds.add_people();
        p->set_id(1000000 + i);
        p->set_name("person-" + std::to_string(i));
        p->set_email("person" + std::to_string(i) + "@example.com");
        p->set_active(i % 2 == 0);
        p->set_score(static_cast<double>(i) * 1.5);
        p->set_created(0x100000000ULL + static_cast<std::uint64_t>(i));
        bench::Address* a = p->mutable_address();
        a->set_street(std::to_string(i) + " Main Street");
        a->set_city("City" + std::to_string(i % 50));
        a->set_zip(static_cast<std::uint32_t>(10000 + i % 90000));
        for (int t = 0; t < 3; ++t) {
            p->add_tags("tag" + std::to_string((i + t) % 20));
        }
        for (int h = 0; h < 5; ++h) {
            p->add_history(i * 10 + h);
        }
        for (int k = 0; k < 2; ++k) {
            bench::Attribute* at = p->add_attributes();
            at->set_key("key" + std::to_string(k));
            at->set_value("val-" + std::to_string(i));
        }
        (*p->mutable_counters())["clicks"] = i;
        (*p->mutable_counters())["views"] = i * 2;
    }
    std::string out;
    ds.SerializeToString(&out);
    return out;
}

// Many small arrays: each record carries six short repeated fields -> a flood of tiny allocations.
std::string make_wide(int items) {
    bench::WideSet ws;
    for (int i = 0; i < items; ++i) {
        bench::Wide* w = ws.add_items();
        for (int k = 0; k < 3; ++k) {
            w->add_a(i + k);
            w->add_b(i * 2 + k);
            w->add_c(i * 3 + k);
            w->add_d(i * 4 + k);
        }
        for (int k = 0; k < 2; ++k) {
            w->add_s("s" + std::to_string((i + k) % 32));
            w->add_t("t" + std::to_string((i + k) % 32));
        }
    }
    std::string out;
    ws.SerializeToString(&out);
    return out;
}

// Few big arrays: a handful of records, each dominated by two large scalar arrays (the doubles array
// alone is `per` * 8 bytes, exceeding the chunk cap so it takes its own chunk).
std::string make_big(int items, int per) {
    bench::BigSet bs;
    for (int i = 0; i < items; ++i) {
        bench::Big* b = bs.add_items();
        for (int k = 0; k < per; ++k) {
            b->add_numbers(static_cast<std::int64_t>(i) * per + k);
            b->add_reals(static_cast<double>(k) * 1.5);
        }
    }
    std::string out;
    bs.SerializeToString(&out);
    return out;
}

// Packed-scalar shapes for the varint-vs-fixed decode comparison: same element count and structure,
// one all-int64 (varint) and one all-double (fixed-width), so the two runs isolate the per-kind
// packed decode cost (a variable-length varint element vs a constant-width one).
std::string make_big_varint(int items, int per) {
    bench::BigSet bs;
    for (int i = 0; i < items; ++i) {
        bench::Big* b = bs.add_items();
        for (int k = 0; k < per; ++k) {
            b->add_numbers(static_cast<std::int64_t>(i) * per + k);
        }
    }
    std::string out;
    bs.SerializeToString(&out);
    return out;
}
std::string make_big_fixed(int items, int per) {
    bench::BigSet bs;
    for (int i = 0; i < items; ++i) {
        bench::Big* b = bs.add_items();
        for (int k = 0; k < per; ++k) {
            b->add_reals(static_cast<double>(k) * 1.5);
        }
    }
    std::string out;
    bs.SerializeToString(&out);
    return out;
}
// OSM-DenseNodes / vector-tile shape: repeated sub-messages, each carrying a packed sint64 DELTA run
// (small signed values -> zigzag encodes to 1-2 bytes, the narrow-mixed regime the Struct/Bulk kernels
// target) plus a packed small-enum run. Unlike the synthetic single-field/single-width `rv` sweep, this
// exercises the kernels the way real packed-heavy formats do: many messages, realistic span sizes, a
// delta+enum value MIX, and per-message framing overhead between the packed spans -- an honest witness
// for the packed-varint wins. Deterministic xorshift so the bytes are reproducible.
std::string make_big_osm(int items, int per) {
    bench::BigSet bs;
    std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
    const auto next = [&]() {
        rng ^= rng << 13U;
        rng ^= rng >> 7U;
        rng ^= rng << 17U;
        return rng;
    };
    for (int i = 0; i < items; ++i) {
        bench::Big* b = bs.add_items();
        for (int k = 0; k < per; ++k) {
            // 1-in-16 a larger jump (~3-byte zigzag), else a small delta (~1-2 byte zigzag).
            const std::int64_t delta = ((next() % 16U) == 0U)
                                           ? static_cast<std::int64_t>(next() % 200000U) - 100000
                                           : static_cast<std::int64_t>(next() % 256U) - 128;
            b->add_zz(delta);
            b->add_kinds((next() & 1U) != 0U ? bench::KIND_ONE : bench::KIND_UNSPECIFIED);
        }
    }
    std::string out;
    bs.SerializeToString(&out);
    return out;
}

// Fixed-size sub-messages, always present: each record carries two 24-byte Vec3s. At the default
// cutoff (16) a 24-byte Vec3 is boxed behind a pointer; recompiling with the cutoff >= 24 inlines it,
// which is how the cutoff was tuned (see the knob-tuning note at the top of this file).
std::string make_particles(int items) {
    bench::ParticleSet ps;
    for (int i = 0; i < items; ++i) {
        bench::Particle* p = ps.add_items();
        p->set_id(i);
        bench::Vec3* pos = p->mutable_position();
        pos->set_x(i * 1.0);
        pos->set_y(i * 2.0);
        pos->set_z(i * 3.0);
        bench::Vec3* vel = p->mutable_velocity();
        vel->set_x(i * 0.1);
        vel->set_y(i * 0.2);
        vel->set_z(i * 0.3);
        p->set_color(static_cast<std::uint32_t>(i));
    }
    std::string out;
    ps.SerializeToString(&out);
    return out;
}

// A RecordSet of scalar-heavy records (no strings/packed/maps), small values so per-field cost is
// dominated by tag dispatch. Every field is set, in ascending order (protoc serializes so).
std::string make_records(int count) {
    bench::RecordSet rs;
    const auto sample = [](bench::Sample* s, int base) {
        s->set_a(base & 63);
        s->set_b((base + 1) & 63);
        s->set_c((base + 2) & 63);
        s->set_d((base + 3) & 63);
        s->set_e((base & 1) != 0);
        s->set_f(static_cast<std::uint32_t>((base + 4) & 63));
    };
    for (int i = 0; i < count; ++i) {
        bench::Record* r = rs.add_records();
        r->set_f1(i & 63);
        r->set_f2((i + 1) & 63);
        r->set_f3((i + 2) & 63);
        r->set_f4((i + 3) & 63);
        r->set_f5(static_cast<std::uint32_t>((i + 4) & 63));
        r->set_f6((i + 5) & 63);
        r->set_f7((i & 1) != 0);
        r->set_f8(static_cast<std::uint64_t>((i + 6) & 63));
        r->set_f9((i + 7) & 63);
        r->set_f10((i + 8) & 63);
        r->set_f11((i + 9) & 63);
        r->set_f12((i + 10) & 63);
        r->set_f13((i + 11) & 63);
        r->set_f14((i + 12) & 63);
        r->set_f15((i + 13) & 63);
        sample(r->mutable_s1(), i + 20);
        sample(r->mutable_s2(), i + 30);
        r->set_f18((i + 14) & 63);
        r->set_f19((i + 15) & 63);
        r->set_f20((i + 16) & 63);
    }
    std::string out;
    rs.SerializeToString(&out);
    return out;
}

// ── checksums (must agree across decoders for Dataset; guard optimize-away for the sweep shapes) ───

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

std::uint64_t checksum_arena_dataset(const rp::bench::Dataset* d) {
    std::uint64_t s = static_cast<std::uint64_t>(d->version()) + d->name().size();
    for (const rp::bench::Person& p : d->people()) {
        s += static_cast<std::uint64_t>(p.id()) + p.name().size() + p.email().size() +
             (p.active() ? 1U : 0U) + bits(p.score()) + p.created();
        if (const rp::bench::Address* a = p.address()) {
            s += a->street().size() + a->city().size() + a->zip();
        }
        for (const std::string_view t : p.tags()) {
            s += t.size();
        }
        for (const std::int32_t h : p.history()) {
            s += static_cast<std::uint32_t>(h);
        }
        for (const rp::bench::Attribute& a : p.attributes()) {
            s += a.key().size() + a.value().size();
        }
        for (const auto& e : p.counters()) {
            s += e.key().size() + static_cast<std::uint32_t>(e.value());
        }
    }
    return s;
}

std::uint64_t checksum_arena_wide(const rp::bench::WideSet* w) {
    std::uint64_t s = 0;
    for (const rp::bench::Wide& it : w->items()) {
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
        for (const std::string_view x : it.s()) {
            s += x.size();
        }
        for (const std::string_view x : it.t()) {
            s += x.size();
        }
    }
    return s;
}

std::uint64_t checksum_arena_big(const rp::bench::BigSet* b) {
    std::uint64_t s = 0;
    for (const rp::bench::Big& it : b->items()) {
        for (const std::int64_t v : it.numbers()) {
            s += static_cast<std::uint64_t>(v);
        }
        for (const double r : it.reals()) {
            s += bits(r);
        }
        for (const std::int64_t v : it.zz()) {  // sint64 (zigzag) -- the OSM-delta shape
            s += static_cast<std::uint64_t>(v);
        }
        for (const rp::bench::Kind k : it.kinds()) {
            s += static_cast<std::uint64_t>(k);
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

std::uint64_t checksum_arena_particle(const rp::bench::ParticleSet* d) {
    std::uint64_t s = 0;
    for (const rp::bench::Particle& it : d->items()) {
        s += static_cast<std::uint64_t>(it.id()) + it.color();
        if (const rp::bench::Vec3* p = it.position()) {
            s += bits(p->x()) + bits(p->y()) + bits(p->z());
        }
        if (const rp::bench::Vec3* v = it.velocity()) {
            s += bits(v->x()) + bits(v->y()) + bits(v->z());
        }
    }
    return s;
}

std::uint64_t checksum_arena_record(const rp::bench::RecordSet* d) {
    if (d == nullptr) {
        return 0;
    }
    std::uint64_t s = 0;
    const auto sample = [&](const rp::bench::Sample* x) {
        if (x != nullptr) {
            s += static_cast<std::uint64_t>(x->a()) + static_cast<std::uint64_t>(x->b()) +
                 static_cast<std::uint64_t>(x->c()) + static_cast<std::uint64_t>(x->d()) +
                 (x->e() ? 1U : 0U) + x->f();
        }
    };
    for (const rp::bench::Record& r : d->records()) {
        s += static_cast<std::uint64_t>(r.f1()) + static_cast<std::uint64_t>(r.f2()) +
             static_cast<std::uint64_t>(r.f3()) + static_cast<std::uint64_t>(r.f4()) + r.f5() +
             static_cast<std::uint64_t>(r.f6()) + (r.f7() ? 1U : 0U) + r.f8() +
             static_cast<std::uint64_t>(r.f9()) + static_cast<std::uint64_t>(r.f10()) +
             static_cast<std::uint64_t>(r.f11()) + static_cast<std::uint64_t>(r.f12()) +
             static_cast<std::uint64_t>(r.f13()) + static_cast<std::uint64_t>(r.f14()) +
             static_cast<std::uint64_t>(r.f15()) + static_cast<std::uint64_t>(r.f18()) +
             static_cast<std::uint64_t>(r.f19()) + static_cast<std::uint64_t>(r.f20());
        sample(r.s1());
        sample(r.s2());
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

std::uint64_t checksum_stream(rapidproto::ByteView buf) {
    using namespace rp::bench::stream;
    std::uint64_t s = 0;
    const Dataset d{buf};
    const rapidproto::DecodeStatus st = d.decode(
        [&](Dataset::name, std::string_view v) { s += v.size(); },
        [&](Dataset::version, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
        [&](Dataset::people, Person p) -> rapidproto::DecodeStatus {
            return p.decode(
                [&](Person::id, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
                [&](Person::name, std::string_view v) { s += v.size(); },
                [&](Person::email, std::string_view v) { s += v.size(); },
                [&](Person::active, bool v) { s += v ? 1U : 0U; },
                [&](Person::score, double v) { s += bits(v); },
                [&](Person::created, std::uint64_t v) { s += v; },
                [&](Person::address, Address a) -> rapidproto::DecodeStatus {
                    return a.decode([&](Address::street, std::string_view v) { s += v.size(); },
                                    [&](Address::city, std::string_view v) { s += v.size(); },
                                    [&](Address::zip, std::uint32_t v) { s += v; });
                },
                [&](Person::tags, std::string_view v) { s += v.size(); },
                [&](Person::history, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Person::attributes, Attribute a) -> rapidproto::DecodeStatus {
                    return a.decode([&](Attribute::key, std::string_view v) { s += v.size(); },
                                    [&](Attribute::value, std::string_view v) { s += v.size(); });
                },
                [&](Person::counters, std::string_view k, std::int32_t v) {
                    s += k.size() + static_cast<std::uint32_t>(v);
                });
        });
    return st.ok() ? s : 0;
}

#ifdef RAPIDPROTO_HAVE_PROTOZERO
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
#endif

// ── timing ────────────────────────────────────────────────────────────────────────────────────────

volatile std::uint64_t g_sink = 0;

template <class F>
double best_ns(F parse_once, int reps, int inner) {
    for (int i = 0; i < 5; ++i) {  // warm up
        g_sink += parse_once();
    }
    double best = 1e18;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t acc = 0;
        for (int k = 0; k < inner; ++k) {
            acc += parse_once();
        }
        const auto t1 = std::chrono::steady_clock::now();
        g_sink += acc;
        const double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(inner);
        best = std::min(best, ns);
    }
    return best;
}

// Fewer inner iterations as the payload grows, so a 32 MB row still finishes quickly.
int inner_for(std::size_t bytes) {
    const auto n = static_cast<int>(100'000'000ULL / (bytes + 1));
    return std::clamp(n, 1, 80);
}

// One shape's sweep: build at each record count, parse once for memory, time cold + warm.
template <class Build, class ParseSum>
void sweep_shape(const char* name, Build build, ParseSum parse_sum,
                 const std::vector<int>& counts) {
    std::printf("\n%-20s %9s %9s %9s %7s %11s %11s\n", name, "wire(MB)", "used(MB)", "held(MB)",
                "held/use", "cold(us/op)", "warm(us/op)");
    for (const int n : counts) {
        const std::string buf = build(n);
        const rapidproto::ByteView view(buf);
        rapidproto::Arena mem;
        g_sink += parse_sum(view, mem);
        const std::size_t used = mem.bytes_used();
        const std::size_t held = mem.bytes_reserved();
        const int inner = inner_for(buf.size());
        const int reps = 5;
        const double cold = best_ns(
            [&]() {
                rapidproto::Arena a;
                return parse_sum(view, a);
            },
            reps, inner);
        rapidproto::Arena warm;
        const double warm_ns = best_ns(
            [&]() {
                warm.reset();
                return parse_sum(view, warm);
            },
            reps, inner);
        std::printf("%-20s %9.2f %9.2f %9.2f %7.3f %11.1f %11.1f\n", "", buf.size() / 1e6,
                    used / 1e6, held / 1e6, static_cast<double>(held) / static_cast<double>(used),
                    cold / 1000.0, warm_ns / 1000.0);
    }
}

// ── repeated-varint sweep ───────────────────────────────────────────────────────────────────────
// Bare-Big checksums (a message carrying only packed int64 `numbers`, field 1) for the sweep arms.
std::uint64_t checksum_big_arena(const rp::bench::Big* b) {
    std::uint64_t s = 0;
    if (b != nullptr) {
        for (const std::int64_t v : b->numbers()) {
            s += static_cast<std::uint64_t>(v);
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
#ifdef RAPIDPROTO_HAVE_PROTOZERO
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
#endif

// Same shape for the packed sint64 `zz` (field 3) and enum `kinds` (field 4) type-comparison arms: the
// wire is identical packed varints, so the only difference from `numbers` is the zigzag / enum-cast conv.
std::uint64_t checksum_big_arena_zz(const rp::bench::Big* b) {
    std::uint64_t s = 0;
    if (b != nullptr) {
        for (const std::int64_t v : b->zz()) {
            s += static_cast<std::uint64_t>(v);
        }
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
std::uint64_t checksum_big_arena_kinds(const rp::bench::Big* b) {
    std::uint64_t s = 0;
    if (b != nullptr) {
        for (const auto k : b->kinds()) {
            s += static_cast<std::uint64_t>(static_cast<std::int32_t>(k));
        }
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
#ifdef RAPIDPROTO_HAVE_PROTOZERO
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
#endif

// The packed int64 fill (rp::bench::Big.numbers) across element byte width (fixed 1..10, uniform, 90/10
// skew) x element count (10 .. 1,000,000): arena-warm vs protoc (and protozero as a raw-parse
// yardstick). The streaming bench sweeps the SAME shapes, so the two map the packed-varint decode
// surface for both decoders.
void sweep_repeated_varint() {
    for (const auto& dist : rpbench::varint_dists()) {
        for (const int count : rpbench::varint_lengths()) {
            // Build a POOL of distinct-seed buffers with the same shape and rotate over it, so no arm
            // replays a single memorized width sequence. Pool size: ~64 MB budget, >= 8 buffers (defeat
            // the predictor at small counts), <= 64 (bound setup). Each element is <= 10 bytes, so
            // count*10 over-estimates a buffer; that only makes the pool smaller, never unsafe.
            const long est = static_cast<long>(count) * 10 + 16;
            const int pool_n =
                static_cast<int>(std::min<long>(64, std::max<long>(8, (64L << 20) / est)));
            std::vector<std::string> pool;
            std::vector<rapidproto::ByteView> views;
            pool.reserve(static_cast<std::size_t>(pool_n));
            views.reserve(static_cast<std::size_t>(pool_n));
            double total_bytes = 0;
            for (int s = 0; s < pool_n; ++s) {
                pool.push_back(rpbench::make_packed_i64(
                    rpbench::varint_values(dist, count, static_cast<std::uint64_t>(s) + 1)));
                total_bytes += static_cast<double>(pool.back().size());
            }
            for (const auto& b : pool) {
                views.emplace_back(b);  // pool strings are stable (reserved, no realloc)
            }
            const double avg_bytes = total_bytes / static_cast<double>(pool_n);
            const std::string name = "rv " + dist.label + " " + rpbench::length_tag(count);
            rapidproto::Arena warm;
            // Each arm keeps its own rotation index; the harness calls every arm the same number of
            // times, so all indices stay in lockstep and the per-round checksum cross-check still holds.
            std::vector<rpbench::Arm> arms = {
                {"protoc",
                 [&, i = 0]() mutable {
                     const std::string& buf = pool[static_cast<std::size_t>(i++) % pool.size()];
                     google::protobuf::Arena pa;
                     auto* m = google::protobuf::Arena::CreateMessage<bench::Big>(&pa);
                     m->ParseFromString(buf);
                     return checksum_big_protoc(*m);
                 }},
                {"arena-warm",
                 [&, i = 0]() mutable {
                     const rapidproto::ByteView& view =
                         views[static_cast<std::size_t>(i++) % views.size()];
                     warm.reset();
                     return checksum_big_arena(rp::bench::Big::decode(view, warm));
                 }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
                {"protozero",
                 [&, i = 0]() mutable {
                     return checksum_big_protozero(
                         views[static_cast<std::size_t>(i++) % views.size()]);
                 }},
#endif
            };
            (void)rpbench::run(name.c_str(), avg_bytes, arms);
        }
    }
}

// Packed sint64 (`zz`, field 3) and packed enum (`kinds`, field 4) carry the SAME packed-varint wire as
// `numbers`, so they run the SAME classifier + kernels; only the per-element conversion differs (zigzag
// for sint64, an int32 cast for enum, vs ~identity for int64). This focused sweep decodes those two at
// 1M elements so the conv cost is directly comparable to the "rv <dist> 1M" int64 rows. sint64 covers
// every width; enum stays on the int32-safe narrow widths (a >4-byte value would truncate, and protoc
// vs our decoders could then disagree -- and real enums are small anyway).
void sweep_repeated_varint_types() {
    constexpr int kCount = 1'000'000;
    const auto run_type = [&](const rpbench::VarintDist& dist, int field_number, const char* tag,
                              auto arena_sum, auto protoc_sum, auto protozero_sum) {
        const long est = static_cast<long>(kCount) * 10 + 16;
        const int pool_n =
            static_cast<int>(std::min<long>(64, std::max<long>(8, (64L << 20) / est)));
        std::vector<std::string> pool;
        std::vector<rapidproto::ByteView> views;
        pool.reserve(static_cast<std::size_t>(pool_n));
        views.reserve(static_cast<std::size_t>(pool_n));
        double total_bytes = 0;
        for (int s = 0; s < pool_n; ++s) {
            pool.push_back(rpbench::make_packed_i64(
                rpbench::varint_values(dist, kCount, static_cast<std::uint64_t>(s) + 1),
                field_number));
            total_bytes += static_cast<double>(pool.back().size());
        }
        for (const auto& b : pool) {
            views.emplace_back(b);
        }
        const double avg_bytes = total_bytes / static_cast<double>(pool_n);
        const std::string name = std::string("rv-") + tag + " " + dist.label + " 1M";
        rapidproto::Arena warm;
        std::vector<rpbench::Arm> arms = {
            {"protoc",
             [&, i = 0]() mutable {
                 const std::string& buf = pool[static_cast<std::size_t>(i++) % pool.size()];
                 google::protobuf::Arena pa;
                 auto* m = google::protobuf::Arena::CreateMessage<bench::Big>(&pa);
                 m->ParseFromString(buf);
                 return protoc_sum(*m);
             }},
            {"arena-warm",
             [&, i = 0]() mutable {
                 const rapidproto::ByteView& view =
                     views[static_cast<std::size_t>(i++) % views.size()];
                 warm.reset();
                 return arena_sum(rp::bench::Big::decode(view, warm));
             }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
            {"protozero",
             [&, i = 0]() mutable {
                 return protozero_sum(views[static_cast<std::size_t>(i++) % views.size()]);
             }},
#endif
        };
        (void)rpbench::run(name.c_str(), avg_bytes, arms);
    };
#ifdef RAPIDPROTO_HAVE_PROTOZERO
    const auto zz_pz = checksum_big_protozero_zz;
    const auto kinds_pz = checksum_big_protozero_kinds;
#else
    const auto zz_pz = [](rapidproto::ByteView) { return std::uint64_t{0}; };
    const auto kinds_pz = [](rapidproto::ByteView) { return std::uint64_t{0}; };
#endif
    for (const auto& dist : rpbench::varint_dists()) {
        run_type(dist, 3, "zz", checksum_big_arena_zz, checksum_big_protoc_zz, zz_pz);
        const bool narrow = dist.label == "fx1" || dist.label == "fx2" || dist.label == "fx3" ||
                            dist.label == "mix12" || dist.label == "mix13";
        if (narrow) {
            run_type(dist, 4, "enum", checksum_big_arena_kinds, checksum_big_protoc_kinds,
                     kinds_pz);
        }
    }
}

}  // namespace

int main() {
    // The baseline's version is half a headline ratio's meaning; report it so a number is never
    // separated from the libprotobuf it was measured against. (The macro encodes MMmmmppp.)
    const std::string buf = make_dataset(kPeople);
    const rapidproto::ByteView view(buf);
    if (rpbench::json_mode()) {
        std::printf(
            R"({"rec":"meta","protobuf_version":"%d.%d.%d","dataset_people":%d,"wire_bytes":%zu})"
            "\n",
            GOOGLE_PROTOBUF_VERSION / 1000000, GOOGLE_PROTOBUF_VERSION / 1000 % 1000,
            GOOGLE_PROTOBUF_VERSION % 1000, kPeople, buf.size());
    } else {
        std::printf("baseline: libprotobuf %d.%d.%d\n", GOOGLE_PROTOBUF_VERSION / 1000000,
                    GOOGLE_PROTOBUF_VERSION / 1000 % 1000, GOOGLE_PROTOBUF_VERSION % 1000);
    }

    // Cross-check correctness first (Dataset, all three decoders).
    rapidproto::Arena setup_arena;
    const rp::bench::Dataset* adoc = rp::bench::Dataset::decode(view, setup_arena);
    google::protobuf::Arena setup_pa;
    auto* pdoc = google::protobuf::Arena::CreateMessage<bench::Dataset>(&setup_pa);
    pdoc->ParseFromString(buf);
    const std::uint64_t c_arena = checksum_arena_dataset(adoc);
    const std::uint64_t c_protoc = checksum_protoc(*pdoc);
    const std::uint64_t c_stream = checksum_stream(view);
    bool mismatch = c_arena != c_protoc || c_arena != c_stream;
#ifdef RAPIDPROTO_HAVE_PROTOZERO
    const std::uint64_t c_pz = checksum_protozero(view);
    mismatch = mismatch || c_arena != c_pz;
#endif
    if (mismatch) {
        std::fprintf(stderr, "CHECKSUM MISMATCH arena=%llu protoc=%llu stream=%llu\n",
                     static_cast<unsigned long long>(c_arena),
                     static_cast<unsigned long long>(c_protoc),
                     static_cast<unsigned long long>(c_stream));
        return 1;
    }

    // Headline parse-time comparison on the shared harness (bench_harness.hpp): frequency-invariant
    // cycles/byte + drift-invariant ratios vs the protoc baseline, with a significance verdict.
    // '+X%' = the decoder out-throughputs protoc (so +53% == 1.53x). protoc is arm 0 (the baseline);
    // arena is measured "cold" (fresh Arena) and "warm" (reset+reuse).
    rapidproto::Arena warm;
    if (!rpbench::json_mode()) {
        std::printf("bench: Dataset with %d people, wire = %zu bytes\n\n", kPeople, buf.size());
    }
    std::vector<rpbench::Arm> arms = {
        {"protoc",
         [&]() {
             google::protobuf::Arena pa;
             auto* m = google::protobuf::Arena::CreateMessage<bench::Dataset>(&pa);
             m->ParseFromString(buf);
             return checksum_protoc(*m);
         }},
        {"arena-cold",
         [&]() {
             rapidproto::Arena a;
             return checksum_arena_dataset(rp::bench::Dataset::decode(view, a));
         }},
        {"arena-warm",
         [&]() {
             warm.reset();
             return checksum_arena_dataset(rp::bench::Dataset::decode(view, warm));
         }},
        {"streamgen", [&]() { return checksum_stream(view); }},
    };
#ifdef RAPIDPROTO_HAVE_PROTOZERO
    arms.push_back({"protozero", [&]() { return checksum_protozero(view); }});
#endif
    (void)rpbench::run("Dataset", static_cast<double>(buf.size()), arms);

    rapidproto::Arena mem_arena;
    (void)rp::bench::Dataset::decode(view, mem_arena);
    google::protobuf::Arena mem_pa;
    auto* mp = google::protobuf::Arena::CreateMessage<bench::Dataset>(&mem_pa);
    mp->ParseFromString(buf);
    const std::size_t mem_a_used = mem_arena.bytes_used();
    const std::size_t mem_a_held = mem_arena.bytes_reserved();
    const auto mem_p_used = static_cast<std::size_t>(mem_pa.SpaceUsed());
    const auto mem_p_held = static_cast<std::size_t>(mem_pa.SpaceAllocated());

    if (rpbench::json_mode()) {
        std::printf(R"({"rec":"mem","shape":"Dataset","arena_used":%zu,"arena_held":%zu,)"
                    R"("protoc_used":%zu,"protoc_held":%zu})"
                    "\n",
                    mem_a_used, mem_a_held, mem_p_used, mem_p_held);
    } else {
        std::printf("\npeak memory of the two materializers (bytes, one parse, like-with-like):\n");
        std::printf("  used (payload):   arena %9zu  protoc %9zu  (%.2fx)\n", mem_a_used,
                    mem_p_used, static_cast<double>(mem_a_used) / static_cast<double>(mem_p_used));
        std::printf("  held (malloc'd):  arena %9zu  protoc %9zu  (%.2fx)\n", mem_a_held,
                    mem_p_held, static_cast<double>(mem_a_held) / static_cast<double>(mem_p_held));
        std::printf("checksum %llu (all agree)\n", static_cast<unsigned long long>(c_arena));
    }

    // Repeated / packed-array shapes: the axes that stress the arena's repeated-field path -- packed
    // varint vs fixed-width elements (same element count), and many-messages-few-elements vs
    // few-messages-many-elements (same element count). Arena (warm) against protoc; the arena cyc/B
    // across these runs is the signal for repeated-field decode work.
    if (!rpbench::json_mode()) {
        std::printf("\nrepeated / packed-array shapes (arena-warm vs protoc):\n");
    }
    const auto bench_bigset = [](const char* name, const std::string& b) {
        rapidproto::ByteView v(b);
        rapidproto::Arena w;
        std::vector<rpbench::Arm> a = {
            {"protoc",
             [&]() {
                 google::protobuf::Arena pa;
                 auto* m = google::protobuf::Arena::CreateMessage<bench::BigSet>(&pa);
                 m->ParseFromString(b);
                 return checksum_protoc_big(*m);
             }},
            {"arena-warm",
             [&]() {
                 w.reset();
                 return checksum_arena_big(rp::bench::BigSet::decode(v, w));
             }},
        };
        (void)rpbench::run(name, static_cast<double>(b.size()), a);
    };
    bench_bigset("packed int64(varint)", make_big_varint(200, 1000));  // 200k varint elements
    bench_bigset("packed double(fixed)", make_big_fixed(200, 1000));   // 200k fixed-width elements
    bench_bigset("osm: sint64 deltas + enum", make_big_osm(200, 1000));  // realistic packed witness
    bench_bigset("few msgs, big arrays", make_big(30, 10000));           // 30 msgs x 20k elements
    {
        const std::string wbuf = make_wide(50000);  // 50k msgs x tiny arrays -- same ~600k elements
        rapidproto::ByteView v(wbuf);
        rapidproto::Arena w;
        std::vector<rpbench::Arm> a = {
            {"protoc",
             [&]() {
                 google::protobuf::Arena pa;
                 auto* m = google::protobuf::Arena::CreateMessage<bench::WideSet>(&pa);
                 m->ParseFromString(wbuf);
                 return checksum_protoc_wide(*m);
             }},
            {"arena-warm",
             [&]() {
                 w.reset();
                 return checksum_arena_wide(rp::bench::WideSet::decode(v, w));
             }},
        };
        (void)rpbench::run("many msgs, tiny arrays", static_cast<double>(wbuf.size()), a);
    }
    {
        // Scalar-heavy records (no strings/packed/maps): decode time is dominated by per-field tag
        // dispatch, so this is the shape that isolates dispatch-shape wins (unlike the string/array-bound
        // shapes above). 20k records x ~34 bytes.
        const std::string rbuf = make_records(20000);
        rapidproto::ByteView v(rbuf);
        rapidproto::Arena w;
        std::vector<rpbench::Arm> a = {
            {"protoc",
             [&]() {
                 google::protobuf::Arena pa;
                 auto* m = google::protobuf::Arena::CreateMessage<bench::RecordSet>(&pa);
                 m->ParseFromString(rbuf);
                 return checksum_protoc_record(*m);
             }},
            {"arena-warm",
             [&]() {
                 w.reset();
                 return checksum_arena_record(rp::bench::RecordSet::decode(v, w));
             }},
        };
        (void)rpbench::run("scalar records (dispatch-bound)", static_cast<double>(rbuf.size()), a);
    }

    // Repeated-varint sweep: the packed int64 decode surface across element byte width x count. Part of
    // the machine-readable comparison (runs in JSON mode too), so it precedes the early return below.
    sweep_repeated_varint();
    // Packed sint64 / enum at 1M: same wire+kernels, measuring the zigzag / enum-cast conversion cost.
    sweep_repeated_varint_types();

    // Chunk-cap sweep: held/used (the arena's growth + chunk-tail waste) and parse time across shapes
    // and sizes up to ~32 MB. Arena only; this tunes the Arena's chunk-growth policy. It is deep
    // analysis, not part of the machine-readable comparison, so JSON mode skips it entirely.
    if (rpbench::json_mode()) {
        return 0;  // the sweep (which feeds g_sink) is skipped; harness checksums guard the arms
    }
    std::printf("\n=== arena chunk-cap sweep (held/use = growth + chunk-tail waste) ===\n");
    sweep_shape(
        "mixed (Dataset)", [](int n) { return make_dataset(n); },
        [](rapidproto::ByteView v, rapidproto::Arena& a) {
            const rp::bench::Dataset* d = rp::bench::Dataset::decode(v, a);
            return d != nullptr ? checksum_arena_dataset(d) : 0;
        },
        {2000, 22000, 88000, 176000});
    sweep_shape(
        "many-small (WideSet)", [](int n) { return make_wide(n); },
        [](rapidproto::ByteView v, rapidproto::Arena& a) {
            const rp::bench::WideSet* d = rp::bench::WideSet::decode(v, a);
            return d != nullptr ? checksum_arena_wide(d) : 0;
        },
        {6000, 62000, 250000, 520000});
    sweep_shape(
        "few-big (BigSet)", [](int n) { return make_big(n, 20000); },
        [](rapidproto::ByteView v, rapidproto::Arena& a) {
            const rp::bench::BigSet* d = rp::bench::BigSet::decode(v, a);
            return d != nullptr ? checksum_arena_big(d) : 0;
        },
        {2, 20, 80, 160});
    sweep_shape(
        "fixed-submsg (Particle)", [](int n) { return make_particles(n); },
        [](rapidproto::ByteView v, rapidproto::Arena& a) {
            const rp::bench::ParticleSet* d = rp::bench::ParticleSet::decode(v, a);
            return d != nullptr ? checksum_arena_particle(d) : 0;
        },
        {6000, 60000, 250000, 500000});

    return g_sink == 0 ? 2 : 0;  // touch the sink so nothing is optimized away
}
