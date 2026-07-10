// Unit tests for the arena runtime (arena_runtime.hpp): the bump Arena, ArenaString SSO, the
// ArrayView/MapView read-only views, and ArenaDecodeError.

#include <catch_amalgamated.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

std::uintptr_t addr(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p);  // NOLINT(*-reinterpret-cast)
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("arena: allocations are aligned and non-overlapping", "[arena]") {
    Arena arena;
    for (const std::size_t align :
         {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
        void* const p = arena.allocate(align, align);
        REQUIRE(p != nullptr);
        CHECK(addr(p) % align == 0);
    }
    // Two distinct allocations do not overlap, and the bytes are writable.
    auto* a = static_cast<char*>(arena.allocate(64, 1));
    auto* b = static_cast<char*>(arena.allocate(64, 1));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    std::memset(a, 0xAA, 64);
    std::memset(b, 0xBB, 64);
    CHECK(a[0] == static_cast<char>(0xAA));
    CHECK(b[0] == static_cast<char>(0xBB));
    CHECK((b >= a + 64 || a >= b + 64));  // disjoint
}

TEST_CASE("arena: grows across chunks without corrupting prior allocations", "[arena]") {
    Arena arena;
    constexpr int kN = 20000;  // 20000 * 8B = 160KB, far past the 4KB default chunk
    std::vector<std::uint64_t*> ptrs;
    ptrs.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        ptrs.push_back(arena.create<std::uint64_t>(static_cast<std::uint64_t>(i)));
    }
    bool intact = true;
    for (int i = 0; i < kN; ++i) {
        const std::uint64_t* p = ptrs[static_cast<std::size_t>(i)];
        if (p == nullptr || *p != static_cast<std::uint64_t>(i)) {
            intact = false;
        }
    }
    CHECK(intact);  // every allocation valid and uncorrupted across chunk growth
    CHECK(arena.bytes_reserved() > 4096);  // multiple chunks were allocated
    CHECK(arena.bytes_used() >= static_cast<std::size_t>(kN) * sizeof(std::uint64_t));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("arena: allocate_array, create, copy_bytes", "[arena]") {
    Arena arena;
    CHECK(arena.allocate_array<int>(0) == nullptr);
    int* xs = arena.allocate_array<int>(4);
    REQUIRE(xs != nullptr);
    CHECK(addr(xs) % alignof(int) == 0);
    for (int i = 0; i < 4; ++i) {
        xs[i] = i * 10;
    }
    CHECK(xs[3] == 30);

    const std::string src = "the quick brown fox";
    const ByteView copy = arena.copy_bytes(src);
    CHECK(copy.size() == src.size());
    CHECK(copy.data() != src.data());  // distinct storage
    CHECK(copy == src);
    CHECK(arena.copy_bytes(ByteView{}).empty());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("arena: reset rewinds and reuses chunks", "[arena]") {
    Arena arena;
    for (int i = 0; i < 5000; ++i) {
        (void)arena.create<std::uint64_t>(std::uint64_t{1});
    }
    const std::size_t reserved_after_first = arena.bytes_reserved();
    CHECK(arena.bytes_used() > 0);

    arena.reset();
    CHECK(arena.bytes_used() == 0);

    for (int i = 0; i < 5000; ++i) {  // same workload reuses the existing chunks
        REQUIRE(arena.create<std::uint64_t>(std::uint64_t{2}) != nullptr);
    }
    CHECK(arena.bytes_reserved() == reserved_after_first);  // no new chunks malloc'd
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("arena: grow after reset keeps existing chunks reachable (no leak)", "[arena]") {
    Arena arena;
    for (int i = 0; i < 2000; ++i) {
        (void)arena.create<std::uint64_t>(std::uint64_t{1});  // build up a few small chunks
    }
    arena.reset();
    // One allocation larger than every existing chunk forces a grow; the new chunk must be spliced
    // in (not orphan the existing successors). A LeakSanitizer build proves nothing is leaked.
    constexpr std::size_t kBig = std::size_t{256} * 1024;
    auto* big = static_cast<char*>(arena.allocate(kBig, 8));
    REQUIRE(big != nullptr);
    std::memset(big, 0x5A, kBig);
    CHECK(big[kBig - 1] == static_cast<char>(0x5A));
    bool ok = true;  // the preserved chunks remain usable for subsequent allocations
    for (int i = 0; i < 2000; ++i) {
        if (arena.create<std::uint64_t>(std::uint64_t{2}) == nullptr) {
            ok = false;
        }
    }
    CHECK(ok);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("arena: size overflow on untrusted counts/sizes is rejected, not wrapped", "[arena]") {
    Arena arena;
    // allocate_array: a count that would overflow n*sizeof(T) must fail, not under-allocate.
    CHECK(arena.allocate_array<std::uint64_t>(SIZE_MAX / sizeof(std::uint64_t) + 1) == nullptr);
    CHECK(arena.allocate_array<std::uint64_t>(SIZE_MAX) == nullptr);
    // A byte request so large that bytes+align would wrap must fail rather than malloc a tiny block.
    CHECK(arena.allocate(SIZE_MAX - 8, 8) == nullptr);
    CHECK(arena.allocate(SIZE_MAX, 16) == nullptr);
    // The arena is still usable after the rejected requests.
    auto* ok = arena.allocate_array<std::uint64_t>(4);
    REQUIRE(ok != nullptr);
    ok[3] = std::uint64_t{42};
    CHECK(ok[3] == std::uint64_t{42});
}

TEST_CASE("arena: a capacity limit fails allocation (the OOM path)", "[arena]") {
    // A cap below the first chunk makes every grow fail, so each allocator returns its OOM sentinel.
    Arena arena;
    arena.set_capacity_limit(64);
    CHECK(arena.allocate(128, 8) == nullptr);
    CHECK(arena.create<int>(42) == nullptr);
    CHECK(arena.allocate_array<int>(100) == nullptr);
    CHECK(arena.copy_bytes(ByteView("hello", 5)).data() == nullptr);  // OOM => empty/null view
    CHECK(arena.bytes_reserved() == 0);  // the cap blocked every reservation
}

TEST_CASE("arena: a capacity limit admits small allocations but fails an oversized one",
          "[arena]") {
    Arena arena;
    arena.set_capacity_limit(100000);
    auto* ok = arena.create<int>(7);  // fits in the first chunk
    REQUIRE(ok != nullptr);
    CHECK(*ok == 7);
    CHECK(arena.allocate(200000, 1) == nullptr);  // a chunk past the cap fails as OOM
    CHECK(*ok == 7);  // the failed grow left the earlier allocation intact
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("arena: caller-provided initial buffer is used before any malloc", "[arena]") {
    alignas(std::max_align_t) char buffer[512];
    Arena arena(buffer, sizeof buffer);
    CHECK(arena.bytes_reserved() >= 256);  // the buffer's usable region
    void* const p = arena.allocate(64, 8);
    REQUIRE(p != nullptr);
    CHECK(p >= buffer);
    CHECK(p < buffer + sizeof(buffer));
    const std::size_t reserved_from_buffer = arena.bytes_reserved();
    // Allocations that stay within the buffer never grow (the chunk descriptor is inline, so a
    // seeded arena that fits performs zero heap allocations -- proven separately under a new-counter).
    for (int i = 0; i < 4; ++i) {
        REQUIRE(arena.allocate(16, 8) != nullptr);
    }
    CHECK(arena.bytes_reserved() == reserved_from_buffer);  // still no growth
    // Exceeding the buffer grows via a heap chunk; reserved increases.
    REQUIRE(arena.allocate(4096, 8) != nullptr);
    CHECK(arena.bytes_reserved() > reserved_from_buffer);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("ArenaString: inline small values, copy big into the arena", "[arena][string]") {
    Arena arena;

    SECTION("empty") {
        const auto s = ArenaString::make(ByteView{}, arena);
        CHECK(s.empty());
        CHECK(s.view().empty());
        CHECK(arena.bytes_used() == 0);  // no allocation
    }

    SECTION("inline up to the cap (15 bytes)") {
        const std::size_t before = arena.bytes_used();
        std::string src(ArenaString::kInlineCap, 'z');  // exactly 15
        const auto s = ArenaString::make(src, arena);
        src.assign(ArenaString::kInlineCap, '!');  // mutate the source afterwards
        CHECK(s.size() == ArenaString::kInlineCap);
        CHECK(s.view() == std::string(ArenaString::kInlineCap, 'z'));  // copied, unaffected
        CHECK(arena.bytes_used() == before);  // inline: no arena allocation
    }

    SECTION("heap above the cap") {
        std::string src(64, 'q');  // > 15 -> arena copy
        const auto s = ArenaString::make(src, arena);
        src.assign(64, '?');  // mutate the source afterwards
        CHECK(s.size() == 64);
        CHECK(s.view() == std::string(64, 'q'));  // copied, unaffected
        CHECK(arena.bytes_used() >= 64);
        CHECK(s.view().data() != src.data());
    }

    SECTION("every inline length round-trips with DISTINCT bytes") {
        // The inline SSO copy has three size branches (n in [1,3], [4,7], [8,15]); the uniform-fill
        // sections above would not notice a byte landing in the wrong slot. Distinct bytes per index
        // pin each branch exactly, and mutating the source after the copy proves independence.
        for (std::size_t n = 0; n <= ArenaString::kInlineCap; ++n) {
            std::string src;
            for (std::size_t i = 0; i < n; ++i) {
                src.push_back(static_cast<char>('a' + i));  // a, b, c, ... all distinct
            }
            const std::string expect = src;
            const auto s = ArenaString::make(src, arena);
            src.assign(n, '!');  // clobber the source; the inline copy must be independent
            CHECK(s.size() == n);
            CHECK(s.view() == expect);
        }
    }
}

TEST_CASE("ArenaString: a value larger than the 32-bit length is rejected, not truncated",
          "[arena][string]") {
    // A heap ArenaString stores a 32-bit length, so >4 GiB is unrepresentable -- and only reachable
    // at 64-bit (at 32-bit a size_t cannot exceed the limit).
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        Arena arena;
        const char backing = 'x';
        // A ByteView that *claims* >4 GiB over a 1-byte buffer. make() checks the size before any
        // copy, so it returns empty without ever reading past the buffer.
        const ByteView too_big(&backing, static_cast<std::size_t>(UINT32_MAX) + 1);
        const ArenaString s = ArenaString::make(too_big, arena);
        CHECK(s.empty());                // rejected, not truncated to a bogus low-32-bit length
        CHECK(arena.bytes_used() == 0);  // nothing was copied into the arena
    }
}

TEST_CASE("ArrayView: read-only contiguous view", "[arena][arrayview]") {
    const int data[] = {2, 4, 6, 8};
    const ArrayView<int> s(data, 4);
    CHECK(s.size() == 4);
    CHECK_FALSE(s.empty());
    CHECK(s.data() == data);
    CHECK(s[2] == 6);
    int sum = 0;
    for (const int v : s) {
        sum += v;
    }
    CHECK(sum == 20);
    CHECK(ArrayView<int>{}.empty());
}

TEST_CASE("StringArrayView: indexing yields std::string_view, not ArenaString", "[arena][string]") {
    Arena arena;
    const std::string big(40, 'x');  // > kInlineCap: forces a heap (arena-copied) ArenaString
    const ArenaString strings[] = {ArenaString::make("hi", arena), ArenaString::make(big, arena),
                                   ArenaString::make(ByteView{}, arena)};
    const StringArrayView v(ArrayView<ArenaString>(strings, 3));

    const std::string_view first = v[0];  // operator[] returns std::string_view, not ArenaString
    CHECK(v.size() == 3);
    CHECK(first == "hi");  // inline element
    CHECK(v[1] == big);    // heap element
    CHECK(v[2].empty());   // empty element
    CHECK(StringArrayView{}.empty());
}

TEST_CASE("StringArrayView: range-for yields std::string_view", "[arena][string]") {
    Arena arena;
    const ArenaString strings[] = {ArenaString::make("ab", arena), ArenaString::make("cde", arena)};
    const StringArrayView v(ArrayView<ArenaString>(strings, 2));

    std::string joined;
    for (const std::string_view s : v) {  // the iterator dereferences to std::string_view
        joined += s;
    }
    CHECK(joined == "abcde");
}

namespace {
// A minimal map entry exposing key()/value(), as a generated map Entry would.
class Entry {
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): trivial test fixture
    Entry(int k, int v) : m_k(k), m_v(v) {}
    [[nodiscard]] int key() const { return m_k; }
    [[nodiscard]] int value() const { return m_v; }

private:
    int m_k;
    int m_v;
};
}  // namespace

TEST_CASE("MapView: linear find is last-wins", "[arena][map]") {
    const Entry entries[] = {{1, 10}, {2, 20}, {1, 11}, {3, 30}};  // key 1 appears twice
    const MapView<Entry> m(ArrayView<Entry>(entries, 4));
    CHECK(m.size() == 4);
    REQUIRE(m.find(1) != nullptr);
    CHECK(m.find(1)->value() == 11);  // last duplicate wins
    CHECK(m.find(2)->value() == 20);
    CHECK(m.find(99) == nullptr);
    CHECK(MapView<Entry>{}.empty());
}

TEST_CASE("ArenaDecodeError: ok() reflects the code", "[arena]") {
    ArenaDecodeError e;
    CHECK(e.ok());
    e.code = ArenaDecodeError::Code::OutOfMemory;
    CHECK_FALSE(e.ok());
}

TEST_CASE("ArenaDecodeError: rp_fail_string distinguishes too-long from out-of-memory", "[arena]") {
    const char backing = 'x';
    ArenaDecodeError oom;
    rp_fail_string(&oom, ByteView(&backing, 1));  // a small payload that failed to store => OOM
    CHECK(oom.code == ArenaDecodeError::Code::OutOfMemory);
    rp_fail_string(nullptr, ByteView(&backing, 1));               // a null err must be a safe no-op
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {  // >4 GiB only reachable at 64-bit
        ArenaDecodeError too_long;
        rp_fail_string(&too_long, ByteView(&backing, static_cast<std::size_t>(UINT32_MAX) + 1));
        CHECK(too_long.code == ArenaDecodeError::Code::StringTooLong);
    }
}

TEST_CASE("copy_payload: arena-copies raw payloads, keeps present-empty non-null, reports OOM",
          "[arena]") {
    Arena arena;
    // A copied payload is arena-owned: same bytes, different storage.
    const std::string src = "payload bytes";
    ByteView out;
    REQUIRE(arena_detail::copy_payload(ByteView(src), arena, out));
    CHECK(out == ByteView(src));
    CHECK(out.data() != src.data());

    // An EMPTY payload copies to a NON-null empty view: null data is reserved for "absent"
    // (the singular raw member's presence encoding), present-and-empty must stay distinct.
    ByteView empty;
    REQUIRE(arena_detail::copy_payload(ByteView(), arena, empty));
    CHECK(empty.empty());
    CHECK(empty.data() != nullptr);

    // A capacity-limited arena surfaces OOM (the raw arm turns this into OutOfMemory); the
    // destination is left untouched.
    Arena tiny;
    tiny.set_capacity_limit(16);
    auto untouched = ByteView(src);
    CHECK_FALSE(arena_detail::copy_payload(ByteView(std::string(64, 'z')), tiny, untouched));
    CHECK(untouched == ByteView(src));
}
