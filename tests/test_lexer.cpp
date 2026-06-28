#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rapidproto/lexer.hpp"

using rapidproto::lex;
using rapidproto::LexResult;
using rapidproto::Token;
using rapidproto::TokenKind;

namespace {

// Lex, requiring success, returning the (move-safe) LexResult for the caller to keep
// alive while inspecting token text views.
LexResult lex_ok(std::string source) {
    auto r = lex(std::move(source));
    REQUIRE(r.is_ok());
    return std::move(r).value();
}

std::string_view text(const Token& t) {
    return t.text;
}

}  // namespace

TEST_CASE("lex a small declaration: kinds, text, offsets") {
    const auto lr = lex_ok("message Foo { }");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 4);

    CHECK(t[0].kind == TokenKind::KwMessage);
    CHECK(text(t[0]) == "message");
    CHECK(t[0].byte_offset == 0);

    CHECK(t[1].kind == TokenKind::Identifier);
    CHECK(text(t[1]) == "Foo");
    CHECK(t[1].byte_offset == 8);

    CHECK(t[2].kind == TokenKind::LBrace);
    CHECK(t[2].byte_offset == 12);
    CHECK(t[3].kind == TokenKind::RBrace);
    CHECK(t[3].byte_offset == 14);
}

TEST_CASE("keywords vs identifiers") {
    const auto lr = lex_ok("message messages to int int32");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 5);
    CHECK(t[0].kind == TokenKind::KwMessage);
    CHECK(t[1].kind == TokenKind::Identifier);  // "messages" is not a keyword
    CHECK(t[2].kind == TokenKind::KwTo);
    CHECK(t[3].kind == TokenKind::Identifier);  // "int" is not a keyword
    CHECK(t[4].kind == TokenKind::KwInt32);
}

TEST_CASE("integer literal classification") {
    const auto lr = lex_ok("0 123 0x1F 0Xbeef 0755");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 5);
    for (const auto& tok : t) {
        CHECK(tok.kind == TokenKind::IntLiteral);
    }
    CHECK(text(t[2]) == "0x1F");
    CHECK(text(t[4]) == "0755");
}

TEST_CASE("float literal classification") {
    const auto lr = lex_ok("1.5 .5 1. 1e10 1.5e-3 2E+8");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 6);
    for (const auto& tok : t) {
        CHECK(tok.kind == TokenKind::FloatLiteral);
    }
    CHECK(text(t[1]) == ".5");
    CHECK(text(t[2]) == "1.");
}

TEST_CASE("greedy numeric munch: digit-led tokens glued to letters/dots are invalid") {
    // Per the spec, a numeric literal greedily consumes following letters and dots,
    // then must classify as an int or float -- otherwise it is a single invalid
    // token (an error), NOT a split into a number + identifier. (Matches protoc.)
    CHECK(lex("123abc").is_err());
    CHECK(lex("1to3").is_err());
    CHECK(lex("0.0.0").is_err());
    CHECK(lex("1.2.3").is_err());
    CHECK(lex("08").is_err());     // 8 is not an octal digit
    CHECK(lex("0x1.5").is_err());  // hex literal cannot have a fractional part
    CHECK(lex("1.foo").is_err());  // ".foo" is munched into the numeric and rejected
}

TEST_CASE("a dot between identifiers is a Dot, not a float") {
    const auto lr = lex_ok("foo.bar");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 3);
    CHECK(t[0].kind == TokenKind::Identifier);
    CHECK(t[1].kind == TokenKind::Dot);
    CHECK(t[2].kind == TokenKind::Identifier);
}

TEST_CASE("string literal decoding: simple") {
    const auto lr = lex_ok(R"("hello world")");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 1);
    CHECK(t[0].kind == TokenKind::StringLiteral);
    CHECK(t[0].str_value == "hello world");
}

TEST_CASE("string literal decoding: single quotes") {
    const auto lr = lex_ok("'abc'");
    REQUIRE(lr.tokens.size() == 1);
    CHECK(lr.tokens[0].str_value == "abc");
}

