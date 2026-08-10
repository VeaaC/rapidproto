#include <catch_amalgamated.hpp>

#include <cstddef>
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

template <typename Fn>
auto parse_ok(std::string src, Fn fn) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = fn(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    return std::move(r.value().value);
}

// True if the message body fails to parse, either outright or by leaving input unconsumed.
bool rejects(const std::string& body) {
    auto lr = lex("message Holder { " + body + " }");
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    const ParseContext ctx;
    auto r = parse_message(Range<Token>(lexed.tokens), ctx);
    return !r.is_ok() || !r.value().remaining.empty();
}

// Index of the first cardinality keyword, i.e. where a oneof label error must be reported.
std::size_t first_label_token(const std::vector<Token>& tokens) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const TokenKind k = tokens[i].kind;
        if (k == TokenKind::KwOptional || k == TokenKind::KwRepeated ||
            k == TokenKind::KwRequired) {
            return i;
        }
    }
    return tokens.size();
}

// A oneof, parsed through its enclosing message: the oneof parser is internal to parser.cpp
// because a `group` member synthesizes a message that belongs to the MESSAGE, not the oneof.
MessageNode message_ok(const std::string& body) {
    const ParseContext ctx;  // proto2: the only syntax level whose grammar admits `group`
    return parse_ok("message Holder { " + body + " }",
                    [&](Range<Token> in) { return parse_message(in, ctx); });
}

OneofNode oneof_ok(const std::string& src) {
    MessageNode m = message_ok(src);
    REQUIRE(m.oneofs.size() == 1);
    return std::move(m.oneofs[0]);
}

FieldNode field_ok(std::string src, SyntaxLevel syntax) {
    ParseContext ctx;
    ctx.syntax_level = syntax;
    return parse_ok(std::move(src), [&](Range<Token> in) { return parse_field(in, ctx); });
}

bool field_rejects(std::string src, SyntaxLevel syntax) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    ParseContext ctx;
    ctx.syntax_level = syntax;
    auto r = parse_field(Range<Token>(lexed.tokens), ctx);
    return r.is_err() || !r.value().remaining.empty();
}

}  // namespace

// --- presence truth table ---------------------------------------------------

TEST_CASE("field presence: proto2") {
    CHECK(field_ok("required int32 x = 1;", SyntaxLevel::Proto2).presence ==
          FieldPresence::Required);
    CHECK(field_ok("optional int32 x = 1;", SyntaxLevel::Proto2).presence ==
          FieldPresence::Explicit);
    CHECK(field_ok("int32 x = 1;", SyntaxLevel::Proto2).presence == FieldPresence::Explicit);

    const FieldNode rep = field_ok("repeated int32 x = 1;", SyntaxLevel::Proto2);
    CHECK(rep.is_repeated);
}

TEST_CASE("field presence: proto3") {
    // No keyword + scalar -> implicit.
    CHECK(field_ok("int32 x = 1;", SyntaxLevel::Proto3).presence == FieldPresence::Implicit);
    // `optional` -> explicit (hazzer).
    CHECK(field_ok("optional int32 x = 1;", SyntaxLevel::Proto3).presence ==
          FieldPresence::Explicit);
    // No keyword + message/enum type: implicit at parse time; the type-resolution pass promotes
    // message-typed fields to explicit once the reference is resolved.
    CHECK(field_ok("Foo x = 1;", SyntaxLevel::Proto3).presence == FieldPresence::Implicit);

    const FieldNode rep = field_ok("repeated int32 x = 1;", SyntaxLevel::Proto3);
    CHECK(rep.is_repeated);
}

TEST_CASE("field presence: editions default is explicit") {
    // Editions default field_presence is EXPLICIT; the feature pass refines it.
    CHECK(field_ok("int32 x = 1;", SyntaxLevel::Edition).presence == FieldPresence::Explicit);
}

// --- repeated encoding truth table -----------------------------------------

TEST_CASE("repeated encoding: packable scalar defaults") {
    CHECK(field_ok("repeated int32 x = 1;", SyntaxLevel::Proto2).repeated_encoding ==
          RepeatedEncoding::Expanded);
    CHECK(field_ok("repeated int32 x = 1;", SyntaxLevel::Proto3).repeated_encoding ==
          RepeatedEncoding::Packed);
    CHECK(field_ok("repeated int32 x = 1;", SyntaxLevel::Edition).repeated_encoding ==
          RepeatedEncoding::Packed);
}

TEST_CASE("repeated encoding: non-packable types are expanded") {
    CHECK(field_ok("repeated string x = 1;", SyntaxLevel::Proto3).repeated_encoding ==
          RepeatedEncoding::Expanded);
    CHECK(field_ok("repeated bytes x = 1;", SyntaxLevel::Proto3).repeated_encoding ==
          RepeatedEncoding::Expanded);
    // Message/unknown type: expanded at parse time (enums refined by the type-resolution pass).
    CHECK(field_ok("repeated Foo x = 1;", SyntaxLevel::Proto3).repeated_encoding ==
          RepeatedEncoding::Expanded);
    CHECK(field_ok("repeated Foo x = 1;", SyntaxLevel::Proto2).repeated_encoding ==
          RepeatedEncoding::Expanded);
}

