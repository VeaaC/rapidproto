// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// Unit coverage for the packed-varint SWAR kernel (rapidproto::wire::decode_packed_varints), which is
// ARENA-ONLY (the streaming decoder decodes packed fields per-element -- see the README Benchmarks). The
// rest of the suite only ever feeds these spans well under the kernel's 256-byte engagement threshold --
// the largest packed fixture is ~93 bytes -- so the classifier, Bulk1/Bulk2, Fixed<3..10>, Struct, Swar,
// and the re-classify path were entirely unexercised. These tests hit each kernel above threshold, the
// 256-byte gate, the P0 merged-varint shape, and the malformed paths, checking decoded VALUES against the
// known inputs (the kernel is the code under test; the inputs are the oracle). Exercised through the two
// arena entry points that ship (arena_detail::decode_packed_varints_large -> the kernel; _small -> the
// byte-loop tail) plus the kernel directly.

#include <catch_amalgamated.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

// An int64 whose protobuf varint encoding is EXACTLY `width` bytes (1..10). Widths 1..9 are positive
// values in [2^(7*(width-1)), 2^(7*width)-1]; width 10 needs the 64th bit (a negative int64, sign-extended
// to 10 bytes). `salt` varies the value inside the band. (Local copy so this strict -Werror TU need not
// pull in the non-strict bench_varint.hpp.)
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): width and salt are unrelated by name and use.
std::int64_t varint_of_width(int width, std::uint64_t salt) {
    if (width <= 1) {
        return static_cast<std::int64_t>(salt % 128U);
    }
    if (width >= 10) {
        return -1 - static_cast<std::int64_t>(salt % 128U);
    }
    const std::uint64_t lo = std::uint64_t{1} << (7U * static_cast<unsigned>(width - 1));
    return static_cast<std::int64_t>(lo + (salt % lo));
}

void put_varint(std::vector<std::uint8_t>& b, std::uint64_t v) {
    while (v >= 0x80U) {
        b.push_back(static_cast<std::uint8_t>(0x80U | (v & 0x7FU)));
        v >>= 7U;
    }
    b.push_back(static_cast<std::uint8_t>(v));
}

// Raw packed-varint payload (no message framing) for the given int64 values.
std::vector<std::uint8_t> encode_packed(const std::vector<std::int64_t>& vals) {
    std::vector<std::uint8_t> b;
    for (const std::int64_t v : vals) {
        put_varint(b, static_cast<std::uint64_t>(v));
    }
    return b;
}

// Decode a raw packed span three ways and require each reproduces `expect` exactly:
//   (1) wire::decode_packed_varints          -- the kernel directly (array_sink)
//   (2) arena_detail::decode_packed_varints_small -- the arena small-span byte-loop tail
//   (3) arena_detail::decode_packed_varints_large -- the arena large-span entry (-> the kernel)
void require_all_paths_decode(const std::vector<std::uint8_t>& bytes,
                              const std::vector<std::int64_t>& expect) {
    const std::uint8_t* const p = bytes.data();
    const std::uint8_t* const end = p + bytes.size();

    // (1) kernel directly via array_sink
    {
        std::vector<std::int64_t> out(expect.size() + 8, 0);
        WireError we = WireError::None;
        std::size_t fo = 0;
        const std::size_t n = wire::decode_packed_varints(
            p, end, p, &we, &fo, wire::array_sink<std::int64_t, wire::conv_int64>{out.data()});
        REQUIRE(n == expect.size());
        out.resize(n);
        CHECK(out == expect);
    }
    // (2) arena small tail
    {
        std::vector<std::int64_t> out(expect.size() + 8, 0);
        ArenaDecodeError err{};
        const std::size_t n =
            arena_detail::decode_packed_varints_small<std::int64_t, wire::conv_int64>(
                p, end, out.data(), &err);
        REQUIRE(n == expect.size());
        out.resize(n);
        CHECK(out == expect);
    }
    // (3) arena large entry -> kernel
    {
        std::vector<std::int64_t> out(expect.size() + 8, 0);
        ArenaDecodeError err{};
        const std::size_t n =
            arena_detail::decode_packed_varints_large<std::int64_t, wire::conv_int64>(
                p, end, out.data(), &err);
        REQUIRE(n == expect.size());
        out.resize(n);
        CHECK(out == expect);
    }
}

