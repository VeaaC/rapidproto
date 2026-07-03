#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

EnumNode parse_enum_ok(std::string src, SyntaxLevel syntax) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    ParseContext ctx;
    ctx.syntax_level = syntax;
    auto r = parse_enum(Range<Token>(lexed.tokens), ctx);
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    return std::move(r.value().value);
}

bool enum_rejects(std::string src, SyntaxLevel syntax) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    ParseContext ctx;
    ctx.syntax_level = syntax;
    auto r = parse_enum(Range<Token>(lexed.tokens), ctx);
    return r.is_err() || !r.value().remaining.empty();
}

}  // namespace

TEST_CASE("enum: openness normalizes from syntax level") {
    CHECK(parse_enum_ok("enum E { A = 0; }", SyntaxLevel::Proto2).openness == EnumOpenness::Closed);
    CHECK(parse_enum_ok("enum E { A = 0; }", SyntaxLevel::Proto3).openness == EnumOpenness::Open);
    CHECK(parse_enum_ok("enum E { A = 0; }", SyntaxLevel::Edition).openness == EnumOpenness::Open);
}

TEST_CASE("enum: values, negatives, and per-value options") {
    const EnumNode e = parse_enum_ok(
        "enum E { UNKNOWN = 0; A = 1; NEG = -1; D = 2 [deprecated = true]; }", SyntaxLevel::Proto2);
    CHECK(e.name == "E");
    REQUIRE(e.values.size() == 4);
    CHECK(e.values[0].name == "UNKNOWN");
    CHECK(e.values[0].number == 0);
    CHECK(e.values[2].name == "NEG");
    CHECK(e.values[2].number == -1);
    REQUIRE(e.values[3].options.size() == 1);
    CHECK(e.values[3].options[0].name[0].name == "deprecated");
}

TEST_CASE("enum: hex and octal value numbers") {
    const EnumNode e = parse_enum_ok("enum E { A = 0x10; B = 010; }", SyntaxLevel::Proto3);
    REQUIRE(e.values.size() == 2);
    CHECK(e.values[0].number == 16);
    CHECK(e.values[1].number == 8);
}

TEST_CASE("enum: int32 boundary and negative-hex value numbers") {
    const EnumNode e = parse_enum_ok("enum E { MIN = -2147483648; MAX = 2147483647; HN = -0x10; }",
                                     SyntaxLevel::Proto3);
    REQUIRE(e.values.size() == 3);
    CHECK(e.values[0].number == INT32_MIN);
    CHECK(e.values[1].number == INT32_MAX);
    CHECK(e.values[2].number == -16);
}

TEST_CASE("enum: an INT64_MIN-magnitude value number parses without signed overflow") {
    // Invalid input (far outside int32), so the numbers are unspecified -- the parse must simply
    // succeed: parse_int32 negates a 2^63 magnitude, which must happen in the unsigned domain
    // (the signed negation would be UB; UBSan guards this).
    const EnumNode e = parse_enum_ok(
        "enum E { A = 0; B = -9223372036854775808; C = -0x8000000000000000; "
        "reserved -9223372036854775808 to -1; }",
        SyntaxLevel::Proto3);
    CHECK(e.values.size() == 3);
    CHECK(e.reserved.size() == 1);
}

TEST_CASE("enum: parser does not enforce allow_alias or first-value-zero (parser only)") {
    // Duplicate numbers and a nonzero proto3 first value both parse; protoc validates these.
    const EnumNode e =
        parse_enum_ok("enum E { option allow_alias = true; A = 5; B = 5; }", SyntaxLevel::Proto3);
    REQUIRE(e.values.size() == 2);
    CHECK(e.values[0].number == 5);
    CHECK(e.values[0].number == e.values[1].number);
}

TEST_CASE("enum: reserved range edge forms (degenerate, descending, negative-to-max)") {
    const EnumNode e = parse_enum_ok("enum E { A = 0; reserved 1 to 1, -2 to -5, -5 to max; }",
                                     SyntaxLevel::Proto3);
    REQUIRE(e.reserved.size() == 1);
    const auto& ranges = e.reserved[0].ranges;
    REQUIRE(ranges.size() == 3);
    CHECK(ranges[0].start == 1);
    CHECK(ranges[0].end == 1);  // degenerate single-value range
    CHECK(ranges[1].start == -2);
    CHECK(ranges[1].end == -5);  // descending; protoc validates ordering, not us
    CHECK(ranges[2].start == -5);
    CHECK(ranges[2].end == kMaxEnumNumber);
}

