#include <catch_amalgamated.hpp>

#include <string>
#include <utility>

#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source_id.hpp"

using rapidproto::Error;
using rapidproto::Parsed;
using rapidproto::Range;
using rapidproto::Result;
using rapidproto::SourceId;

TEST_CASE("Error from offset + message has no source") {
    const Error e(7, "boom");
    CHECK_FALSE(e.source.valid());
    CHECK(e.byte_offset == 7);
    CHECK(e.message == "boom");
}

TEST_CASE("Error can carry a source id") {
    const Error e(SourceId{3}, 12, "bad");
    REQUIRE(e.source.valid());
    CHECK(e.source.index() == 3);
    CHECK(e.byte_offset == 12);
    CHECK(e.message == "bad");
}

TEST_CASE("Result holds a success value") {
    Result<int> r(42);
    REQUIRE(r.is_ok());
    CHECK_FALSE(r.is_err());
    CHECK(static_cast<bool>(r));
    CHECK(r.value() == 42);
}

TEST_CASE("Result holds an error") {
    Result<int> r(Error{7, "boom"});
    REQUIRE(r.is_err());
    CHECK_FALSE(r.is_ok());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.error().byte_offset == 7);
    CHECK(r.error().message == "boom");
}

TEST_CASE("Result value is mutable through the lvalue accessor") {
    Result<int> r(1);
    r.value() = 99;
    CHECK(r.value() == 99);
}

TEST_CASE("Result supports move-only payloads") {
    Result<std::string> r(std::string("owned"));
    REQUIRE(r.is_ok());
    std::string moved = std::move(r).value();
    CHECK(moved == "owned");
}

namespace {

// Returns 2*x on success, forwarding the error otherwise — exercises RP_TRY's
// error-forwarding and success-binding paths.
Result<int> doubled(Result<int> in) {
    RP_TRY(x, std::move(in));
    return x * 2;
}

// Two RP_TRYs in one function (distinct lines -> distinct temporaries).
Result<int> sum_of(Result<int> a, Result<int> b) {
    RP_TRY(x, std::move(a));
    RP_TRY(y, std::move(b));
    return x + y;
}

// Two RP_TRYs on the SAME physical line: __COUNTER__ keeps the temporaries
// distinct (this would fail to compile with a __LINE__-based suffix). The
// clang-format guard preserves the single line this regression test depends on.
// clang-format off
Result<int> sum_same_line(Result<int> a, Result<int> b) {
    RP_TRY(x, std::move(a)); RP_TRY(y, std::move(b)); return x + y;
}
// clang-format on

// Enclosing function returns a DIFFERENT Result<T> than the unwrapped value:
// the error is forwarded via the implicit Result(Error) conversion.
Result<std::string> describe(Result<int> in) {
    RP_TRY(x, std::move(in));
    return std::to_string(x);
}

}  // namespace

TEST_CASE("RP_TRY binds the value on success") {
    auto r = doubled(Result<int>(21));
    REQUIRE(r.is_ok());
    CHECK(r.value() == 42);
}

TEST_CASE("RP_TRY forwards the error on failure") {
    auto r = doubled(Result<int>(Error{5, "bad input"}));
    REQUIRE(r.is_err());
    CHECK(r.error().byte_offset == 5);
    CHECK(r.error().message == "bad input");
}

TEST_CASE("RP_TRY: multiple uses in one scope") {
    CHECK(sum_of(Result<int>(3), Result<int>(4)).value() == 7);

    auto err_first = sum_of(Result<int>(Error{1, "first"}), Result<int>(4));
    REQUIRE(err_first.is_err());
    CHECK(err_first.error().message == "first");

    auto err_second = sum_of(Result<int>(3), Result<int>(Error{2, "second"}));
    REQUIRE(err_second.is_err());
    CHECK(err_second.error().message == "second");
}

TEST_CASE("RP_TRY: two uses on the same physical line don't collide") {
    CHECK(sum_same_line(Result<int>(10), Result<int>(5)).value() == 15);
    auto e = sum_same_line(Result<int>(Error{9, "x"}), Result<int>(5));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 9);
}

TEST_CASE("RP_TRY: forwards across a differing enclosing return type") {
    CHECK(describe(Result<int>(7)).value() == "7");

    auto e = describe(Result<int>(Error{3, "nope"}));
    REQUIRE(e.is_err());
    CHECK(e.error().message == "nope");
}

TEST_CASE("Parsed carries remaining input and a value") {
    const char src[] = {'a', 'b', 'c'};
    const Parsed<int, char> p{Range<char>(src).subspan(1), 7};
    CHECK(p.value == 7);
    REQUIRE(p.remaining.size() == 2);
    CHECK(p.remaining[0] == 'b');
}
