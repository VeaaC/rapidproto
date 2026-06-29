// Arena benchmark: a standalone -O3 -DNDEBUG executable (layout-stable, like rapidproto_bench).
// Two parts:
//   1. A realistic bench::Dataset (2000 records) decoded three ways and compared apples-to-apples:
//        arena:  the arena decoder materializing a tree in a bump Arena (under test)
//        protoc: protoc-generated C++ + google::protobuf::Arena (the baseline)
//        stream: the streaming callback decoder (a zero-materialization yardstick)
//      Reports parse TIME (best-of-N; arena measured "cold" = fresh arena and "warm" = reset+reuse)
//      and peak MEMORY like-with-like: payload (arena bytes_used vs protoc SpaceUsed) and total-
//      malloc'd (arena bytes_reserved vs protoc SpaceAllocated). All three must agree on a checksum.
//   2. A chunk-cap sweep (arena only): three payload shapes (a mixed Dataset, a many-small-arrays
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
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

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
        if (const auto a = p.address()) {  // MessageRef: truthy when present, deref like a pointer
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
    }
    return s;
}

std::uint64_t checksum_arena_particle(const rp::bench::ParticleSet* d) {
    std::uint64_t s = 0;
    for (const rp::bench::Particle& it : d->items()) {
        s += static_cast<std::uint64_t>(it.id()) + it.color();
        if (const auto p =
                it.position()) {  // MessageRef: truthy when present, deref like a pointer
            s += bits(p->x()) + bits(p->y()) + bits(p->z());
        }
        if (const auto v = it.velocity()) {
            s += bits(v->x()) + bits(v->y()) + bits(v->z());
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

}  // namespace

int main() {
    const std::string buf = make_dataset(kPeople);
    const rapidproto::ByteView view(buf);

    // Cross-check correctness first (Dataset, all three decoders).
    rapidproto::Arena setup_arena;
    const rp::bench::Dataset* adoc = rp::bench::Dataset::decode(view, setup_arena);
    google::protobuf::Arena setup_pa;
    auto* pdoc = google::protobuf::Arena::CreateMessage<bench::Dataset>(&setup_pa);
    pdoc->ParseFromString(buf);
    const std::uint64_t c_arena = checksum_arena_dataset(adoc);
    const std::uint64_t c_protoc = checksum_protoc(*pdoc);
    const std::uint64_t c_stream = checksum_stream(view);
    if (c_arena != c_protoc || c_arena != c_stream) {
        std::fprintf(stderr, "CHECKSUM MISMATCH arena=%llu protoc=%llu stream=%llu\n",
                     static_cast<unsigned long long>(c_arena),
                     static_cast<unsigned long long>(c_protoc),
                     static_cast<unsigned long long>(c_stream));
        return 1;
    }

    constexpr int kReps = 50;
    constexpr int kInner = 200;
    const double t_arena_cold = best_ns(
        [&]() {
            rapidproto::Arena a;
            return checksum_arena_dataset(rp::bench::Dataset::decode(view, a));
        },
        kReps, kInner);
    rapidproto::Arena warm;
    const double t_arena_warm = best_ns(
        [&]() {
            warm.reset();
            return checksum_arena_dataset(rp::bench::Dataset::decode(view, warm));
        },
        kReps, kInner);
    const double t_protoc = best_ns(
        [&]() {
            google::protobuf::Arena pa;
            auto* m = google::protobuf::Arena::CreateMessage<bench::Dataset>(&pa);
            m->ParseFromString(buf);
            return checksum_protoc(*m);
        },
        kReps, kInner);
    const double t_stream = best_ns([&]() { return checksum_stream(view); }, kReps, kInner);

    rapidproto::Arena mem_arena;
    (void)rp::bench::Dataset::decode(view, mem_arena);
    google::protobuf::Arena mem_pa;
    auto* mp = google::protobuf::Arena::CreateMessage<bench::Dataset>(&mem_pa);
    mp->ParseFromString(buf);
    const std::size_t mem_a_used = mem_arena.bytes_used();
    const std::size_t mem_a_held = mem_arena.bytes_reserved();
    const auto mem_p_used = static_cast<std::size_t>(mem_pa.SpaceUsed());
    const auto mem_p_held = static_cast<std::size_t>(mem_pa.SpaceAllocated());

    std::printf("bench: Dataset with %d people, wire = %zu bytes\n", kPeople, buf.size());
    std::printf("\nparse time (ns/op, best of %d):\n", kReps);
    std::printf("  arena (cold)  %9.0f  (%.2fx protoc)\n", t_arena_cold, t_arena_cold / t_protoc);
    std::printf("  arena (warm)  %9.0f  (%.2fx protoc)\n", t_arena_warm, t_arena_warm / t_protoc);
    std::printf("  protoc+Arena  %9.0f  (1.00x)\n", t_protoc);
    std::printf("  streamgen     %9.0f  (%.2fx protoc)\n", t_stream, t_stream / t_protoc);
    std::printf("\npeak memory (bytes, one parse; each metric compares like-with-like):\n");
    std::printf("  used (payload):   arena %9zu  protoc %9zu  (%.2fx)\n", mem_a_used, mem_p_used,
                static_cast<double>(mem_a_used) / static_cast<double>(mem_p_used));
    std::printf("  held (malloc'd):  arena %9zu  protoc %9zu  (%.2fx)\n", mem_a_held, mem_p_held,
                static_cast<double>(mem_a_held) / static_cast<double>(mem_p_held));
    std::printf("checksum %llu (all agree)\n", static_cast<unsigned long long>(c_arena));

    // Chunk-cap sweep: held/used (the arena's growth + chunk-tail waste) and parse time across shapes
    // and sizes up to ~32 MB. Arena only; this tunes the Arena's chunk-growth policy.
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
