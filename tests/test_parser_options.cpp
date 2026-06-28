#include <catch_amalgamated.hpp>

#include <cmath>
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

// Lex `src`, run `fn` over the full token stream, require success + full
// consumption, and return the produced (self-owning) value. The LexResult is kept
// alive for the parse; the returned AST copies all strings, so it outlives it.
template <typename Fn>
auto parse_ok(std::string src, Fn fn) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = fn(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());  // consumed every token
    return std::move(r.value().value);
}

// True if parsing fails outright, or succeeds but leaves tokens unconsumed.
template <typename Fn>
bool parse_rejects(std::string src, Fn fn) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = fn(Range<Token>(lexed.tokens));
    return r.is_err() || !r.value().remaining.empty();
}

}  // namespace

// --- option names -----------------------------------------------------------

TEST_CASE("option decl: simple name and string value") {
    const Option o = parse_ok(R"(option java_package = "com.foo.bar";)", parse_option_decl);
    REQUIRE(o.name.size() == 1);
    CHECK(o.name[0].name == "java_package");
    CHECK_FALSE(o.name[0].is_extension);
    CHECK(std::get<std::string>(o.value.value) == "com.foo.bar");
}

TEST_CASE("option decl: keyword as a name component") {
    // `message` is a keyword but is legal in a name position.
    const Option o = parse_ok("option message = 1;", parse_option_decl);
    REQUIRE(o.name.size() == 1);
    CHECK(o.name[0].name == "message");
}

TEST_CASE("option decl: extension name in parens") {
    const Option o = parse_ok("option (my.pkg.ext) = 42;", parse_option_decl);
    REQUIRE(o.name.size() == 1);
    CHECK(o.name[0].name == "my.pkg.ext");
    CHECK(o.name[0].is_extension);
    CHECK(std::get<std::uint64_t>(o.value.value) == 42);
}

TEST_CASE("option decl: extension component followed by dotted field path") {
    const Option o = parse_ok("option (my.ext).field.sub = true;", parse_option_decl);
    REQUIRE(o.name.size() == 3);
    CHECK(o.name[0].name == "my.ext");
    CHECK(o.name[0].is_extension);
    CHECK(o.name[1].name == "field");
    CHECK_FALSE(o.name[1].is_extension);
    CHECK(o.name[2].name == "sub");
    CHECK(std::get<Identifier>(o.value.value).name == "true");  // raw: not coerced to bool
}

// --- scalar values ----------------------------------------------------------

TEST_CASE("scalar values: integers, signs, bases") {
    CHECK(std::get<std::uint64_t>(parse_ok("5", parse_value).value) == 5);
    CHECK(std::get<std::int64_t>(parse_ok("-5", parse_value).value) == -5);
    CHECK(std::get<std::uint64_t>(parse_ok("+5", parse_value).value) == 5);
    CHECK(std::get<std::uint64_t>(parse_ok("0xFF", parse_value).value) == 255);
    CHECK(std::get<std::uint64_t>(parse_ok("0755", parse_value).value) == 493);  // octal
    CHECK(std::get<std::uint64_t>(parse_ok("0", parse_value).value) == 0);

    // Full uint64 range (would overflow int64).
    CHECK(std::get<std::uint64_t>(parse_ok("18446744073709551615", parse_value).value) ==
          18446744073709551615ULL);
    // INT64_MIN magnitude is representable when negated.
    CHECK(std::get<std::int64_t>(parse_ok("-9223372036854775808", parse_value).value) == INT64_MIN);
}

TEST_CASE("scalar values: floats and special floats") {
    CHECK(std::get<double>(parse_ok("3.14", parse_value).value) == Catch::Approx(3.14));
    CHECK(std::get<double>(parse_ok("-2.5", parse_value).value) == Catch::Approx(-2.5));
    CHECK(std::get<double>(parse_ok("1.5e3", parse_value).value) == Catch::Approx(1500.0));

    const double pos_inf = std::get<double>(parse_ok("inf", parse_value).value);
    CHECK((std::isinf(pos_inf) && pos_inf > 0));
    const double neg_inf = std::get<double>(parse_ok("-inf", parse_value).value);
    CHECK((std::isinf(neg_inf) && neg_inf < 0));
    CHECK(std::isnan(std::get<double>(parse_ok("nan", parse_value).value)));
    CHECK(std::isnan(std::get<double>(parse_ok("-nan", parse_value).value)));
}

