#include <catch_amalgamated.hpp>

#include "parse_helpers.hpp"

#include <string>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/features.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

FileNode parse_file_ok(std::string src) {
    return test::parse_file_ok(std::move(src));
}

// resolve_features returns a Result because it refuses an edition whose feature defaults it does not
// know. Every fixture below names a known one, so a failure here is a broken test rather than an
// expected outcome -- the rejection itself is exercised by its own case at the end of this file.
void resolve_ok(FileNode& file) {
    auto resolved = resolve_features(file);
    REQUIRE(resolved.is_ok());
}

}  // namespace

TEST_CASE("features: a file-level feature applies to all fields") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        message M { int32 a = 1; int32 b = 2; }
    )");
    resolve_ok(f);
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
    resolve_ok(f);
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
    resolve_ok(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);  // A
    CHECK(f.messages[1].fields[0].presence == FieldPresence::Explicit);  // B (edition default)
}

TEST_CASE("features: a field-level feature overrides the inherited value") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        message M { int32 a = 1; int32 b = 2 [features.field_presence = EXPLICIT]; }
    )");
    resolve_ok(f);
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
    resolve_ok(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);
    CHECK(f.messages[0].nested_messages[0].fields[0].presence == FieldPresence::Implicit);
}

TEST_CASE("features: proto2 and proto3 files are unaffected (no-op)") {
    FileNode p2 = parse_file_ok(R"(syntax = "proto2"; message M { optional int32 a = 1; })");
    resolve_ok(p2);
    CHECK(p2.messages[0].fields[0].presence == FieldPresence::Explicit);  // proto2 optional

    FileNode p3 = parse_file_ok(R"(syntax = "proto3"; message M { int32 a = 1; })");
    resolve_ok(p3);
    CHECK(p3.messages[0].fields[0].presence == FieldPresence::Implicit);  // proto3 scalar unchanged
}

TEST_CASE("features: enum_type openness inherits and overrides") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        enum E { A = 0; }
        message M { enum F { option features.enum_type = CLOSED; B = 0; } }
    )");
    resolve_ok(f);
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
    resolve_ok(f);
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
    resolve_ok(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Implicit);  // message-level
    CHECK(f.messages[0].enums[0].openness == EnumOpenness::Closed);      // file-level inherited
}

TEST_CASE("features: oneof members stay explicit even under IMPLICIT") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        message M { oneof o { int32 a = 1; } }
    )");
    resolve_ok(f);
    CHECK(f.messages[0].oneofs[0].fields[0].presence == FieldPresence::Explicit);
}

TEST_CASE("features: repeated encoding is not forced on non-packable types") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.repeated_field_encoding = PACKED;
        message M { repeated string s = 1; }
    )");
    resolve_ok(f);
    CHECK(f.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Expanded);  // string stays
}

TEST_CASE("features: LEGACY_REQUIRED resolves to Required") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        message M { int32 a = 1 [features.field_presence = LEGACY_REQUIRED]; }
    )");
    resolve_ok(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Required);
}

TEST_CASE("features: edition 2024 uses the same decode defaults as 2023") {
    // All four decode-relevant defaults, so the case is the parity pin its name promises --
    // asserting presence alone left the other three free to drift between the two editions.
    FileNode f = parse_file_ok(R"(edition = "2024"; message M {
        int32 a = 1;
        repeated int32 r = 2;
        E e = 3;
        M m = 4;
        enum E { X = 0; }
    })");
    resolve_ok(f);
    CHECK(f.messages[0].fields[0].presence == FieldPresence::Explicit);  // 2024 default EXPLICIT
    CHECK(f.messages[0].fields[1].repeated_encoding == RepeatedEncoding::Packed);
    CHECK(f.messages[0].fields[3].message_encoding == MessageEncoding::LengthPrefixed);
    CHECK(f.messages[0].enums[0].openness == EnumOpenness::Open);
}

TEST_CASE("features: the aggregate 'option features = { ... }' form is honored") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features = { field_presence: IMPLICIT enum_type: CLOSED };
        message M { int32 a = 1; enum E { X = 0; } }
    )");
    resolve_ok(f);
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
    resolve_ok(f);
    // Inner inherits Outer's EXPLICIT, not the file's IMPLICIT.
    CHECK(f.messages[0].nested_messages[0].fields[0].presence == FieldPresence::Explicit);
}

TEST_CASE("features: extend-block fields inherit file features") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.field_presence = IMPLICIT;
        extend Foo { int32 bar = 100; }
    )");
    resolve_ok(f);
    CHECK(f.extends[0].fields[0].presence == FieldPresence::Implicit);
}

TEST_CASE("features: file-level message_encoding reaches a field") {
    FileNode f = parse_file_ok(R"(
        edition = "2023";
        option features.message_encoding = DELIMITED;
        message M { M child = 1; }
    )");
    resolve_ok(f);
    CHECK(f.messages[0].fields[0].message_encoding == MessageEncoding::Delimited);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one assertion set over five editions
TEST_CASE("features: an edition with unknown defaults is rejected, not assumed") {
    // Every known edition happens to share one set of decode-relevant defaults, so assuming them
    // for an unrecognized edition would "work" until the edition that changes one -- and then
    // decode by the wrong rules with no diagnostic. protoc rejects an unknown edition too, so no
    // protoc-valid schema reaches this.
    for (const char* edition : {"2022", "2025", "9999", "", "202x"}) {
        FileNode f = parse_file_ok(R"(edition = ")" + std::string(edition) +
                                   R"("; message M { int32 a = 1; })");
        auto resolved = resolve_features(f);
        REQUIRE(resolved.is_err());
        CHECK(resolved.error().message.find("unknown edition") != std::string::npos);
        // The message names the editions that ARE known, so the reader learns what to do next.
        CHECK(resolved.error().message.find("2023") != std::string::npos);
        // Anchored where FileNode recorded the edition string (the two reads of one value can
        // only diverge if resolve_features stops reading file.edition_offset).
        CHECK(resolved.error().byte_offset == f.edition_offset);
        CHECK(f.edition_offset > 0);
    }
}

TEST_CASE("features: every known edition resolves") {
    // Catches a REMOVAL from features.cpp's list (an edition the fixtures assume going dark).
    // An ADDITION is caught by the unknown-edition case below, whose rejection list includes
    // "2025" -- this loop iterates its own hardcoded editions and cannot see a new one.
    for (const char* edition : {"2023", "2024"}) {
        FileNode f = parse_file_ok(R"(edition = ")" + std::string(edition) +
                                   R"("; message M { int32 a = 1; })");
        auto resolved = resolve_features(f);
        CHECK(resolved.is_ok());
    }
}
