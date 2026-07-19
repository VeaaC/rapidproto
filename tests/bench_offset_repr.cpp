// Isolated A/B for the "single chunk + offsets instead of pointers" experiment.
//
// The arena tree today stores 8-byte POINTERS: ArenaString {char[8] ptr, uint32 len} = 12 B,
// ArrayView/MapView {char[8] ptr, uint32 size} = 12 B, and a bare `const T*` = 8 B for any sub-message
// that is variable-size or over the 16-byte inline cutoff. If the input and the arena share ONE
// address range, each of those can hold a 40-bit SELF-RELATIVE offset instead -> 9 / 9 / 5 B.
//
// Self-relative (target - &self) rather than base-relative (target - base) so a dereference needs no
// base register -- `this + off` -- which is what lets the accessors keep their exact signatures. It
// also makes 0 a free null sentinel: an offset of 0 would point at the field itself, never valid.
//
// This benchmark answers ONE question before any codegen work: does shrinking those cells (fewer cache
// lines) beat the extra offset arithmetic (a sign-extended unaligned load + add per dereference)?
// It deliberately does NOT decode protobuf -- it models the node shapes and the two access patterns
// that matter, so the representation is the only variable.
//
// Both variants are built and measured in the SAME binary and interleaved across repetitions, because
// cross-build comparisons on this project carry a ~10% code-placement floor (see the bench notes in
// bench_arena.cpp). Ratios within one binary are the trustworthy number here.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kRecords = 200000;  // enough that the node array far exceeds L2
constexpr int kReps = 7;                  // odd; we report the median

// ── shared bump region ───────────────────────────────────────────────────────────────────────────
// One contiguous buffer holding the "input" bytes first and the "arena" nodes after, so a self-
// relative offset can reach from a node back into the input. The pointer variant uses the identical
// region, so allocation behaviour is held constant and only the CELL layout differs.
struct Region {
    std::vector<char> buf;
    std::size_t used = 0;
    explicit Region(std::size_t cap) : buf(cap) {}
    char* alloc(std::size_t n, std::size_t align) {
        used = (used + align - 1) & ~(align - 1);
        char* p = buf.data() + used;
        used += n;
        return p;
    }
};

// ── variant A: pointers (today's representation) ─────────────────────────────────────────────────
// Mirrors the real cells: the pointer lives in a char[8] read via memcpy so the cell stays 4-aligned
// (a real pointer member would pad 12 -> 16), exactly as ArenaString/ArrayView do today.
struct PtrStr {
    char ptr[8] = {};
    std::uint32_t len = 0;
    [[nodiscard]] const char* data() const noexcept {
        const char* p = nullptr;
        std::memcpy(&p, ptr, sizeof p);
        return p;
    }
};
struct PtrArr {
    char ptr[8] = {};
    std::uint32_t size = 0;
    [[nodiscard]] const std::int32_t* data() const noexcept {
        const std::int32_t* p = nullptr;
        std::memcpy(&p, ptr, sizeof p);
        return p;
    }
};
struct PtrSub {
    std::int64_t v = 0;
};
struct PtrRec {              // 12 + 12 + 12 + 8 + 8 = 52 -> padded 56
    PtrStr name;             // 12
    PtrStr email;            // 12
    PtrArr tags;             // 12
    const PtrSub* addr;      // 8
    std::int64_t id;         // 8
};

// ── variant B: 40-bit self-relative offsets ──────────────────────────────────────────────────────
// A 5-byte signed offset, read as an 8-byte unaligned load then arithmetic-shifted down. Reading 8
// bytes from a 5-byte field over-reads up to 3 bytes, which is safe because the region is padded --
// the real implementation would guarantee the same tail slack.
constexpr int kOffBits = 40;
constexpr int kShift = 64 - kOffBits;

inline void store_off(char* cell, const void* target, const void* self) noexcept {
    const auto d = static_cast<std::int64_t>(static_cast<const char*>(target) -
                                             static_cast<const char*>(self));
    std::memcpy(cell, &d, 5);
}
inline std::int64_t load_off(const char* cell) noexcept {
    std::int64_t raw = 0;
    std::memcpy(&raw, cell, sizeof raw);            // 8-byte load, 3 bytes of slack
    return (raw << kShift) >> kShift;               // sign-extend the low 40 bits
}

