#include <catch_amalgamated.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rapidproto/combinators.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

using rapidproto::Range;

namespace {

constexpr auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
constexpr auto is_alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
constexpr auto is_space = [](char c) { return c == ' '; };

// Convenience: a matched Range<char> as a std::string_view for comparison.
std::string_view sv(Range<char> r) {
    return r.to_string_view();
}

}  // namespace

// --- primitives ------------------------------------------------------------

TEST_CASE("one matches a single element") {
    const auto p = rapidproto::one(is_digit);

    const std::string ok = "5x";
    auto r = p(Range<char>(ok));
    REQUIRE(r.is_ok());
    CHECK(r.value().value == '5');
    CHECK(sv(r.value().remaining) == "x");

    const std::string bad = "x";
    CHECK(p(Range<char>(bad)).is_err());

    const std::string empty;
    CHECK(p(Range<char>(empty)).is_err());
}

TEST_CASE("tag matches an exact subsequence") {
    const auto p = rapidproto::tag("syntax");

    const std::string ok = "syntax=proto3";
    auto r = p(Range<char>(ok));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "syntax");
    CHECK(sv(r.value().remaining) == "=proto3");

    const std::string bad = "syntay";
    CHECK(p(Range<char>(bad)).is_err());

    const std::string shortInput = "syn";
    CHECK(p(Range<char>(shortInput)).is_err());
}

TEST_CASE("take_while consumes zero or more") {
    const auto p = rapidproto::take_while(is_digit);

    const std::string some = "123ab";
    auto r = p(Range<char>(some));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "123");
    CHECK(sv(r.value().remaining) == "ab");

    const std::string none = "abc";
    auto r2 = p(Range<char>(none));
    REQUIRE(r2.is_ok());
    CHECK(r2.value().value.empty());
    CHECK(sv(r2.value().remaining) == "abc");
}

TEST_CASE("take_while1 requires at least one") {
    const auto p = rapidproto::take_while1(is_digit);

    const std::string some = "12ab";
    auto r = p(Range<char>(some));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "12");

    const std::string none = "ab";
    CHECK(p(Range<char>(none)).is_err());
}

TEST_CASE("take_till consumes until predicate") {
    const auto p = rapidproto::take_till(is_space);
    const std::string s = "abc def";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "abc");
    CHECK(sv(r.value().remaining) == " def");
}

TEST_CASE("take_until stops before a needle") {
    const auto p = rapidproto::take_until("*/");

    const std::string s = "body */rest";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "body ");
    CHECK(sv(r.value().remaining) == "*/rest");

    const std::string missing = "no terminator";
    CHECK(p(Range<char>(missing)).is_err());
}

// --- combinators -----------------------------------------------------------

TEST_CASE("alt returns the first match") {
    const auto p = rapidproto::alt(rapidproto::tag("foo"), rapidproto::tag("bar"));

    const std::string s = "bar!";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "bar");
    CHECK(sv(r.value().remaining) == "!");
}

TEST_CASE("alt reports the farthest error on all-fail") {
    // branch 1 gets 2 chars in before failing; branch 2 fails immediately.
    // (recognize() normalizes the seq's tuple output to Range<char> so both
    // branches share one output type, as alt requires.)
    const auto p = rapidproto::alt(
        rapidproto::recognize(rapidproto::seq(rapidproto::tag("ab"), rapidproto::tag("cd"))),
        rapidproto::tag("x"));
    const std::string s = "abz";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_err());
    CHECK(r.error().byte_offset == 2);  // farthest = the "cd" mismatch after "ab"
}

TEST_CASE("seq collects a tuple and threads input") {
    const auto p = rapidproto::seq(rapidproto::tag("a"), rapidproto::tag("b"));

    const std::string ok = "abc";
    auto r = p(Range<char>(ok));
    REQUIRE(r.is_ok());
    CHECK(sv(std::get<0>(r.value().value)) == "a");
    CHECK(sv(std::get<1>(r.value().value)) == "b");
    CHECK(sv(r.value().remaining) == "c");

    const std::string bad = "ax";
    auto e = p(Range<char>(bad));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 1);  // second element fails at offset 1
}

TEST_CASE("opt never fails") {
    const auto p = rapidproto::opt(rapidproto::tag("x"));

    const std::string present = "xy";
    auto r = p(Range<char>(present));
    REQUIRE(r.is_ok());
    REQUIRE(r.value().value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(sv(*r.value().value) == "x");
    CHECK(sv(r.value().remaining) == "y");

    const std::string absent = "zy";
    auto r2 = p(Range<char>(absent));
    REQUIRE(r2.is_ok());
    CHECK_FALSE(r2.value().value.has_value());
    CHECK(sv(r2.value().remaining) == "zy");  // no input consumed
}

TEST_CASE("many collects zero or more") {
    const auto p = rapidproto::many(rapidproto::one(is_digit));

    const std::string s = "123a";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_ok());
    CHECK(r.value().value == std::vector<char>{'1', '2', '3'});
    CHECK(sv(r.value().remaining) == "a");

    const std::string none = "abc";
    auto r2 = p(Range<char>(none));
    REQUIRE(r2.is_ok());
    CHECK(r2.value().value.empty());
}

