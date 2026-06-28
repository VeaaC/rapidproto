#include <catch_amalgamated.hpp>

#include <string>
#include <utility>

#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

// A file is "rejected" if parse_file errors OR leaves tokens unconsumed.
bool file_rejects(std::string src) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    return r.is_err() || !r.value().remaining.empty();
}

bool file_accepts(std::string src) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    return r.is_ok() && r.value().remaining.empty();
}

// Build a schema with `depth` nested messages (innermost holds a field), to probe the parser's
// recursion-depth cap.
std::string nested_messages(int depth) {
    std::string src = R"(syntax = "proto3";)";
    for (int i = 0; i < depth; ++i) {
        src += " message M" + std::to_string(i) + " {";
    }
    src += " int32 x = 1;";
    for (int i = 0; i < depth; ++i) {
        src += " }";
    }
    return src;
}

// Build a schema whose lone option value is a `depth`-deep nested message-literal aggregate (the
// parser's other recursive-descent path).
std::string nested_aggregate(int depth) {
    std::string value = "0";
    for (int i = 0; i < depth; ++i) {
        value.insert(0, "{ a: ");
        value += " }";
    }
    return R"(syntax = "proto3"; message M { option (x) = )" + value + "; }";
}

}  // namespace

TEST_CASE("file errors: malformed declarations are rejected") {
    CHECK(file_rejects(R"(syntax = ;)"));               // missing syntax string
    CHECK(file_rejects(R"(syntax "proto3";)"));         // missing '='
    CHECK(file_rejects("package my.pkg"));              // missing ';'
    CHECK(file_rejects(R"(import "x.proto")"));         // missing ';'
    CHECK(file_rejects("message M {"));                 // unterminated message
    CHECK(file_rejects("message {}"));                  // missing message name
    CHECK(file_rejects("message M { int32 x = ; }"));   // missing field number
    CHECK(file_rejects("service S {"));                 // unterminated service braces
    CHECK(file_rejects("extend Foo { int32 x = 1; "));  // unterminated extend
    CHECK(file_rejects("message M { oneof o { } "));    // unterminated oneof
}

TEST_CASE("file errors: pathologically deep nesting fails cleanly (no stack overflow)") {
    // The parser is recursive descent; without the depth cap, deeply nested input would overflow the
    // stack. The cap (kMaxParseDepth) is far above any real schema, so absurd depths are rejected
    // while legitimately-deep schemas still parse.
    SECTION("deeper than any real schema but under the cap still parses") {
        CHECK(file_accepts(nested_messages(30)));
    }

    SECTION("nested messages far over the cap are rejected, not crashed") {
        auto lr = lex(nested_messages(5000));
        REQUIRE(lr.is_ok());
        const LexResult lexed = std::move(lr).value();
        auto r = parse_file(Range<Token>(lexed.tokens));
        REQUIRE(r.is_err());
        CHECK(r.error().message.find("nesting depth") != std::string::npos);
    }

    SECTION("nested option-literal aggregates far over the cap are rejected, not crashed") {
        CHECK(file_rejects(nested_aggregate(5000)));
    }
}

TEST_CASE("file errors: edge cases that should still parse") {
    CHECK(file_accepts(""));                                        // empty file
    CHECK(file_accepts(R"(syntax = "proto3";)"));                   // syntax only
    CHECK(file_accepts(";;;"));                                     // stray empty statements
    CHECK(file_accepts(R"(syntax = "proto3"; ; message M {} ;)"));  // interspersed empties
    CHECK(file_accepts("message M {};"));                           // trailing ';' after a message
}

TEST_CASE("file errors: a trailing unparseable token is reported") {
    // The message parses, but the dangling token is left unconsumed -> not fully accepted.
    CHECK(file_rejects("message M {} int32"));
}

TEST_CASE("file errors: structural rejections") {
    CHECK(file_rejects("group G = 1 { int32 v = 1; }"));  // group not allowed at file scope
    CHECK(file_rejects(R"(option x = true; syntax = "proto3";)"));  // syntax must come first
    CHECK(file_rejects("message M { int32 x = 1 }"));               // field missing trailing ';'
    CHECK(file_rejects("service S;"));                              // service requires a body
}