TEST_CASE("field: editions over-permissively accepts optional/required keywords") {
    // Editions removed these keywords; the parser accepts them (parser-only). Editions presence
    // is owned by the feature pass regardless.
    CHECK(field_ok("required int32 x = 1;", SyntaxLevel::Edition).presence ==
          FieldPresence::Required);
    CHECK(field_ok("optional int32 x = 1;", SyntaxLevel::Edition).presence ==
          FieldPresence::Explicit);
}

TEST_CASE("field: numbers are not range-checked (parser only)") {
    CHECK(field_ok("int32 x = 19000;", SyntaxLevel::Proto3).number == 19000);  // reserved range
    CHECK(field_ok("int32 x = 536870911;", SyntaxLevel::Proto3).number == 536870911);  // 2^29 - 1
}

// --- type names, numbers, options ------------------------------------------

TEST_CASE("field: scalar/qualified/leading-dot type names retained as written") {
    CHECK(field_ok("int32 x = 1;", SyntaxLevel::Proto3).type_name == "int32");
    CHECK(field_ok("foo.Bar x = 1;", SyntaxLevel::Proto3).type_name == "foo.Bar");
    CHECK(field_ok(".foo.Bar x = 1;", SyntaxLevel::Proto3).type_name == ".foo.Bar");
}

TEST_CASE("field: number and compact options") {
    const FieldNode f =
        field_ok("repeated int32 x = 5 [packed = false, deprecated = true];", SyntaxLevel::Proto3);
    CHECK(f.name == "x");
    CHECK(f.number == 5);
    REQUIRE(f.options.size() == 2);
    CHECK(f.options[0].name[0].name == "packed");
    CHECK(f.options[1].name[0].name == "deprecated");
}

TEST_CASE("field: keyword-spelled field names are allowed") {
    CHECK(field_ok("int32 message = 1;", SyntaxLevel::Proto3).name == "message");
    CHECK(field_ok("int32 optional = 1;", SyntaxLevel::Proto2).name == "optional");
}

TEST_CASE("field: malformed declarations are rejected") {
    CHECK(field_rejects("int32 x 1;", SyntaxLevel::Proto3));     // missing '='
    CHECK(field_rejects("int32 = 1;", SyntaxLevel::Proto3));     // missing name
    CHECK(field_rejects("int32 x = 1", SyntaxLevel::Proto3));    // missing ';'
    CHECK(field_rejects("int32 x = -1;", SyntaxLevel::Proto3));  // field number cannot be negative
}

// --- maps -------------------------------------------------------------------

TEST_CASE("map field: key/value types, number, options") {
    const MapFieldNode m =
        parse_ok("map<string, Foo> entries = 7 [deprecated = true];", parse_map_field);
    CHECK(m.name == "entries");
    CHECK(m.key_type == "string");
    CHECK(m.value_type == "Foo");
    CHECK(m.number == 7);
    REQUIRE(m.options.size() == 1);
    CHECK(m.options[0].name[0].name == "deprecated");
}

TEST_CASE("map field: scalar value and qualified value types") {
    CHECK(parse_ok("map<int32, int64> m = 1;", parse_map_field).value_type == "int64");
    CHECK(parse_ok("map<int64, foo.Bar> m = 1;", parse_map_field).key_type == "int64");
    CHECK(parse_ok("map<int64, .foo.Bar> m = 1;", parse_map_field).value_type == ".foo.Bar");
    CHECK(parse_ok("map<sint32, int32> m = 1;", parse_map_field).key_type == "sint32");
    CHECK(parse_ok("map<fixed64, int32> m = 1;", parse_map_field).key_type == "fixed64");
}

TEST_CASE("map field: key-type validity is not enforced (parser only)") {
    // map<Foo, Bar> is an invalid key per the spec, but the parser stores it as written;
    // key-type validity is a protoc/validation concern, not the parser's.
    const MapFieldNode m = parse_ok("map<Foo, Bar> m = 1;", parse_map_field);
    CHECK(m.key_type == "Foo");
    CHECK(m.value_type == "Bar");
}

TEST_CASE("map field: malformed declarations are rejected") {
    auto rejects = [](std::string src) {
        auto lr = lex(std::move(src));
        REQUIRE(lr.is_ok());
        const LexResult lexed = std::move(lr).value();
        auto r = parse_map_field(Range<Token>(lexed.tokens));
        return r.is_err() || !r.value().remaining.empty();
    };
    CHECK(rejects("map<string> m = 1;"));         // missing value type
    CHECK(rejects("map string, int32> m = 1;"));  // missing '<'
    CHECK(rejects("map<string, int32> m = 1"));   // missing ';'
}

// --- oneofs -----------------------------------------------------------------