TEST_CASE("string escapes: simple") {
    // Source literal:  "\n\t\\\"\'\?\b"
    const auto lr = lex_ok(R"("\n\t\\\"\'\?\b")");
    REQUIRE(lr.tokens.size() == 1);
    CHECK(lr.tokens[0].str_value == std::string("\n\t\\\"'?\b"));
}

TEST_CASE("string escapes: hex/octal/unicode byte values") {
    CHECK(lex_ok(R"("\x41")").tokens[0].str_value == "A");
    CHECK(lex_ok(R"("\101")").tokens[0].str_value == "A");  // octal 101 = 65 = 'A'
    CHECK(lex_ok(R"("A")").tokens[0].str_value == "A");
    {
        // A 4-digit \u escape, assembled by byte so no literal "backslash-u" (which
        // tooling tends to pre-decode) appears in this source file. 0x5C == '\'.
        std::string u;
        u.push_back('"');
        u.push_back(static_cast<char>(0x5C));
        u += "u0041";
        u.push_back('"');
        CHECK(lex_ok(u).tokens[0].str_value == "A");
    }
    CHECK(lex_ok(R"("\U00000041")").tokens[0].str_value == "A");  // 8-digit \U

    // one-digit hex
    CHECK(lex_ok(R"("\x7")").tokens[0].str_value == std::string(1, '\x07'));

    // U+00E9 (é): the literal multibyte char, and the \u / \U escapes, all decode
    // to the two UTF-8 bytes 0xC3 0xA9.
    const std::string e_acute = {static_cast<char>(0xC3), static_cast<char>(0xA9)};
    CHECK(lex_ok("\"\xC3\xA9\"").tokens[0].str_value == e_acute);  // literal é
    CHECK(lex_ok(R"("é")").tokens[0].str_value == e_acute);
    CHECK(lex_ok(R"("\U000000e9")").tokens[0].str_value == e_acute);

    // Embedded NUL bytes are representable in the decoded value.
    CHECK(lex_ok(R"("\x00")").tokens[0].str_value == std::string(1, '\0'));
    CHECK(lex_ok(R"("\000")").tokens[0].str_value == std::string(1, '\0'));
}

TEST_CASE("escape digit-count boundaries") {
    // \x takes at most 2 hex digits (protobuf.com spec); the 3rd is a literal char.
    CHECK(lex_ok(R"("\x414")").tokens[0].str_value == "A4");
    // octal takes at most 3 digits.
    CHECK(lex_ok(R"("\1011")").tokens[0].str_value == "A1");
    // octal masks to a byte (\777 -> 0xFF).
    CHECK(lex_ok(R"("\777")").tokens[0].str_value == std::string(1, static_cast<char>(0xFF)));
}

TEST_CASE("invalid unicode escapes are rejected") {
    CHECK(lex(R"("\U00110000")").is_err());  // > U+10FFFF
    CHECK(lex(R"("\ud800")").is_err());      // lone high surrogate
    CHECK(lex(R"("\udfff")").is_err());      // lone low surrogate
    // U+D7FF is just below the surrogate range -> valid (3 UTF-8 bytes).
    CHECK(lex_ok(R"("퟿")").tokens[0].str_value.size() == 3);
}

TEST_CASE("adjacent string literals merge into one token") {
    const auto lr = lex_ok(R"("ab" "cd"   "ef")");
    REQUIRE(lr.tokens.size() == 1);
    CHECK(lr.tokens[0].kind == TokenKind::StringLiteral);
    CHECK(lr.tokens[0].str_value == "abcdef");
    CHECK(lr.tokens[0].byte_offset == 0);
}

TEST_CASE("adjacent string merge across a comment; trailing trivia excluded") {
    const auto lr = lex_ok(R"("ab" /* c */ "cd"   )");
    REQUIRE(lr.tokens.size() == 1);
    CHECK(lr.tokens[0].str_value == "abcd");
    // The token text span runs first-quote..last-quote and excludes trailing spaces.
    CHECK(text(lr.tokens[0]).back() == '"');
}