TEST_CASE("many guards against non-consuming sub-parsers") {
    // take_while can succeed consuming nothing -> infinite-loop guard fires.
    const auto p = rapidproto::many(rapidproto::take_while(is_digit));
    const std::string s = "abc";
    CHECK(p(Range<char>(s)).is_err());
}

TEST_CASE("many1 requires at least one") {
    const auto p = rapidproto::many1(rapidproto::one(is_digit));

    const std::string ok = "12a";
    auto r = p(Range<char>(ok));
    REQUIRE(r.is_ok());
    CHECK(r.value().value == std::vector<char>{'1', '2'});
    CHECK(sv(r.value().remaining) == "a");

    const std::string bad = "ab";
    CHECK(p(Range<char>(bad)).is_err());
}

TEST_CASE("map transforms the output") {
    const auto p = rapidproto::map(rapidproto::take_while1(is_digit),
                                   [](Range<char> r) { return std::stoi(std::string(sv(r))); });
    const std::string s = "42x";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_ok());
    CHECK(r.value().value == 42);
    CHECK(sv(r.value().remaining) == "x");
}

TEST_CASE("recognize returns the consumed span") {
    const auto p =
        rapidproto::recognize(rapidproto::seq(rapidproto::tag("ab"), rapidproto::tag("cd")));
    const std::string s = "abcdef";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "abcd");
    CHECK(sv(r.value().remaining) == "ef");
}

TEST_CASE("all_consuming requires the whole input") {
    const auto p = rapidproto::all_consuming(rapidproto::tag("abc"));

    const std::string ok = "abc";
    CHECK(p(Range<char>(ok)).is_ok());

    const std::string trailing = "abcd";
    auto e = p(Range<char>(trailing));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 3);
}

TEST_CASE("delimited returns the middle") {
    const auto p = rapidproto::delimited(rapidproto::tag("("), rapidproto::take_while(is_alpha),
                                         rapidproto::tag(")"));

    const std::string ok = "(abc)x";
    auto r = p(Range<char>(ok));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "abc");
    CHECK(sv(r.value().remaining) == "x");

    const std::string unterminated = "(abc";
    auto e = p(Range<char>(unterminated));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 4);  // missing ')' after "(abc"
}

TEST_CASE("preceded discards the prefix") {
    const auto p = rapidproto::preceded(rapidproto::tag(">"), rapidproto::take_while1(is_alpha));
    const std::string s = ">abc";
    auto r = p(Range<char>(s));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "abc");
}

TEST_CASE("separated_list parses item (sep item)*") {
    const auto p =
        rapidproto::separated_list(rapidproto::take_while1(is_alpha), rapidproto::tag(","));

    const std::string list = "a,bb,ccc";
    auto r = p(Range<char>(list));
    REQUIRE(r.is_ok());
    REQUIRE(r.value().value.size() == 3);
    CHECK(sv(r.value().value[0]) == "a");
    CHECK(sv(r.value().value[1]) == "bb");
    CHECK(sv(r.value().value[2]) == "ccc");
    CHECK(r.value().remaining.empty());

    const std::string none = "123";
    auto r2 = p(Range<char>(none));
    REQUIRE(r2.is_ok());
    CHECK(r2.value().value.empty());
    CHECK(sv(r2.value().remaining) == "123");

    const std::string trailing = "a,";
    CHECK(p(Range<char>(trailing)).is_err());  // trailing separator
}

// --- deeper offset-propagation and failure-path coverage -------------------

TEST_CASE("seq lifts offsets through nesting") {
    const auto p = rapidproto::seq(rapidproto::tag("a"),
                                   rapidproto::seq(rapidproto::tag("b"), rapidproto::tag("c")));
    const std::string s = "abx";  // inner "c" fails at absolute offset 2
    auto e = p(Range<char>(s));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 2);
}

TEST_CASE("delimited reports prefix and middle failures") {
    const auto p = rapidproto::delimited(rapidproto::tag("("), rapidproto::take_while1(is_alpha),
                                         rapidproto::tag(")"));

    const std::string noOpen = "abc)";
    auto e1 = p(Range<char>(noOpen));
    REQUIRE(e1.is_err());
    CHECK(e1.error().byte_offset == 0);  // '(' missing

    const std::string emptyMid = "()";
    auto e2 = p(Range<char>(emptyMid));
    REQUIRE(e2.is_err());
    CHECK(e2.error().byte_offset == 1);  // alpha middle fails after '('
}

TEST_CASE("preceded forwards a body failure with a lifted offset") {
    const auto p = rapidproto::preceded(rapidproto::tag(">"), rapidproto::take_while1(is_alpha));

    const std::string bodyFails = ">123";  // body fails right after '>'
    auto e = p(Range<char>(bodyFails));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 1);

    const std::string preFails = "abc";  // prefix fails at 0
    auto e2 = p(Range<char>(preFails));
    REQUIRE(e2.is_err());
    CHECK(e2.error().byte_offset == 0);
}