TEST_CASE("scalar values: strings and identifiers are distinct") {
    CHECK(std::get<std::string>(parse_ok(R"("hello")", parse_value).value) == "hello");
    // identifier value (e.g. an enum constant) stays an Identifier, not a string.
    CHECK(std::get<Identifier>(parse_ok("SPEED", parse_value).value).name == "SPEED");
    // a keyword-spelled identifier in value position is still an identifier.
    CHECK(std::get<Identifier>(parse_ok("max", parse_value).value).name == "max");
}

// --- message literals -------------------------------------------------------

TEST_CASE("message literal: brace body with colons and separators") {
    const OptionValue v = parse_ok(R"({ name: "x", count: 3; flag: true })", parse_value);
    const auto& ml = std::get<MessageLiteral>(v.value);
    REQUIRE(ml.fields.size() == 3);
    CHECK(ml.fields[0].name == "name");
    CHECK(ml.fields[0].name_kind == MessageFieldNameKind::Simple);
    CHECK(std::get<std::string>(ml.fields[0].value.value) == "x");
    CHECK(std::get<std::uint64_t>(ml.fields[1].value.value) == 3);
    CHECK(std::get<Identifier>(ml.fields[2].value.value).name == "true");
}

TEST_CASE("message literal: nested message without a colon, and angle brackets") {
    const OptionValue v = parse_ok("{ inner { a: 1 } outer: <b: 2> }", parse_value);
    const auto& ml = std::get<MessageLiteral>(v.value);
    REQUIRE(ml.fields.size() == 2);

    // `inner { ... }` — message-valued field with no colon.
    const auto& inner = std::get<MessageLiteral>(ml.fields[0].value.value);
    REQUIRE(inner.fields.size() == 1);
    CHECK(std::get<std::uint64_t>(inner.fields[0].value.value) == 1);

    // `outer: < ... >` — angle-bracket message literal.
    const auto& outer = std::get<MessageLiteral>(ml.fields[1].value.value);
    CHECK(std::get<std::uint64_t>(outer.fields[0].value.value) == 2);
}

TEST_CASE("message literal: empty body") {
    const OptionValue v = parse_ok("{}", parse_value);
    CHECK(std::get<MessageLiteral>(v.value).fields.empty());
}

TEST_CASE("message literal: extension field-name key") {
    const OptionValue v = parse_ok("{ [my.pkg.ext]: 7 }", parse_value);
    const auto& f = std::get<MessageLiteral>(v.value).fields.at(0);
    CHECK(f.name_kind == MessageFieldNameKind::Extension);
    CHECK(f.name == "my.pkg.ext");
    CHECK(std::get<std::uint64_t>(f.value.value) == 7);
}

TEST_CASE("message literal: Any type-URL field-name key") {
    const OptionValue v = parse_ok("{ [type.googleapis.com/foo.Bar]: { x: 1 } }", parse_value);
    const auto& f = std::get<MessageLiteral>(v.value).fields.at(0);
    CHECK(f.name_kind == MessageFieldNameKind::AnyTypeUrl);
    CHECK(f.name == "type.googleapis.com");
    CHECK(f.any_type == "foo.Bar");
    CHECK(std::get<MessageLiteral>(f.value.value).fields.size() == 1);
}

// --- list literals ----------------------------------------------------------

TEST_CASE("list literal: scalars, empty, and list of messages") {
    const OptionValue list_v = parse_ok("[1, 2, 3]", parse_value);
    const auto& list = std::get<ListLiteral>(list_v.value);
    REQUIRE(list.elements.size() == 3);
    CHECK(std::get<std::uint64_t>(list.elements[2].value) == 3);

    const OptionValue empty_v = parse_ok("[]", parse_value);
    CHECK(std::get<ListLiteral>(empty_v.value).elements.empty());

    const OptionValue msgs_v = parse_ok("[{a: 1}, {a: 2}]", parse_value);
    const auto& msgs = std::get<ListLiteral>(msgs_v.value);
    REQUIRE(msgs.elements.size() == 2);
    CHECK(std::get<std::uint64_t>(
              std::get<MessageLiteral>(msgs.elements[1].value).fields[0].value.value) == 2);
}

TEST_CASE("option decl: message-literal value") {
    const Option o = parse_ok("option (foo) = { a: 1, b: [2, 3] };", parse_option_decl);
    CHECK(o.name[0].is_extension);
    const auto& ml = std::get<MessageLiteral>(o.value.value);
    REQUIRE(ml.fields.size() == 2);
    CHECK(std::get<ListLiteral>(ml.fields[1].value.value).elements.size() == 2);
}

// --- compact options --------------------------------------------------------