struct OffStr {                                     // 5 + 4 = 9
    char off[5] = {};
    std::uint32_t len = 0;
    [[nodiscard]] const char* data() const noexcept {
        const std::int64_t d = load_off(off);
        return d == 0 ? nullptr : reinterpret_cast<const char*>(this) + d;
    }
} __attribute__((packed));
struct OffArr {                                     // 5 + 4 = 9
    char off[5] = {};
    std::uint32_t size = 0;
    [[nodiscard]] const std::int32_t* data() const noexcept {
        const std::int64_t d = load_off(off);
        return d == 0 ? nullptr
                      : reinterpret_cast<const std::int32_t*>(
                            reinterpret_cast<const char*>(this) + d);
    }
} __attribute__((packed));
struct OffSub {
    std::int64_t v = 0;
};
struct OffRec {              // 9 + 9 + 9 + 5 + 8 = 40
    OffStr name;             // 9
    OffStr email;            // 9
    OffArr tags;             // 9
    char addr[5];            // 5  (self-relative, 0 == null)
    std::int64_t id;         // 8
    [[nodiscard]] const OffSub* addr_p() const noexcept {
        const std::int64_t d = load_off(addr);
        return d == 0 ? nullptr
                      : reinterpret_cast<const OffSub*>(reinterpret_cast<const char*>(addr) + d);
    }
} __attribute__((packed));

// ── variant C: 32-bit self-relative offsets, NATURALLY ALIGNED ───────────────────────────────────
// The 40-bit cell needs __attribute__((packed)), which makes the compiler treat every member as
// unaligned -- a penalty unrelated to the offset idea. A 32-bit offset sidesteps it entirely:
// {int32, uint32} is 8 bytes at align 4, so the cell is SMALLER than the 40-bit one (12->8 vs 12->9)
// and every load is a single aligned instruction. Cost: the whole chunk (input + arena) must fit 2 GiB
// of self-relative reach.
struct Off32Str {                                   // 8, align 4
    std::int32_t off = 0;
    std::uint32_t len = 0;
    [[nodiscard]] const char* data() const noexcept {
        return off == 0 ? nullptr : reinterpret_cast<const char*>(this) + off;
    }
};
struct Off32Arr {                                   // 8, align 4
    std::int32_t off = 0;
    std::uint32_t size = 0;
    [[nodiscard]] const std::int32_t* data() const noexcept {
        return off == 0 ? nullptr
                        : reinterpret_cast<const std::int32_t*>(
                              reinterpret_cast<const char*>(this) + off);
    }
};
struct Off32Rec {            // 8 + 8 + 8 + 4 + 8 = 36 -> 40
    Off32Str name;
    Off32Str email;
    Off32Arr tags;
    std::int32_t addr = 0;   // self-relative, 0 == null
    std::int64_t id = 0;
    [[nodiscard]] const OffSub* addr_p() const noexcept {
        return addr == 0 ? nullptr
                         : reinterpret_cast<const OffSub*>(
                               reinterpret_cast<const char*>(&addr) + addr);
    }
};

// ── variant E: 40-bit UNSIGNED offsets, cells packed but the record left naturally aligned ───────
// Two corrections to variant B. (1) Only the 9-byte CELLS are packed; the record itself is not, so
// scalars keep their natural alignment (9+9+9+5 = 32, so the int64 lands 8-aligned). Blanket-packing
// the record was what made variant B look bad -- that penalty is not inherent to a 40-bit offset.
// (2) Offsets are UNSIGNED with the direction implied by the field kind: message/array/map refs point
// FORWARD (a parent is allocated before its children; a grown array reallocates further forward),
// strings/bytes point BACKWARD into the input region. Unsigned costs one AND instead of the two
// shifts sign-extension needs, and doubles reach: 40 bits = 1 TiB, so a 2 GiB input expanding into a
// much larger arena still fits, which 32 bits cannot promise.
constexpr std::uint64_t kOff40Mask = (std::uint64_t{1} << 40) - 1;

