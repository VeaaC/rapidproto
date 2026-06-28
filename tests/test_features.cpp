#include <catch_amalgamated.hpp>

#include <string>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/features.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

FileNode parse_file_ok(std::string src) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    return std::move(r.value().value);
}

}  // namespace

TEST_CASE("features: a file-level feature applies to all fields") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        message M { int32 a = 1; int32 b = 2; }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);
    CHECK(f.messages[0].fields[1].presence == FieldPresence::Implicit);
}

TEST_CASE("features: explicitly setting features to their edition defaults is accepted") {
    // Covers the "set the feature to the value the edition already defaults to" branch of every
    // feature -- distinct from the non-default overrides tested elsewhere.
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = EXPLICIT;
        option features.enum_type = OPEN;
        option features.message_encoding = LENGTH_PREFIXED;
        option features.repeated_field_encoding = PACKED;
        option features.utf8_validation = VERIFY;
        message M {
            int32 x = 1;
            repeated int32 r = 2;
            enum E { Z = 0; }
        }
    )");
    resolve_features(f);
    const MessageNode& m = f.messages[0];
    CHECK(m.fields[0].presence == FieldPresence::Explicit);
    CHECK(m.fields[1].repeated_encoding == RepeatedEncoding::Packed);
    CHECK(m.enums[0].openness == EnumOpenness::Open);
}

TEST_CASE("features: a message-level feature overrides its fields but not siblings") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        message A { option features.field_presence = IMPLICIT; int32 x = 1; }
        message B { int32 y = 1; }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);  // A
    CHECK(f.messages[1].fields[0].presence == FieldPresence::Explicit);  // B (edition default)
}

TEST_CASE("features: a field-level feature overrides the inherited value") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        message M { int32 a = 1; int32 b = 2 [features.field_presence = EXPLICIT]; }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);  // inherits file
    CHECK(f.messages[0].fields[1].presence == FieldPresence::Explicit);  // field override
}

TEST_CASE("features: nested messages inherit from the enclosing message") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        message Outer {
            option features.field_presence = IMPLICIT;
            int32 a = 1;
            message Inner { int32 b = 1; }
        }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);
    CHECK(f.messages[0].nested_messages[0].fields[0].presence == FieldPresence::Implicit);
}

TEST_CASE("features: proto2 and proto3 files are unaffected (no-op)") {
    FileNode p2 = parse_file_ok(R"(syntax = "proto2"; message M { optional int32 a = 1; })");
    resolve_features(p2);
    CHECK(p2.messages[0].fields[0].presence == FieldPresence::Explicit);  // proto2 optional

    FileNode p3 = parse_file_ok(R"(syntax = "proto3"; message M { int32 a = 1; })");
    resolve_features(p3);
    CHECK(p3.messages[0].fields[0].presence == FieldPresence::Implicit);  // proto3 scalar unchanged
}

TEST_CASE("features: enum_type openness inherits and overrides") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        enum E { A = 0; }
        message M { enum F { option features.enum_type = CLOSED; B = 0; } }
    )");
    resolve_features(f);
    CHECK(f.enums[0].openness == EnumOpenness::Open);                // edition default
    CHECK(f.messages[0].enums[0].openness == EnumOpenness::Closed);  // enum-level override
}

TEST_CASE("features: message_encoding and repeated_field_encoding") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        message M {
            repeated int32 nums = 1 [features.repeated_field_encoding = EXPANDED];
            M child = 2 [features.message_encoding = DELIMITED];
        }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Expanded);
    CHECK(f.messages[0].fields[1].message_encoding == MessageEncoding::Delimited);
}

TEST_CASE("features: multiple features set at different levels simultaneously") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.enum_type = CLOSED;
        message M {
            option features.field_presence = IMPLICIT;
            int32 a = 1;
            enum E { X = 0; }
        }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);  // message-level
    CHECK(f.messages[0].enums[0].openness == EnumOpenness::Closed);      // file-level inherited
}

TEST_CASE("features: oneof members stay explicit even under IMPLICIT") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        message M { oneof o { int32 a = 1; } }
    )");
    resolve_features(f);
    CHECK(f.messages[0].oneofs[0].fields[0].presence == FieldPresence::Explicit);
}

TEST_CASE("features: repeated encoding is not forced on non-packable types") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.repeated_field_encoding = PACKED;
        message M { repeated string s = 1; }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Expanded);  // string stays
}

TEST_CASE("features: LEGACY_REQUIRED resolves to Required") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        message M { int32 a = 1 [features.field_presence = LEGACY_REQUIRED]; }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Required);
}

TEST_CASE("features: edition 2024 uses the same decode defaults as 2023") {
    FileNode f = parse_file_ok(R"(edition = "2024"; message M { int32 a = 1; })");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Explicit);  // 2024 default EXPLICIT
}

TEST_CASE("features: the aggregate 'option features = { ... }' form is honored") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features = { field_presence: IMPLICIT enum_type: CLOSED };
        message M { int32 a = 1; enum E { X = 0; } }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);
    CHECK(f.messages[0].enums[0].openness == EnumOpenness::Closed);
}

TEST_CASE("features: a mid-chain message re-override beats the file level") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        message Outer {
            option features.field_presence = EXPLICIT;
            message Inner { int32 deep = 1; }
        }
    )");
    resolve_features(f);
    // Inner inherits Outer's EXPLICIT, not the file's IMPLICIT.
    CHECK(f.messages[0].nested_messages[0].fields[0].presence == FieldPresence::Explicit);
}

TEST_CASE("features: extend-block fields inherit file features") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        extend Foo { int32 bar = 100; }
    )");
    resolve_features(f);
    CHECK(f.extends[0].fields[0].presence == FieldPresence::Implicit);
}

TEST_CASE("features: file-level message_encoding reaches a field") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.message_encoding = DELIMITED;
        message M { M child = 1; }
    )");
    resolve_features(f);
    CHECK(f.messages[0].fields[0].message_encoding == MessageEncoding::Delimited);
}