TEST_CASE("compact options: multiple entries") {
    const std::vector<Option> opts =
        parse_ok("[packed = true, deprecated = false, (custom) = 1]", parse_compact_options);
    REQUIRE(opts.size() == 3);
    CHECK(opts[0].name[0].name == "packed");
    CHECK(std::get<Identifier>(opts[0].value.value).name == "true");
    CHECK(opts[2].name[0].name == "custom");
    CHECK(opts[2].name[0].is_extension);
    CHECK(std::get<std::uint64_t>(opts[2].value.value) == 1);
}

TEST_CASE("compact options: single entry") {
    const std::vector<Option> opts = parse_ok("[deprecated = true]", parse_compact_options);
    REQUIRE(opts.size() == 1);
    CHECK(opts[0].name[0].name == "deprecated");
}

TEST_CASE("scalar values: float overflow underflows to 0, overflows to inf") {
    CHECK(std::isinf(std::get<double>(parse_ok("1e400", parse_value).value)));
    CHECK(std::get<double>(parse_ok("1e-400", parse_value).value) ==
          Catch::Approx(0.0));  // not inf
    CHECK(std::get<double>(parse_ok("-1e-400", parse_value).value) == Catch::Approx(0.0));
    // a decimal integer past 2^64 falls back to a (huge but finite) float.
    CHECK(std::get<double>(parse_ok("100000000000000000000", parse_value).value) ==
          Catch::Approx(1e20));
}

TEST_CASE("integer literals beyond 64-bit become doubles (protoc-validated schema)") {
    // A decimal integer past int64/uint64 range is only valid for a double-typed
    // option, so it is interpreted as a double rather than rejected.
    CHECK(std::get<double>(parse_ok("-100000000000000000000", parse_value).value) ==
          Catch::Approx(-1e20));
    CHECK(std::get<double>(parse_ok("-9223372036854775809", parse_value).value) ==
          Catch::Approx(-9.223372036854776e18));
}

TEST_CASE("option decl: fully-qualified extension name keeps the leading dot") {
    const Option o = parse_ok("option (.foo.bar) = 1;", parse_option_decl);
    REQUIRE(o.name.size() == 1);
    CHECK(o.name[0].name == ".foo.bar");
    CHECK(o.name[0].is_extension);
}

TEST_CASE("option decl: multiple extension components") {
    const Option o = parse_ok("option (a.b).(c.d).e = 1;", parse_option_decl);
    REQUIRE(o.name.size() == 3);
    CHECK(o.name[0].name == "a.b");
    CHECK(o.name[0].is_extension);
    CHECK(o.name[1].name == "c.d");
    CHECK(o.name[1].is_extension);
    CHECK(o.name[2].name == "e");
    CHECK_FALSE(o.name[2].is_extension);
}

TEST_CASE("message literal: colon-less list-of-messages field") {
    const OptionValue v = parse_ok("{ items [{a: 1}, {a: 2}] }", parse_value);
    const auto& f = std::get<MessageLiteral>(v.value).fields.at(0);
    CHECK(f.name == "items");
    const auto& list = std::get<ListLiteral>(f.value.value);
    REQUIRE(list.elements.size() == 2);
    CHECK(std::get<std::uint64_t>(
              std::get<MessageLiteral>(list.elements[1].value).fields[0].value.value) == 2);
}

TEST_CASE("message literal: scalar field requires a colon, message field may omit it") {
    CHECK(parse_rejects("{ a 1 }", parse_value));               // scalar needs ':'
    CHECK_FALSE(parse_rejects("{ a { b: 1 } }", parse_value));  // message value may omit ':'
}

// --- error cases ------------------------------------------------------------

TEST_CASE("option decl rejects malformed input") {
    CHECK(parse_rejects("option = 5;", parse_option_decl));       // missing name
    CHECK(parse_rejects("option foo 5;", parse_option_decl));     // missing '='
    CHECK(parse_rejects("option foo = 5", parse_option_decl));    // missing ';'
    CHECK(parse_rejects("option (foo = 5;", parse_option_decl));  // unterminated extension name
    CHECK(parse_rejects("option foo = ;", parse_option_decl));    // missing value
}

TEST_CASE("value parser rejects malformed literals") {
    CHECK(parse_rejects("{ a: 1", parse_value));   // unterminated message literal
    CHECK(parse_rejects("[1, 2", parse_value));    // unterminated list
    CHECK(parse_rejects("- ", parse_value));       // sign with no number
    CHECK(parse_rejects("{ : 1 }", parse_value));  // missing field name
}
