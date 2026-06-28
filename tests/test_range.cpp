#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "rapidproto/range.hpp"

using rapidproto::Range;

// The char-only string/string_view constructors must gate on the class
// parameter T, not on a deduced argument type. (NB: is_constructible<_, std::string>
// passes an *rvalue*, which the dangling-temporary delete below rejects — so the
// realistic "from an lvalue" check uses std::string&.)
static_assert(std::is_constructible_v<Range<char>, std::string&>,
              "Range<char> should be constructible from a std::string lvalue");
static_assert(std::is_constructible_v<Range<char>, std::string_view>,
              "Range<char> should be constructible from std::string_view");
// Range<NonChar> must NOT advertise a std::string constructor (otherwise
// is_constructible lies and the ctor body hard-errors when selected).
static_assert(!std::is_constructible_v<Range<int>, std::string&>,
              "Range<int> must NOT advertise a std::string constructor");

// Owning temporaries are rejected (they would dangle immediately) — both the
// explicit rvalue form and the bare type (is_constructible uses an rvalue).
static_assert(!std::is_constructible_v<Range<char>, std::string&&>,
              "Range<char> must reject owning std::string temporaries");
static_assert(!std::is_constructible_v<Range<char>, std::string>,
              "Range<char> must reject owning std::string temporaries (rvalue)");
static_assert(!std::is_constructible_v<Range<int>, std::vector<int>&&>,
              "Range<int> must reject owning std::vector temporaries");
// A string_view owns nothing, so a string_view temporary stays allowed.
static_assert(std::is_constructible_v<Range<char>, std::string_view&&>,
              "Range<char> may be built from a string_view temporary");

TEST_CASE("Range default construction is empty") {
    const Range<int> r;
    CHECK(r.size() == 0);  // NOLINT(readability-container-size-empty): exercises size() itself
    CHECK(r.empty());
    CHECK(r.begin() == r.end());
}

TEST_CASE("Range from data + size") {
    const int arr[] = {10, 20, 30};
    const Range<int> r(arr, 3);
    REQUIRE(r.size() == 3);
    CHECK_FALSE(r.empty());
    CHECK(r[0] == 10);
    CHECK(r[2] == 30);
    CHECK(r.front() == 10);
    CHECK(r.data() == arr);
}

TEST_CASE("Range from begin + end pointers") {
    const int arr[] = {1, 2, 3, 4};
    const Range<int> r(arr, arr + 4);
    CHECK(r.size() == 4);
    CHECK(r[3] == 4);

    const Range<int> empty(arr, arr);
    CHECK(empty.empty());
}

TEST_CASE("Range from C array has span extent (includes any trailing element)") {
    const int arr[] = {5, 6, 7};
    const Range<int> r(arr);
    CHECK(r.size() == 3);
    CHECK(r[1] == 6);

    // String literal includes the trailing '\0' (std::span-consistent).
    const Range<char> s("ab");
    CHECK(s.size() == 3);
    CHECK(s[2] == '\0');
}

TEST_CASE("Range from vector") {
    std::vector<int> v{1, 2, 3};
    const Range<int> r(v);
    REQUIRE(r.size() == 3);
    CHECK(r[1] == 2);
    CHECK(r.data() == v.data());
}

TEST_CASE("Range from std::string and string_view (char only)") {
    const std::string s = "hello";
    const Range<char> r(s);
    CHECK(r.size() == 5);
    CHECK(r.to_string_view() == "hello");

    const std::string_view sv = "world";
    const Range<char> r2(sv);
    CHECK(r2.size() == 5);
    CHECK(r2.to_string_view() == "world");
}

TEST_CASE("Range iteration") {
    const std::vector<int> v{1, 2, 3, 4};
    const Range<int> r(v);
    int sum = 0;
    for (const int x : r) {
        sum += x;
    }
    CHECK(sum == 10);
}

TEST_CASE("Range subspan(offset)") {
    const std::vector<int> v{1, 2, 3, 4, 5};
    const Range<int> r(v);

    auto tail = r.subspan(2);
    REQUIRE(tail.size() == 3);
    CHECK(tail[0] == 3);
    CHECK(tail.front() == 3);

    CHECK(r.subspan(5).empty());
    CHECK(r.subspan(99).empty());  // clamps, no UB
}

TEST_CASE("Range subspan(offset, count)") {
    const std::vector<int> v{1, 2, 3, 4, 5};
    const Range<int> r(v);

    auto mid = r.subspan(1, 2);
    REQUIRE(mid.size() == 2);
    CHECK(mid[0] == 2);
    CHECK(mid[1] == 3);

    // count clamps to available
    CHECK(r.subspan(3, 100).size() == 2);
    // offset past end clamps to empty
    CHECK(r.subspan(99, 100).empty());
}

TEST_CASE("Range subspan boundary arithmetic (no unsigned underflow)") {
    const std::vector<int> v{1, 2, 3};
    const Range<int> r(v);

    CHECK(r.subspan(0, 0).empty());             // zero count
    CHECK(r.subspan(r.size()).empty());         // offset == size
    CHECK(r.subspan(r.size(), 5).empty());      // offset == size with count
    CHECK(r.subspan(0, SIZE_MAX).size() == 3);  // count saturates, no overflow
    CHECK(r.subspan(SIZE_MAX).empty());         // offset saturates, no underflow
    CHECK(r.subspan(SIZE_MAX, SIZE_MAX).empty());
}

TEST_CASE("Range equality is element-wise") {
    const std::vector<int> a{1, 2, 3};
    const std::vector<int> b{1, 2, 3};
    const std::vector<int> c{1, 2};
    const std::vector<int> d{1, 2, 4};

    // a and b are distinct buffers with equal values: equality is content-based.
    CHECK(Range<int>(a) == Range<int>(b));
    CHECK(Range<int>(a) != Range<int>(c));  // different size
    CHECK(Range<int>(a) != Range<int>(d));  // same size, different element
    // empty ranges compare equal (deliberately exercising operator==, not emptiness)
    // NOLINTNEXTLINE(readability-container-size-empty)
    CHECK(Range<int>() == Range<int>());
}

TEST_CASE("Range subspan preserves identity into the same buffer") {
    std::vector<int> v{1, 2, 3, 4};
    const Range<int> r(v);
    auto s = r.subspan(1, 2);
    CHECK(s.data() == v.data() + 1);
}