TEST_CASE("comments are discarded") {
    const auto lr = lex_ok("// a line comment\nmessage /* block */ Foo");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 2);
    CHECK(t[0].kind == TokenKind::KwMessage);
    CHECK(t[1].kind == TokenKind::Identifier);
}

TEST_CASE("a NUL byte inside a comment is an error") {
    CHECK(lex(std::string("// a\0b\n", 7)).is_err());   // line comment
    CHECK(lex(std::string("/* a\0b */", 9)).is_err());  // block comment
}

TEST_CASE("UTF-8 BOM is skipped") {
    const std::string bom = {static_cast<char>(0xEF), static_cast<char>(0xBB),
                             static_cast<char>(0xBF)};
    const auto lr = lex_ok(bom + "syntax");
    REQUIRE(lr.tokens.size() == 1);
    CHECK(lr.tokens[0].kind == TokenKind::KwSyntax);
    CHECK(lr.tokens[0].byte_offset == 3);  // offsets index the full buffer, BOM included
}

TEST_CASE("symbols") {
    const auto lr = lex_ok("; , . : / = - + < > ( ) [ ] { }");
    const auto& t = lr.tokens;
    REQUIRE(t.size() == 16);
    CHECK(t[0].kind == TokenKind::Semicolon);
    CHECK(t[4].kind == TokenKind::Slash);
    CHECK(t[6].kind == TokenKind::Minus);
    CHECK(t[7].kind == TokenKind::Plus);
    CHECK(t[14].kind == TokenKind::LBrace);
    CHECK(t[15].kind == TokenKind::RBrace);
}

TEST_CASE("empty and whitespace-only input yields no tokens") {
    CHECK(lex_ok("").tokens.empty());
    CHECK(lex_ok("   \n\t  ").tokens.empty());
}

TEST_CASE("lexer errors carry a position") {
    // `cut` reports a committed failure where the missing delimiter was expected:
    // the unterminated string `"abc` wants its close quote at end-of-input (offset 4).
    auto unterminated_string = lex(R"("abc)");
    REQUIRE(unterminated_string.is_err());
    CHECK(unterminated_string.error().byte_offset == 4);

    auto newline_in_string = lex("\"ab\ncd\"");
    CHECK(newline_in_string.is_err());

    // The unterminated block comment wants its "*/" at end-of-input (offset 19).
    auto unterminated_comment = lex("message /* unclosed");
    REQUIRE(unterminated_comment.is_err());
    CHECK(unterminated_comment.error().byte_offset == 19);

    CHECK(lex(R"("\q")").is_err());    // invalid escape
    CHECK(lex(R"("\x")").is_err());    // \x with no digits
    CHECK(lex(R"("\u12")").is_err());  // incomplete unicode
    CHECK(lex("@").is_err());          // unexpected character
    CHECK(lex("0x").is_err());         // hex literal with no digits
    CHECK(lex("1e").is_err());         // exponent with no digits
}

TEST_CASE("error offsets point at the offending position") {
    CHECK(lex(R"("\q")").error().byte_offset == 1);    // the backslash
    CHECK(lex(R"("ab\x")").error().byte_offset == 3);  // the backslash
    CHECK(lex("foo @").error().byte_offset == 4);      // the '@'
}

TEST_CASE("token text views survive moves, even for SSO-length sources") {
    auto r = lex("msg");  // short -> SSO if it were a plain std::string member
    REQUIRE(r.is_ok());
    LexResult moved = std::move(r).value();
    LexResult moved_again = std::move(moved);

    // Move through a reallocating vector as well.
    std::vector<LexResult> v;
    v.push_back(std::move(moved_again));
    for (int i = 0; i < 64; ++i) {
        v.push_back(lex_ok("x"));  // forces reallocations that move earlier elements
    }
    REQUIRE(v.front().tokens.size() == 1);
    CHECK(v.front().tokens[0].text == "msg");  // string_view still valid
    CHECK(v.front().tokens[0].kind == TokenKind::Identifier);
}