inline void store_u40(char* cell, std::uint64_t v) noexcept { std::memcpy(cell, &v, 5); }
inline std::uint64_t load_u40(const char* cell) noexcept {
    std::uint64_t raw = 0;
    std::memcpy(&raw, cell, sizeof raw);  // 8-byte load, 3 bytes of tail slack
    return raw & kOff40Mask;              // unsigned -> one mask, no sign-extension
}

struct __attribute__((packed)) U40Str {  // 9: backward offset into the input
    char off[5] = {};
    std::uint32_t len = 0;
    [[nodiscard]] const char* data() const noexcept {
        const std::uint64_t d = load_u40(off);
        return d == 0 ? nullptr : reinterpret_cast<const char*>(this) - d;
    }
};
struct __attribute__((packed)) U40Arr {  // 9: forward offset into the arena
    char off[5] = {};
    std::uint32_t size = 0;
    [[nodiscard]] const std::int32_t* data() const noexcept {
        const std::uint64_t d = load_u40(off);
        return d == 0 ? nullptr
                      : reinterpret_cast<const std::int32_t*>(
                            reinterpret_cast<const char*>(this) + d);
    }
};
struct U40Rec {              // 9 + 9 + 9 + 5 + 8 = 40; int64 at offset 32 stays 8-aligned
    U40Str name;
    U40Str email;
    U40Arr tags;
    char addr[5] = {};       // forward, 0 == null
    std::int64_t id = 0;
    [[nodiscard]] const OffSub* addr_p() const noexcept {
        const std::uint64_t d = load_u40(addr);
        return d == 0 ? nullptr
                      : reinterpret_cast<const OffSub*>(reinterpret_cast<const char*>(addr) + d);
    }
};

// ── payload ──────────────────────────────────────────────────────────────────────────────────────
struct Source {
    std::vector<std::string> names, emails;
    std::vector<std::vector<std::int32_t>> tags;
};

