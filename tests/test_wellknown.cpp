#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"
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
    auto lr = lex(std::string(src.value_or(std::string_view{})));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    // descriptor.proto is proto2 with extension ranges + extend.
    CHECK(r.value().value.syntax_level == SyntaxLevel::Proto2);
    CHECK_FALSE(r.value().value.messages.empty());
}