// One decode path's rejection: count == SIZE_MAX, and matching error code + span-relative offset.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): positional -- result triple then expected pair.
void expect_reject(std::size_t n, WireError code, std::size_t off, WireError expect_code,
                   std::size_t expect_off) {
    CHECK(n == static_cast<std::size_t>(-1));
    CHECK(code == expect_code);
    CHECK(off == expect_off);
}

// Require all three entry points REJECT `bytes` with `expect_code` at `expect_off` (span-relative) -- so
// the kernel, the arena small tail, and the arena large entry agree on BOTH the error and the offset on
// malformed input.
void require_all_paths_reject(const std::vector<std::uint8_t>& bytes, WireError expect_code,
                              std::size_t expect_off) {
    const std::uint8_t* const p = bytes.data();
    const std::uint8_t* const end = p + bytes.size();
    std::vector<std::int64_t> out1(bytes.size() + 8, 0);
    std::vector<std::int64_t> out2(bytes.size() + 8, 0);
    std::vector<std::int64_t> out3(bytes.size() + 8, 0);
    WireError we1 = WireError::None;
    std::size_t fo1 = 0;
    ArenaDecodeError err2{};
    ArenaDecodeError err3{};
    const std::size_t n1 = wire::decode_packed_varints(
        p, end, p, &we1, &fo1, wire::array_sink<std::int64_t, wire::conv_int64>{out1.data()});
    const std::size_t n2 =
        arena_detail::decode_packed_varints_small<std::int64_t, wire::conv_int64>(
            p, end, out2.data(), &err2);
    const std::size_t n3 =
        arena_detail::decode_packed_varints_large<std::int64_t, wire::conv_int64>(
            p, end, out3.data(), &err3);
    // All three paths must reject with the same (code, span-relative offset).
    expect_reject(n1, we1, fo1, expect_code, expect_off);
    expect_reject(n2, err2.wire, err2.offset, expect_code, expect_off);
    expect_reject(n3, err3.wire, err3.offset, expect_code, expect_off);
}

// `count` int64 values each encoding to exactly `width` bytes (deterministic salt).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): width and count are unrelated by name and use.
std::vector<std::int64_t> fixed_width_values(int width, int count) {
    std::vector<std::int64_t> v;
    v.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        v.push_back(
            varint_of_width(width, static_cast<std::uint64_t>(i) * std::uint64_t{2654435761}));
    }
    return v;
}

// A width-3 run of 1023 bytes, then a 10-byte varint at bytes 1023..1032, then a width-3 tail. The wide
// outlier forces the Fixed<3> kernel to bail to the scalar reader mid-run and resume; the byte offset
// (1023) is also a stable, awkward reject anchor for the malformed cases below.
std::vector<std::int64_t> straddle_span() {
    std::vector<std::int64_t> v;
    v.reserve(842);
    for (int i = 0; i < 341; ++i) {
        v.push_back(varint_of_width(3, static_cast<std::uint64_t>(i)));
    }
    v.push_back(varint_of_width(10, 7));
    for (int i = 0; i < 500; ++i) {
        v.push_back(varint_of_width(3, static_cast<std::uint64_t>(i)));
    }
    return v;
}

// Exactly one 64-byte Bulk1 block (all-1-byte) then mixed wide values -- the kernel's Bulk1 -> re-classify
// transition into a Fixed/Struct arm.
std::vector<std::int64_t> handoff_span() {
    std::vector<std::int64_t> v;
    v.reserve(464);
    for (int i = 0; i < 64; ++i) {
        v.push_back(varint_of_width(1, static_cast<std::uint64_t>(i)));
    }
    for (int i = 0; i < 400; ++i) {
        v.push_back(varint_of_width(i % 3 == 0 ? 7 : 4, static_cast<std::uint64_t>(i)));
    }
    return v;
}

// A homogeneous-3-byte run interrupted by a 1- and a 2-byte value: the P0 merged-varint shape.
std::vector<std::int64_t> merged_guard_span() {
    std::vector<std::int64_t> v;
    v.reserve(402);
    for (int i = 0; i < 200; ++i) {
        v.push_back(varint_of_width(3, static_cast<std::uint64_t>(i)));
    }
    v.push_back(varint_of_width(1, 5));  // a 1-byte value amid 3-byte ones
    v.push_back(varint_of_width(2, 9));
    for (int i = 0; i < 200; ++i) {
        v.push_back(varint_of_width(3, static_cast<std::uint64_t>(i) + 1000U));
    }
    return v;
}