TEST_CASE("map and recognize forward inner errors") {
    const std::string s = "x";

    const auto m =
        rapidproto::map(rapidproto::tag("k"), [](Range<char> r) { return sv(r).size(); });
    CHECK(m(Range<char>(s)).is_err());

    const auto rec = rapidproto::recognize(rapidproto::tag("k"));
    CHECK(rec(Range<char>(s)).is_err());
}

TEST_CASE("take_till edge cases") {
    const auto p = rapidproto::take_till(is_space);

    const std::string allMatch = "abc";  // predicate never true -> consume all
    auto r = p(Range<char>(allMatch));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "abc");
    CHECK(r.value().remaining.empty());

    const std::string immediate = " x";  // predicate true at 0 -> empty span
    auto r2 = p(Range<char>(immediate));
    REQUIRE(r2.is_ok());
    CHECK(r2.value().value.empty());
    CHECK(sv(r2.value().remaining) == " x");
}

TEST_CASE("separated_list reports a mid-list item failure offset") {
    const auto p =
        rapidproto::separated_list(rapidproto::take_while1(is_alpha), rapidproto::tag(","));

    const std::string midFail = "ab,cd,99";  // third item fails at offset 6
    auto e = p(Range<char>(midFail));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 6);

    const std::string trailing = "a,";  // trailing sep -> item fails at offset 2
    auto e2 = p(Range<char>(trailing));
    REQUIRE(e2.is_err());
    CHECK(e2.error().byte_offset == 2);
}

TEST_CASE("many1 guards against non-consuming sub-parsers") {
    const auto p = rapidproto::many1(rapidproto::take_while(is_digit));
    const std::string s = "abc";  // take_while matches nothing -> guard fires
    CHECK(p(Range<char>(s)).is_err());
}

TEST_CASE("alt with three branches; all fail at offset 0") {
    const auto p =
        rapidproto::alt(rapidproto::tag("aa"), rapidproto::tag("bb"), rapidproto::tag("cc"));

    const std::string hit = "bbx";  // non-first branch succeeds
    auto r = p(Range<char>(hit));
    REQUIRE(r.is_ok());
    CHECK(sv(r.value().value) == "bb");

    const std::string miss = "zz";
    auto e = p(Range<char>(miss));
    REQUIRE(e.is_err());
    CHECK(e.error().byte_offset == 0);
}

TEST_CASE("cut: a committed failure stops alt from trying later branches") {
    const std::string s = "ax";

    // Without cut, alt falls through to the second branch and matches "a".
    const auto without = rapidproto::alt(rapidproto::tag("ab"), rapidproto::tag("a"));
    CHECK(without(Range<char>(s)).is_ok());

    // With cut, the first branch's failure is fatal -> alt does NOT try the second.
    const auto with = rapidproto::alt(rapidproto::cut(rapidproto::tag("ab")), rapidproto::tag("a"));
    auto r = with(Range<char>(s));
    REQUIRE(r.is_err());
    CHECK(r.error().fatal);
}

TEST_CASE("cut: a committed failure propagates through many and opt") {
    // many normally stops at a non-match; a cut failure instead propagates as an error.
    const auto p_many = rapidproto::many(rapidproto::cut(rapidproto::tag("a")));
    const std::string s = "aab";
    auto rm = p_many(Range<char>(s));
    REQUIRE(rm.is_err());
    CHECK(rm.error().fatal);

    // opt normally yields nullopt on failure; a cut failure propagates instead.
    const auto p_opt = rapidproto::opt(rapidproto::cut(rapidproto::tag("x")));
    const std::string t = "yz";
    auto ro = p_opt(Range<char>(t));
    REQUIRE(ro.is_err());
    CHECK(ro.error().fatal);
}

TEST_CASE("cut: commit after a prefix, with the failure offset lifted by seq") {
    // recognize() unifies the seq's tuple output to Range so both alt branches match.
    const auto committed = rapidproto::recognize(
        rapidproto::seq(rapidproto::tag("("), rapidproto::cut(rapidproto::tag("a"))));

    const std::string ok = "(a";
    CHECK(committed(Range<char>(ok)).is_ok());  // success path unaffected by cut

    const auto choice = rapidproto::alt(committed, rapidproto::tag("(b"));
    const std::string bad = "(b";
    auto r = choice(Range<char>(bad));
    REQUIRE(r.is_err());  // cut prevented falling through to "(b"
    CHECK(r.error().fatal);
    CHECK(r.error().byte_offset == 1);  // the committed tag("a") failed at offset 1
}

TEST_CASE("combinators on empty input") {
    const std::string empty;

    CHECK(rapidproto::many(rapidproto::one(is_digit))(Range<char>(empty)).is_ok());
    CHECK(rapidproto::many1(rapidproto::one(is_digit))(Range<char>(empty)).is_err());
    CHECK(rapidproto::all_consuming(rapidproto::opt(rapidproto::tag("x")))(Range<char>(empty))
              .is_ok());
    CHECK(rapidproto::separated_list(rapidproto::take_while1(is_alpha),
                                     rapidproto::tag(","))(Range<char>(empty))
              .is_ok());
}