Source make_source() {
    std::mt19937 rng(12345);
    Source s;
    s.names.reserve(kRecords);
    s.emails.reserve(kRecords);
    s.tags.reserve(kRecords);
    for (std::size_t i = 0; i < kRecords; ++i) {
        s.names.push_back("name_" + std::to_string(rng() % 100000));
        s.emails.push_back("user" + std::to_string(rng() % 100000) + "@example.com");
        std::vector<std::int32_t> t(1 + (rng() % 6));
        for (auto& x : t) x = static_cast<std::int32_t>(rng());
        s.tags.push_back(std::move(t));
    }
    return s;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
    const Source src = make_source();
    // Region big enough for input bytes + nodes + arrays, with tail slack for the 8-byte over-read.
    std::size_t cap = 1 << 20;
    for (std::size_t i = 0; i < kRecords; ++i) {
        cap += src.names[i].size() + src.emails[i].size() + src.tags[i].size() * 4 + 128;
    }

    double build_ptr = 1e18, build_off = 1e18, walk_ptr = 1e18, walk_off = 1e18;
    double rand_ptr = 1e18, rand_off = 1e18;
    double build_o32 = 1e18, walk_o32 = 1e18, rand_o32 = 1e18;
    std::size_t used_o32 = 0;
    double build_u40 = 1e18, walk_u40 = 1e18, rand_u40 = 1e18;
    std::size_t used_u40 = 0;
    std::size_t used_ptr = 0, used_off = 0;
    std::int64_t sink = 0;

    // A shuffled visit order: sequential walks are prefetcher-friendly, so they under-report any
    // cache-density win. Random order is latency-bound -- the case smaller nodes should actually help.
    std::vector<std::uint32_t> order(kRecords);
    for (std::size_t i = 0; i < kRecords; ++i) order[i] = static_cast<std::uint32_t>(i);
    std::shuffle(order.begin(), order.end(), std::mt19937(999));

    for (int rep = 0; rep < kReps; ++rep) {
        // ---- variant A: pointers ----
        {
            Region r(cap);
            auto t0 = std::chrono::steady_clock::now();
            auto* recs = reinterpret_cast<PtrRec*>(r.alloc(sizeof(PtrRec) * kRecords, 8));
            for (std::size_t i = 0; i < kRecords; ++i) {
                PtrRec& rec = recs[i];
                char* n = r.alloc(src.names[i].size(), 1);
                std::memcpy(n, src.names[i].data(), src.names[i].size());
                std::memcpy(rec.name.ptr, &n, sizeof n);
                rec.name.len = static_cast<std::uint32_t>(src.names[i].size());
                char* e = r.alloc(src.emails[i].size(), 1);
                std::memcpy(e, src.emails[i].data(), src.emails[i].size());
                std::memcpy(rec.email.ptr, &e, sizeof e);
                rec.email.len = static_cast<std::uint32_t>(src.emails[i].size());
                const auto tn = src.tags[i].size();
                char* a = r.alloc(tn * 4, 4);
                std::memcpy(a, src.tags[i].data(), tn * 4);
                std::memcpy(rec.tags.ptr, &a, sizeof a);
                rec.tags.size = static_cast<std::uint32_t>(tn);
                auto* sub = reinterpret_cast<PtrSub*>(r.alloc(sizeof(PtrSub), 8));
                sub->v = static_cast<std::int64_t>(i);
                rec.addr = sub;
                rec.id = static_cast<std::int64_t>(i);
            }
            const double bt = ms_since(t0);
            used_ptr = r.used;
            auto t1 = std::chrono::steady_clock::now();
            std::int64_t acc = 0;
            for (std::size_t i = 0; i < kRecords; ++i) {
                const PtrRec& rec = recs[i];
                acc += rec.name.len + rec.email.len + rec.id;
                acc += static_cast<unsigned char>(rec.name.data()[0]);
                acc += static_cast<unsigned char>(rec.email.data()[0]);
                const std::int32_t* t = rec.tags.data();
                for (std::uint32_t k = 0; k < rec.tags.size; ++k) acc += t[k];
                acc += rec.addr->v;
            }
            const double wt = ms_since(t1);
            sink += acc;
            auto t2 = std::chrono::steady_clock::now();
            std::int64_t racc = 0;
            for (std::size_t j = 0; j < kRecords; ++j) {
                const PtrRec& rec = recs[order[j]];
                racc += rec.name.len + rec.email.len + rec.id;
                racc += static_cast<unsigned char>(rec.name.data()[0]);
                racc += static_cast<unsigned char>(rec.email.data()[0]);
                racc += rec.tags.data()[0] + rec.addr->v;
            }
            const double rt = ms_since(t2);
            sink += racc;
            if (bt < build_ptr) build_ptr = bt;
            if (wt < walk_ptr) walk_ptr = wt;
            if (rt < rand_ptr) rand_ptr = rt;
        }
        // ---- variant B: 40-bit self-relative offsets ----
        {
            Region r(cap);
            auto t0 = std::chrono::steady_clock::now();
            auto* recs = reinterpret_cast<OffRec*>(r.alloc(sizeof(OffRec) * kRecords, 8));
            for (std::size_t i = 0; i < kRecords; ++i) {
                OffRec& rec = recs[i];
                char* n = r.alloc(src.names[i].size(), 1);
                std::memcpy(n, src.names[i].data(), src.names[i].size());
                store_off(rec.name.off, n, &rec.name);
                rec.name.len = static_cast<std::uint32_t>(src.names[i].size());
                char* e = r.alloc(src.emails[i].size(), 1);
                std::memcpy(e, src.emails[i].data(), src.emails[i].size());
                store_off(rec.email.off, e, &rec.email);
                rec.email.len = static_cast<std::uint32_t>(src.emails[i].size());
                const auto tn = src.tags[i].size();
                char* a = r.alloc(tn * 4, 4);
                std::memcpy(a, src.tags[i].data(), tn * 4);
                store_off(rec.tags.off, a, &rec.tags);
                rec.tags.size = static_cast<std::uint32_t>(tn);
                auto* sub = reinterpret_cast<OffSub*>(r.alloc(sizeof(OffSub), 8));
                sub->v = static_cast<std::int64_t>(i);
                store_off(rec.addr, sub, rec.addr);
                rec.id = static_cast<std::int64_t>(i);
            }
            const double bt = ms_since(t0);
            used_off = r.used;
            auto t1 = std::chrono::steady_clock::now();
            std::int64_t acc = 0;
            for (std::size_t i = 0; i < kRecords; ++i) {
                const OffRec& rec = recs[i];
                acc += rec.name.len + rec.email.len + rec.id;
                acc += static_cast<unsigned char>(rec.name.data()[0]);
                acc += static_cast<unsigned char>(rec.email.data()[0]);
                const std::int32_t* t = rec.tags.data();
                for (std::uint32_t k = 0; k < rec.tags.size; ++k) acc += t[k];
                acc += rec.addr_p()->v;
            }
            const double wt = ms_since(t1);
            sink += acc;
            auto t2 = std::chrono::steady_clock::now();
            std::int64_t racc = 0;
            for (std::size_t j = 0; j < kRecords; ++j) {
                const OffRec& rec = recs[order[j]];
                racc += rec.name.len + rec.email.len + rec.id;
                racc += static_cast<unsigned char>(rec.name.data()[0]);
                racc += static_cast<unsigned char>(rec.email.data()[0]);
                racc += rec.tags.data()[0] + rec.addr_p()->v;
            }
            const double rt = ms_since(t2);
            sink += racc;
            if (bt < build_off) build_off = bt;
            if (wt < walk_off) walk_off = wt;
            if (rt < rand_off) rand_off = rt;
        }
        // ---- variant C: 32-bit self-relative offsets, naturally aligned ----
        {
            Region r(cap);
            auto t0 = std::chrono::steady_clock::now();
            auto* recs = reinterpret_cast<Off32Rec*>(r.alloc(sizeof(Off32Rec) * kRecords, 8));
            for (std::size_t i = 0; i < kRecords; ++i) {
                Off32Rec& rec = recs[i];
                char* n = r.alloc(src.names[i].size(), 1);
                std::memcpy(n, src.names[i].data(), src.names[i].size());
                rec.name.off = static_cast<std::int32_t>(n - reinterpret_cast<char*>(&rec.name));
                rec.name.len = static_cast<std::uint32_t>(src.names[i].size());
                char* e = r.alloc(src.emails[i].size(), 1);
                std::memcpy(e, src.emails[i].data(), src.emails[i].size());
                rec.email.off = static_cast<std::int32_t>(e - reinterpret_cast<char*>(&rec.email));
                rec.email.len = static_cast<std::uint32_t>(src.emails[i].size());
                const auto tn = src.tags[i].size();
                char* a = r.alloc(tn * 4, 4);
                std::memcpy(a, src.tags[i].data(), tn * 4);
                rec.tags.off = static_cast<std::int32_t>(a - reinterpret_cast<char*>(&rec.tags));
                rec.tags.size = static_cast<std::uint32_t>(tn);
                auto* sub = reinterpret_cast<OffSub*>(r.alloc(sizeof(OffSub), 8));
                sub->v = static_cast<std::int64_t>(i);
                rec.addr = static_cast<std::int32_t>(reinterpret_cast<char*>(sub) -
                                                    reinterpret_cast<char*>(&rec.addr));
                rec.id = static_cast<std::int64_t>(i);
            }
            const double bt = ms_since(t0);
            used_o32 = r.used;
            auto t1 = std::chrono::steady_clock::now();
            std::int64_t acc = 0;
            for (std::size_t i = 0; i < kRecords; ++i) {
                const Off32Rec& rec = recs[i];
                acc += rec.name.len + rec.email.len + rec.id;
                acc += static_cast<unsigned char>(rec.name.data()[0]);
                acc += static_cast<unsigned char>(rec.email.data()[0]);
                const std::int32_t* t = rec.tags.data();
                for (std::uint32_t k = 0; k < rec.tags.size; ++k) acc += t[k];
                acc += rec.addr_p()->v;
            }
            const double wt = ms_since(t1);
            sink += acc;
            auto t2 = std::chrono::steady_clock::now();
            std::int64_t racc = 0;
            for (std::size_t j = 0; j < kRecords; ++j) {
                const Off32Rec& rec = recs[order[j]];
                racc += rec.name.len + rec.email.len + rec.id;
                racc += static_cast<unsigned char>(rec.name.data()[0]);
                racc += static_cast<unsigned char>(rec.email.data()[0]);
                racc += rec.tags.data()[0] + rec.addr_p()->v;
            }
            const double rt = ms_since(t2);
            sink += racc;
            if (bt < build_o32) build_o32 = bt;
            if (wt < walk_o32) walk_o32 = wt;
            if (rt < rand_o32) rand_o32 = rt;
        }
        // ---- variant E: 40-bit unsigned, cells packed / record aligned ----
        {
            Region r(cap);
            auto t0 = std::chrono::steady_clock::now();
            // Reserve the "input" region FIRST so string cells genuinely point backward.
            std::size_t input_bytes = 0;
            for (std::size_t i = 0; i < kRecords; ++i) {
                input_bytes += src.names[i].size() + src.emails[i].size();
            }
            char* const input = r.alloc(input_bytes, 1);
            char* cur = input;
            auto* recs = reinterpret_cast<U40Rec*>(r.alloc(sizeof(U40Rec) * kRecords, 8));
            for (std::size_t i = 0; i < kRecords; ++i) {
                U40Rec& rec = recs[i];
                char* n = cur;
                std::memcpy(n, src.names[i].data(), src.names[i].size());
                cur += src.names[i].size();
                store_u40(rec.name.off,
                          static_cast<std::uint64_t>(reinterpret_cast<char*>(&rec.name) - n));
                rec.name.len = static_cast<std::uint32_t>(src.names[i].size());
                char* e = cur;
                std::memcpy(e, src.emails[i].data(), src.emails[i].size());
                cur += src.emails[i].size();
                store_u40(rec.email.off,
                          static_cast<std::uint64_t>(reinterpret_cast<char*>(&rec.email) - e));
                rec.email.len = static_cast<std::uint32_t>(src.emails[i].size());
                const auto tn = src.tags[i].size();
                char* a = r.alloc(tn * 4, 4);
                std::memcpy(a, src.tags[i].data(), tn * 4);
                store_u40(rec.tags.off,
                          static_cast<std::uint64_t>(a - reinterpret_cast<char*>(&rec.tags)));
                rec.tags.size = static_cast<std::uint32_t>(tn);
                auto* sub = reinterpret_cast<OffSub*>(r.alloc(sizeof(OffSub), 8));
                sub->v = static_cast<std::int64_t>(i);
                store_u40(rec.addr, static_cast<std::uint64_t>(reinterpret_cast<char*>(sub) -
                                                              rec.addr));
                rec.id = static_cast<std::int64_t>(i);
            }
            const double bt = ms_since(t0);
            used_u40 = r.used;
            auto t1 = std::chrono::steady_clock::now();
            std::int64_t acc = 0;
            for (std::size_t i = 0; i < kRecords; ++i) {
                const U40Rec& rec = recs[i];
                acc += rec.name.len + rec.email.len + rec.id;
                acc += static_cast<unsigned char>(rec.name.data()[0]);
                acc += static_cast<unsigned char>(rec.email.data()[0]);
                const std::int32_t* t = rec.tags.data();
                for (std::uint32_t k = 0; k < rec.tags.size; ++k) acc += t[k];
                acc += rec.addr_p()->v;
            }
            const double wt = ms_since(t1);
            sink += acc;
            auto t2 = std::chrono::steady_clock::now();
            std::int64_t racc = 0;
            for (std::size_t j = 0; j < kRecords; ++j) {
                const U40Rec& rec = recs[order[j]];
                racc += rec.name.len + rec.email.len + rec.id;
                racc += static_cast<unsigned char>(rec.name.data()[0]);
                racc += static_cast<unsigned char>(rec.email.data()[0]);
                racc += rec.tags.data()[0] + rec.addr_p()->v;
            }
            const double rt = ms_since(t2);
            sink += racc;
            if (bt < build_u40) build_u40 = bt;
            if (wt < walk_u40) walk_u40 = wt;
            if (rt < rand_u40) rand_u40 = rt;
        }
    }

    std::printf("records            %zu\n", kRecords);
    std::printf("sizeof record      ptr=%zu  off=%zu   (%.1f%% smaller)\n", sizeof(PtrRec),
                sizeof(OffRec), 100.0 * (1.0 - double(sizeof(OffRec)) / double(sizeof(PtrRec))));
    std::printf("region used        ptr=%.2f MB  off=%.2f MB   (%.1f%% smaller)\n",
                double(used_ptr) / 1e6, double(used_off) / 1e6,
                100.0 * (1.0 - double(used_off) / double(used_ptr)));
    std::printf("build  (best of %d) ptr=%.2f ms  off=%.2f ms   off/ptr=%.3f\n", kReps, build_ptr,
                build_off, build_off / build_ptr);
    std::printf("walk   (best of %d) ptr=%.2f ms  off=%.2f ms   off/ptr=%.3f\n", kReps, walk_ptr,
                walk_off, walk_off / walk_ptr);
    std::printf("random (best of %d) ptr=%.2f ms  off=%.2f ms   off/ptr=%.3f\n", kReps, rand_ptr,
                rand_off, rand_off / rand_ptr);
    std::printf("\n-- 32-bit aligned offsets (variant C) --\n");
    std::printf("sizeof record      ptr=%zu  off32=%zu   (%.1f%% smaller)\n", sizeof(PtrRec),
                sizeof(Off32Rec), 100.0 * (1.0 - double(sizeof(Off32Rec)) / double(sizeof(PtrRec))));
    std::printf("region used        ptr=%.2f MB  off32=%.2f MB   (%.1f%% smaller)\n",
                double(used_ptr) / 1e6, double(used_o32) / 1e6,
                100.0 * (1.0 - double(used_o32) / double(used_ptr)));
    std::printf("build              ptr=%.2f ms  off32=%.2f ms   off32/ptr=%.3f\n", build_ptr,
                build_o32, build_o32 / build_ptr);
    std::printf("walk               ptr=%.2f ms  off32=%.2f ms   off32/ptr=%.3f\n", walk_ptr,
                walk_o32, walk_o32 / walk_ptr);
    std::printf("random             ptr=%.2f ms  off32=%.2f ms   off32/ptr=%.3f\n", rand_ptr,
                rand_o32, rand_o32 / rand_ptr);
    std::printf("\n-- 40-bit UNSIGNED, cells packed / record aligned (variant E) --\n");
    std::printf("sizeof record      ptr=%zu  u40=%zu   (%.1f%% smaller)\n", sizeof(PtrRec),
                sizeof(U40Rec), 100.0 * (1.0 - double(sizeof(U40Rec)) / double(sizeof(PtrRec))));
    std::printf("region used        ptr=%.2f MB  u40=%.2f MB   (%.1f%% smaller)\n",
                double(used_ptr) / 1e6, double(used_u40) / 1e6,
                100.0 * (1.0 - double(used_u40) / double(used_ptr)));
    std::printf("build              ptr=%.2f ms  u40=%.2f ms   u40/ptr=%.3f\n", build_ptr,
                build_u40, build_u40 / build_ptr);
    std::printf("walk               ptr=%.2f ms  u40=%.2f ms   u40/ptr=%.3f\n", walk_ptr, walk_u40,
                walk_u40 / walk_ptr);
    std::printf("random             ptr=%.2f ms  u40=%.2f ms   u40/ptr=%.3f\n", rand_ptr, rand_u40,
                rand_u40 / rand_ptr);
    std::printf("checksum %lld\n", static_cast<long long>(sink));
    return 0;
}