// A narrow-mixed run (widths 1-3 -> classified Struct) with a 10-byte outlier every 100 elements. The
// Struct kernel decodes those 9/10-byte outliers IN PLACE via its scalar `len > 8` path -- a live branch
// (incl. a malformed-reject) no other case reaches (confirmed engaged by instrumentation).
std::vector<std::int64_t> struct_wide_outlier_span() {
    std::vector<std::int64_t> v;
    v.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        const int w = (i % 100 == 50) ? 10 : (1 + (i % 3));
        v.push_back(varint_of_width(w, static_cast<std::uint64_t>(i) * std::uint64_t{2654435761}));
    }
    return v;
}

// Two long homogeneous runs of DIFFERENT widths, each > 4096 bytes: the first engages a Fixed kernel that
// runs its full stretch, hits the width change, and the classifier RE-CLASSIFIES the remainder (via the
// >=4096-progress / >=512-remaining gates) -- a path the mixed/skew distributions never take.
std::vector<std::int64_t> reclassify_span() {
    std::vector<std::int64_t> v;
    v.reserve(4000);
    for (int i = 0; i < 2000; ++i) {  // 6000 bytes of width-3 -> Fixed<3>
        v.push_back(varint_of_width(3, static_cast<std::uint64_t>(i)));
    }
    for (int i = 0; i < 2000; ++i) {  // then 10000 bytes of width-5 -> re-classify -> Fixed<5>
        v.push_back(varint_of_width(5, static_cast<std::uint64_t>(i)));
    }
    return v;
}

}  // namespace

TEST_CASE("packed-kernels: homogeneous widths 1..10 above the 256B gate", "[packed-kernels]") {
    // Each width selects a distinct kernel: 1->Bulk1, 2->Bulk2, 3..8->Fixed<W>, 9/10->Fixed<9/10>.
    for (int w = 1; w <= 10; ++w) {
        // Enough elements that the byte span comfortably exceeds 256 (so the kernel, not the tail, runs).
        const int count = 300;
        const std::vector<std::int64_t> vals = fixed_width_values(w, count);
        const std::vector<std::uint8_t> bytes = encode_packed(vals);
        REQUIRE(bytes.size() >= 256U);
        CAPTURE(w, bytes.size());
        require_all_paths_decode(bytes, vals);
    }
}

TEST_CASE("packed-kernels: mixed-width distributions above threshold", "[packed-kernels]") {
    // Struct (narrow-mixed) and Swar (wide/uniform) are only reachable via non-homogeneous windows.
    std::mt19937_64 rng(12345);
    struct Dist {
        const char* name;
        int lo, hi;
    };
    for (const Dist d : {Dist{"narrow-1-2", 1, 2}, Dist{"narrow-1-3", 1, 3},
                         Dist{"uniform-1-10", 1, 10}, Dist{"wide-6-10", 6, 10}}) {
        std::uniform_int_distribution<int> width(d.lo, d.hi);
        std::vector<std::int64_t> vals;
        vals.reserve(400);
        for (int i = 0; i < 400; ++i) {
            vals.push_back(varint_of_width(width(rng), rng()));
        }
        CAPTURE(d.name);
        require_all_paths_decode(encode_packed(vals), vals);
    }
    // Skew: 90% 1-byte with occasional wide values -- a 1-byte-dominant mixed shape that classifies as
    // Swar (per-element SWAR with a 1-byte fast path), the common real-world "small ids + rare big" case.
    {
        std::uniform_int_distribution<int> tenth(0, 9);
        std::uniform_int_distribution<int> any(1, 10);
        std::vector<std::int64_t> vals;
        vals.reserve(2000);
        for (int i = 0; i < 2000; ++i) {
            const int w = (tenth(rng) == 0) ? any(rng) : 1;
            vals.push_back(varint_of_width(w, rng()));
        }
        require_all_paths_decode(encode_packed(vals), vals);
    }
}

TEST_CASE("packed-kernels: 256-byte engagement boundary", "[packed-kernels]") {
    // Spans of exactly 255 / 256 / 257 bytes must decode identically whether the kernel engages (>=256)
    // or the byte-loop tail handles it (<256). Build with 1-byte values so byte count == element count.
    for (const int nbytes : {255, 256, 257}) {
        const std::vector<std::int64_t> vals = fixed_width_values(1, nbytes);
        const std::vector<std::uint8_t> bytes = encode_packed(vals);
        REQUIRE(bytes.size() == static_cast<std::size_t>(nbytes));
        CAPTURE(nbytes);
        require_all_paths_decode(bytes, vals);
    }
}