TEST_CASE("oneof: fields are always explicit, with options") {
    const OneofNode o = oneof_ok(
        "oneof choice { option deprecated = true; int32 a = 1; Foo b = 2; string c = 3; }");
    CHECK(o.name == "choice");
    REQUIRE(o.options.size() == 1);
    CHECK(o.options[0].name[0].name == "deprecated");
    REQUIRE(o.fields.size() == 3);
    CHECK(o.fields[0].name == "a");
    CHECK(o.fields[0].number == 1);
    CHECK(o.fields[1].type_name == "Foo");
    // oneof members always explicit, never repeated
    CHECK(o.fields[0].presence == FieldPresence::Explicit);
    CHECK(o.fields[1].presence == FieldPresence::Explicit);
    CHECK(o.fields[2].presence == FieldPresence::Explicit);
    CHECK_FALSE(o.fields[0].is_repeated);
}

TEST_CASE("oneof: the body follows the spec's OneofElement, which has no empty statement") {
    // MessageElement lists EmptyDecl; OneofElement does not, and protoc agrees ("Expected type
    // name."). An empty oneof BODY is still fine -- `{ OneofElement }` admits zero elements, so
    // there the grammar is looser than protoc, and we follow the grammar.
    CHECK(rejects("oneof p { int32 i = 1; ; }"));
    CHECK(rejects("oneof p { ; }"));
    CHECK_FALSE(rejects("oneof p { }"));  // grammar-legal; protoc enforces a prose rule we do not
    // The other half of the claim: MessageElement DOES list EmptyDecl, so `;` stays legal there.
    CHECK_FALSE(rejects("; int32 a = 1; ;"));
    // The cardinality keywords are subtracted from a oneof field's LEADING identifier only, so a
    // `.`-rooted fully-qualified name starting with one is still a legal type -- the hand-rolled
    // token guard must not swallow it.
    CHECK_FALSE(rejects("oneof p { .optional.Foo f = 1; }"));
}

TEST_CASE("oneof: a group member synthesizes a message in the ENCLOSING scope") {
    // protoc accepts `group` inside a oneof. A oneof is not a scope, so the synthesized message
    // belongs to the message while the delimited field joins the oneof.
    const MessageNode m =
        message_ok("oneof pick { int32 i = 1; group G = 2 { optional int32 x = 3; } }");
    REQUIRE(m.oneofs.size() == 1);
    REQUIRE(m.oneofs[0].fields.size() == 2);
    CHECK(m.oneofs[0].fields[0].name == "i");
    const FieldNode& group_field = m.oneofs[0].fields[1];
    CHECK(group_field.name == "g");       // the field is the lowercased group name
    CHECK(group_field.type_name == "G");  // ... referring to the synthesized message
    CHECK(group_field.number == 2);
    CHECK(group_field.is_group);
    CHECK(group_field.message_encoding == MessageEncoding::Delimited);
    CHECK(group_field.presence == FieldPresence::Explicit);  // every oneof member is explicit
    CHECK_FALSE(group_field.is_repeated);
    // The message lands beside the oneof, not inside it.
    REQUIRE(m.nested_messages.size() == 1);
    CHECK(m.nested_messages[0].name == "G");
    REQUIRE(m.nested_messages[0].fields.size() == 1);
    CHECK(m.nested_messages[0].fields[0].name == "x");
}

TEST_CASE("oneof: a member must not carry a label") {
    // protoc: "Fields in oneofs must not have labels", and the spec forbids it syntactically --
    // there is no cardinality slot in either oneof production. parse_group accepts one at message
    // scope (where the spec REQUIRES it), so the oneof body has to refuse it explicitly.
    for (const char* src :
         {"oneof p { optional group G = 2 { optional int32 x = 3; } }",
          "oneof p { repeated group G = 2 { optional int32 x = 3; } }",
          "oneof p { required group G = 2 { optional int32 x = 3; } }",
          "oneof p { optional int32 y = 1; }", "oneof p { repeated int32 y = 1; }"}) {
        auto lr = lex("message Holder { " + std::string(src) + " }");
        REQUIRE(lr.is_ok());
        const LexResult lexed = std::move(lr).value();
        const ParseContext ctx;
        auto r = parse_message(Range<Token>(lexed.tokens), ctx);
        REQUIRE(r.is_err());
        // Pin the POSITION, not just the rejection: parser offsets are token indices, and the
        // error must land on the label itself rather than at the brace or at end of input.
        CHECK(r.error().byte_offset == first_label_token(lexed.tokens));
        CHECK(r.error().message.find("must not have a label") != std::string::npos);
    }
}

TEST_CASE("oneof: field-level compact options") {
    const OneofNode o = oneof_ok("oneof c { int32 a = 1 [deprecated = true]; }");
    REQUIRE(o.fields.size() == 1);
    REQUIRE(o.fields[0].options.size() == 1);
    CHECK(o.fields[0].options[0].name[0].name == "deprecated");
}