TEST_CASE("enum: reserved ranges with 'to max' and negatives") {
    const EnumNode e = parse_enum_ok("enum E { A = 0; reserved 2, 5 to 10, -3 to -1, 100 to max; }",
                                     SyntaxLevel::Proto3);
    REQUIRE(e.reserved.size() == 1);
    const auto& ranges = e.reserved[0].ranges;
    REQUIRE(ranges.size() == 4);
    CHECK(ranges[0].start == 2);
    CHECK(ranges[0].end == 2);  // single number -> [n, n]
    CHECK(ranges[1].start == 5);
    CHECK(ranges[1].end == 10);
    CHECK(ranges[2].start == -3);
    CHECK(ranges[2].end == -1);
    CHECK(ranges[3].start == 100);
    CHECK(ranges[3].end == kMaxEnumNumber);  // 'max' -> INT32_MAX for enums
}

TEST_CASE("enum: reserved names as string literals and identifiers") {
    const EnumNode e = parse_enum_ok(
        R"(enum E { A = 0; reserved "FOO", "BAR"; reserved baz, qux; })", SyntaxLevel::Proto3);
    REQUIRE(e.reserved.size() == 2);
    CHECK(e.reserved[0].names == std::vector<std::string>{"FOO", "BAR"});
    CHECK(e.reserved[0].ranges.empty());
    CHECK(e.reserved[1].names == std::vector<std::string>{"baz", "qux"});
}

TEST_CASE("enum: option declarations in the body") {
    const EnumNode e =
        parse_enum_ok("enum E { option allow_alias = true; A = 0; B = 0; }", SyntaxLevel::Proto2);
    REQUIRE(e.options.size() == 1);
    CHECK(e.options[0].name[0].name == "allow_alias");
    CHECK(e.values.size() == 2);
}

TEST_CASE("enum: visibility modifiers (editions)") {
    CHECK(parse_enum_ok("export enum E { A = 0; }", SyntaxLevel::Edition).visibility ==
          Visibility::Export);
    CHECK(parse_enum_ok("local enum E { A = 0; }", SyntaxLevel::Edition).visibility ==
          Visibility::Local);
    CHECK(parse_enum_ok("enum E { A = 0; }", SyntaxLevel::Edition).visibility ==
          Visibility::Default);
}

TEST_CASE("enum: empty statements are skipped") {
    const EnumNode e = parse_enum_ok("enum E { ; A = 0; ; B = 1; }", SyntaxLevel::Proto3);
    REQUIRE(e.values.size() == 2);
    CHECK(e.values[1].number == 1);
}

TEST_CASE("enum: keyword-spelled value names are allowed") {
    const EnumNode e = parse_enum_ok("enum E { message = 0; string = 1; }", SyntaxLevel::Proto3);
    REQUIRE(e.values.size() == 2);
    CHECK(e.values[0].name == "message");
    CHECK(e.values[1].name == "string");
}

TEST_CASE("enum: malformed bodies are rejected") {
    CHECK(enum_rejects("enum E { A 0; }", SyntaxLevel::Proto3));        // missing '='
    CHECK(enum_rejects("enum E { A = ; }", SyntaxLevel::Proto3));       // missing number
    CHECK(enum_rejects("enum { A = 0; }", SyntaxLevel::Proto3));        // missing name
    CHECK(enum_rejects("enum E { A = 0; ", SyntaxLevel::Proto3));       // unterminated body
    CHECK(enum_rejects("enum E { reserved ; }", SyntaxLevel::Proto3));  // empty reserved
    CHECK(enum_rejects("enum E { A = +1; }", SyntaxLevel::Proto3));     // '+' not allowed on number
    CHECK(enum_rejects("enum E { reserved 1, 2, ; }", SyntaxLevel::Proto3));  // trailing comma
    CHECK(
        enum_rejects(R"(enum E { reserved 1, "FOO"; })", SyntaxLevel::Proto3));  // mixed range+name
    CHECK(enum_rejects("enum E { option = 0; }",
                       SyntaxLevel::Proto3));  // 'option' is not a value name
}
