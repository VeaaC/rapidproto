#include <catch_amalgamated.hpp>

#include <string>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

// Lex `src`, parse the whole file, require success + full consumption, return the FileNode.
FileNode file_ok(std::string src) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    return std::move(r.value().value);
}

MessageNode message_ok(std::string src, SyntaxLevel syntax) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    ParseContext ctx;
    ctx.syntax_level = syntax;
    auto r = parse_message(Range<Token>(lexed.tokens), ctx);
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    return std::move(r.value().value);
}

}  // namespace

// --- syntax / file level ----------------------------------------------------

TEST_CASE("file: syntax declaration sets the level; absent => proto2") {
    CHECK(file_ok("").syntax_level == SyntaxLevel::Proto2);  // empty file
    CHECK(file_ok(R"(syntax = "proto3";)").syntax_level == SyntaxLevel::Proto3);
    CHECK(file_ok(R"(syntax = "proto2";)").syntax_level == SyntaxLevel::Proto2);
    CHECK(file_ok("message M {}").syntax_level == SyntaxLevel::Proto2);  // no decl
}

TEST_CASE("file: edition declaration") {
    const FileNode f = file_ok(R"(edition = "2023"; message M {})");
    CHECK(f.syntax_level == SyntaxLevel::Edition);
    CHECK(f.edition == "2023");
    REQUIRE(f.messages.size() == 1);
}

TEST_CASE("file: package, imports, and options") {
    const FileNode f = file_ok(R"(
        syntax = "proto3";
        package my.pkg;
        import "other.proto";
        import public "pub.proto";
        import weak "weak.proto";
        option java_package = "com.x";
        message M {}
    )");
    CHECK(f.package == "my.pkg");
    REQUIRE(f.imports.size() == 3);
    CHECK(f.imports[0].path == "other.proto");
    CHECK(f.imports[0].kind == ImportKind::Standard);
    CHECK(f.imports[1].kind == ImportKind::Public);
    CHECK(f.imports[2].kind == ImportKind::Weak);
    REQUIRE(f.options.size() == 1);
    CHECK(f.options[0].name[0].name == "java_package");
    REQUIRE(f.messages.size() == 1);
}

TEST_CASE("file: import option (editions)") {
    const FileNode f = file_ok(R"(edition = "2024"; import option "features.proto";)");
    REQUIRE(f.imports.size() == 1);
    CHECK(f.imports[0].kind == ImportKind::Option);
}

TEST_CASE("file: top-level enums and extends") {
    const FileNode f = file_ok(R"(
        syntax = "proto2";
        enum E { A = 0; }
        extend Foo { optional int32 bar = 100; }
    )");
    REQUIRE(f.enums.size() == 1);
    CHECK(f.enums[0].name == "E");
    REQUIRE(f.extends.size() == 1);
    CHECK(f.extends[0].extendee_type_name == "Foo");
    CHECK(f.extends[0].fields[0].number == 100);
}

// --- services are dropped ---------------------------------------------------

TEST_CASE("file: service blocks are parsed past and dropped") {
    const FileNode f = file_ok(R"(
        syntax = "proto3";
        message Req {}
        message Resp {}
        service S {
            rpc Unary(Req) returns (Resp);
            rpc Streaming(stream Req) returns (stream Resp) { option deprecated = true; }
        }
        message After {}
    )");
    // No ServiceNode exists; the sibling messages still parse.
    REQUIRE(f.messages.size() == 3);
    CHECK(f.messages[0].name == "Req");
    CHECK(f.messages[2].name == "After");
}

// --- message bodies ---------------------------------------------------------

TEST_CASE("message: a mix of element kinds") {
    const MessageNode m = message_ok(R"(
        message M {
            option deprecated = true;
            int32 a = 1;
            repeated string b = 2;
            map<string, int32> c = 3;
            oneof choice { int32 x = 4; Foo y = 5; }
            enum Inner { Z = 0; }
            message Nested { int32 q = 1; }
            reserved 6, 7 to 9;
            reserved "old";
            extensions 100 to 200;
        }
    )",
                                     SyntaxLevel::Proto2);
    CHECK(m.name == "M");
    REQUIRE(m.options.size() == 1);
    REQUIRE(m.fields.size() == 2);
    CHECK(m.fields[0].name == "a");
    CHECK(m.fields[1].is_repeated);
    REQUIRE(m.map_fields.size() == 1);
    CHECK(m.map_fields[0].name == "c");
    REQUIRE(m.oneofs.size() == 1);
    CHECK(m.oneofs[0].fields.size() == 2);
    REQUIRE(m.enums.size() == 1);
    REQUIRE(m.nested_messages.size() == 1);
    CHECK(m.nested_messages[0].name == "Nested");
    REQUIRE(m.reserved.size() == 2);
    REQUIRE(m.extension_ranges.size() == 1);
    CHECK(m.extension_ranges[0].ranges[0].start == 100);
    CHECK(m.extension_ranges[0].ranges[0].end == 200);
}

TEST_CASE("message: extension ranges with 'to max' and compact options") {
    const MessageNode m =
        message_ok("message M { extensions 4, 8 to 12, 1000 to max [declaration = {}]; }",
                   SyntaxLevel::Proto2);
    REQUIRE(m.extension_ranges.size() == 1);
    const auto& r = m.extension_ranges[0].ranges;
    REQUIRE(r.size() == 3);
    CHECK(r[0].start == 4);
    CHECK(r[0].end == 4);
    CHECK(r[1].end == 12);
    CHECK(r[2].end == kMaxMessageFieldNumber);  // 'max' = 2^29-1 for message fields
    REQUIRE(m.extension_ranges[0].options.size() == 1);
}