TEST_CASE("packed-kernels: large spans with embedded wide varints", "[packed-kernels]") {
    // Multi-kilobyte spans that make the Fixed<W> kernels run long stretches and bail on width changes. Use
    // WIDTH-3 values (a Fixed<3> run) across a range of lengths, then spans with a 10-byte outlier embedded
    // in the run and a Bulk1->wide transition -- all differentially checked against the oracle.
    for (const int count : {342, 400, 683, 1000, 2000}) {  // 1026..6000 bytes
        const std::vector<std::int64_t> vals = fixed_width_values(3, count);
        const std::vector<std::uint8_t> bytes = encode_packed(vals);
        REQUIRE(bytes.size() > 1024U);
        CAPTURE(count, bytes.size());
        require_all_paths_decode(bytes, vals);
    }
    SECTION("10-byte varint embedded in a Fixed<3> run") {
        const std::vector<std::int64_t> vals = straddle_span();
        const std::vector<std::uint8_t> bytes = encode_packed(vals);
        REQUIRE(bytes.size() > 1032U);
        require_all_paths_decode(bytes, vals);
    }
    SECTION("Bulk1 block -> wide-value re-classify transition") {
        const std::vector<std::int64_t> vals = handoff_span();
        require_all_paths_decode(encode_packed(vals), vals);
    }
}

TEST_CASE("packed-kernels: P0 merged-varint guard (fixed_fill must not merge)",
          "[packed-kernels]") {
    // Regression for 82079ce: fixed_fill's homogeneous-width guard once checked only the last two bytes
    // and would MERGE a shorter varint with following bytes into one over-wide value. A homogeneous-W run
    // interrupted by a shorter varint must decode each element correctly (bailing to the tail), not merge.
    // Build a mostly-3-byte run with a stray 1-byte and 2-byte value in the middle; differential decode
    // catches any mis-merge (wrong value or wrong count).
    const std::vector<std::int64_t> vals = merged_guard_span();
    require_all_paths_decode(encode_packed(vals), vals);
}

TEST_CASE("packed-kernels: Struct 9/10-byte in-place + re-classify paths", "[packed-kernels]") {
    // Two live kernel branches the width/mixed cases above never reach (confirmed by instrumentation):
    // the Struct kernel's in-place scalar decode of a 9/10-byte outlier, and the classifier re-classifying
    // the remainder after a long homogeneous run changes width.
    SECTION("Struct decodes a 9/10-byte outlier in place") {
        const std::vector<std::int64_t> vals = struct_wide_outlier_span();
        require_all_paths_decode(encode_packed(vals), vals);
    }
    SECTION("re-classify after a long homogeneous run hits a width change") {
        const std::vector<std::int64_t> vals = reclassify_span();
        require_all_paths_decode(encode_packed(vals), vals);
    }
}

TEST_CASE("packed-kernels: malformed input rejected on every path", "[packed-kernels]") {
    SECTION("truncated final varint, error deep in a large span") {
        std::vector<std::int64_t> vals =
            fixed_width_values(3, 1500);        // 4500 bytes, a long Fixed<3> run
        vals.push_back(varint_of_width(5, 3));  // a multi-byte value to truncate
        std::vector<std::uint8_t> bytes = encode_packed(vals);
        bytes.pop_back();  // drop its terminator -> truncated
        require_all_paths_reject(bytes, WireError::TruncatedVarint,
                                 4500);  // the 5-byte value at byte 4500
    }
    SECTION("truncated wide varint embedded mid-span") {
        // straddle_span()'s 10-byte varint sits at bytes 1023..1032; drop the tail after it so that value
        // itself is left truncated -- the reject offset must point at its start.
        std::vector<std::uint8_t> bytes = encode_packed(straddle_span());
        bytes.resize(1030);  // mid the 10-byte varint at bytes 1023..1032
        require_all_paths_reject(bytes, WireError::TruncatedVarint,
                                 1023);  // the wide value starts at byte 1023
    }
    SECTION("overlong varint (11 continuation bytes) -> overflow") {
        std::vector<std::uint8_t> bytes = encode_packed(fixed_width_values(1, 300));
        bytes.insert(bytes.end(), 11, 0x80U);  // 11 continuation bytes, never terminating
        bytes.push_back(0x00U);
        require_all_paths_reject(bytes, WireError::VarintOverflow,
                                 300);  // overlong starts at byte 300
    }
}
