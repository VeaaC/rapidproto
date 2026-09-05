#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>

#include "parse_helpers.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/wellknown.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

TEST_CASE("wellknown: all 11 well-known types are embedded") {
    const std::string_view paths[] = {
        "google/protobuf/any.proto",
        "google/protobuf/api.proto",
        "google/protobuf/descriptor.proto",
        "google/protobuf/duration.proto",
        "google/protobuf/empty.proto",
        "google/protobuf/field_mask.proto",
        "google/protobuf/source_context.proto",
        "google/protobuf/struct.proto",
        "google/protobuf/timestamp.proto",
        "google/protobuf/type.proto",
        "google/protobuf/wrappers.proto",
    };
    for (const auto path : paths) {
        const auto src = wellknown_source(path);
        REQUIRE(src.has_value());
        CHECK_FALSE(src.value_or(std::string_view{}).empty());  // value_or: clang-tidy-safe
    }
}

TEST_CASE("wellknown: unknown paths return nullopt") {
    CHECK_FALSE(wellknown_source("google/protobuf/unknown.proto").has_value());
    CHECK_FALSE(wellknown_source("descriptor.proto").has_value());  // must be the canonical path
    CHECK_FALSE(wellknown_source("").has_value());
}

TEST_CASE("wellknown: embedded descriptor.proto is itself parseable") {
    const auto src = wellknown_source("google/protobuf/descriptor.proto");
    REQUIRE(src.has_value());
    const FileNode file =
        test::parse_file_ok(std::string(src.value_or(std::string_view{})));  // value_or: tidy-safe
    // descriptor.proto is proto2 with extension ranges + extend.
    CHECK(file.syntax_level == SyntaxLevel::Proto2);
    CHECK_FALSE(file.messages.empty());
}