TEST_CASE("message: deeply nested messages (3+ levels)") {
    const MessageNode m = message_ok("message A { message B { message C { int32 deep = 1; } } }",
                                     SyntaxLevel::Proto3);
    REQUIRE(m.nested_messages.size() == 1);
    const MessageNode& b = m.nested_messages[0];
    REQUIRE(b.nested_messages.size() == 1);
    const MessageNode& c = b.nested_messages[0];  // 3rd level holds the field
    REQUIRE(c.fields.size() == 1);
    CHECK(c.fields[0].name == "deep");
}

TEST_CASE("message: visibility modifier") {
    CHECK(message_ok("export message M {}", SyntaxLevel::Edition).visibility == Visibility::Export);
    CHECK(message_ok("local message M {}", SyntaxLevel::Edition).visibility == Visibility::Local);
    CHECK(message_ok("message M {}", SyntaxLevel::Edition).visibility == Visibility::Default);
}

TEST_CASE("message: nested extend") {
    const MessageNode m =
        message_ok("message M { extend Other { optional int32 e = 50; } }", SyntaxLevel::Proto2);
    REQUIRE(m.extends.size() == 1);
    CHECK(m.extends[0].extendee_type_name == "Other");
    CHECK(m.extends[0].fields[0].number == 50);
}

// --- groups -----------------------------------------------------------------

TEST_CASE("message: group synthesizes a nested message and a lowercased is_group field") {
    const MessageNode m = message_ok("message M { repeated group Result = 1 { int32 score = 2; } }",
                                     SyntaxLevel::Proto2);
    // synthesized nested message keeps the capitalized name
    REQUIRE(m.nested_messages.size() == 1);
    CHECK(m.nested_messages[0].name == "Result");
    CHECK(m.nested_messages[0].fields[0].name == "score");
    // field is lowercased, references the group type, is_group, delimited, repeated
    REQUIRE(m.fields.size() == 1);
    CHECK(m.fields[0].name == "result");
    CHECK(m.fields[0].type_name == "Result");
    CHECK(m.fields[0].number == 1);
    CHECK(m.fields[0].is_group);
    CHECK(m.fields[0].message_encoding == MessageEncoding::Delimited);
    CHECK(m.fields[0].is_repeated);
}

TEST_CASE("message: nested group inside a group") {
    const MessageNode m = message_ok(
        "message M { optional group Outer = 1 { optional group Inner = 2 { int32 v = 3; } } }",
        SyntaxLevel::Proto2);
    REQUIRE(m.nested_messages.size() == 1);
    const MessageNode& outer = m.nested_messages[0];
    CHECK(outer.name == "Outer");
    REQUIRE(outer.fields.size() == 1);
    CHECK(outer.fields[0].name == "inner");
    CHECK(outer.fields[0].is_group);
    REQUIRE(outer.nested_messages.size() == 1);  // Inner's message hoisted into Outer's message
    CHECK(outer.nested_messages[0].name == "Inner");
}

TEST_CASE("file: group inside a file-level extend hoists its message to the file") {
    const FileNode f = file_ok("extend Foo { optional group G = 100 { int32 v = 1; } }");
    REQUIRE(f.extends.size() == 1);
    CHECK(f.extends[0].fields[0].name == "g");
    CHECK(f.extends[0].fields[0].is_group);
    REQUIRE(f.messages.size() == 1);  // synthesized group message hoisted to file scope
    CHECK(f.messages[0].name == "G");
}

TEST_CASE("message: nested enum visibility (editions)") {
    const MessageNode m =
        message_ok("message M { export enum E { A = 0; } }", SyntaxLevel::Edition);
    REQUIRE(m.enums.size() == 1);
    CHECK(m.enums[0].visibility == Visibility::Export);
}

TEST_CASE("file: complete proto3 file end to end") {
    const FileNode f = file_ok(R"(
        syntax = "proto3";
        package a.b;
        message M {
            int32 scalar = 1;
            optional int32 opt = 2;
            repeated int64 nums = 3;
            map<string, M> children = 4;
            oneof pick { string s = 5; M m = 6; }
        }
    )");
    CHECK(f.syntax_level == SyntaxLevel::Proto3);
    CHECK(f.package == "a.b");
    REQUIRE(f.messages.size() == 1);
    const MessageNode& m = f.messages[0];
    REQUIRE(m.fields.size() == 3);
    CHECK(m.fields[0].presence == FieldPresence::Implicit);            // proto3 scalar
    CHECK(m.fields[1].presence == FieldPresence::Explicit);            // optional
    CHECK(m.fields[2].repeated_encoding == RepeatedEncoding::Packed);  // proto3 repeated int64
    CHECK(m.map_fields.size() == 1);
    CHECK(m.oneofs.size() == 1);
}

TEST_CASE("message: group inside extend emits its message to the enclosing scope") {
    const MessageNode m =
        message_ok("message M { extend Foo { optional group G = 100 { int32 v = 1; } } }",
                   SyntaxLevel::Proto2);
    REQUIRE(m.extends.size() == 1);
    // the group field lives in the extend...
    REQUIRE(m.extends[0].fields.size() == 1);
    CHECK(m.extends[0].fields[0].name == "g");
    CHECK(m.extends[0].fields[0].is_group);
    // ...but its synthesized message is hoisted to the enclosing message
    REQUIRE(m.nested_messages.size() == 1);
    CHECK(m.nested_messages[0].name == "G");
}
