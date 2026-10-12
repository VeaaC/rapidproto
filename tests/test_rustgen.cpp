// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter

#include <string>
#include <string_view>
#include <utility>

#include <catch_amalgamated.hpp>

#include "parse_helpers.hpp"
#include "rapidproto/rustgen/generator.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

std::string gen(std::string src) {
    return rustgen::generate_common(test::parse_file_ok(std::move(src)));
}

bool has(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("rustgen common: strippable enum strips the prefix and builds the classify enum") {
    // Every value carries the `COLOR_` prefix, so it is stripped from the consts.
    const std::string out = gen(R"(syntax = "proto3"; package m.n;
        enum Color { COLOR_UNKNOWN = 0; COLOR_RED = 1; COLOR_GREEN = 2; })");

    CHECK(has(out, "pub mod m {"));
    CHECK(has(out, "pub mod n {"));
    CHECK(has(out, "pub struct Color(pub i32);"));
    CHECK(has(out, "pub const UNKNOWN: Color = Color(0);"));
    CHECK(has(out, "pub const RED: Color = Color(1);"));
    CHECK(has(out, "pub const GREEN: Color = Color(2);"));

    CHECK(has(out, "pub fn kind(self) -> ColorKind {"));
    CHECK(has(out, "1 => ColorKind::Red,"));
    CHECK(has(out, "2 => ColorKind::Green,"));
    CHECK(has(out, "other => ColorKind::Unknown(other),"));

    CHECK(has(out, "#[non_exhaustive]"));
    CHECK(has(out, "pub enum ColorKind {"));
    CHECK(has(out, "Unknown(i32),"));
    // COLOR_UNKNOWN strips to UNKNOWN; its PascalCase variant `Unknown` collides with the reserved
    // `Unknown(i32)`, so it is deduped to `Unknown_`.
    CHECK(has(out, "Unknown_,"));
    CHECK(has(out, "0 => ColorKind::Unknown_,"));
}

TEST_CASE("rustgen common: a value missing the prefix disables stripping for the whole enum") {
    // `RED` lacks the `E_` prefix, so no value is stripped (all-or-nothing). No package => no module.
    const std::string out = gen(R"(syntax = "proto3";
        enum E { E_UNKNOWN = 0; RED = 1; })");

    CHECK_FALSE(has(out, "pub mod "));
    CHECK(has(out, "pub const E_UNKNOWN: E = E(0);"));
    CHECK(has(out, "pub const RED: E = E(1);"));
    CHECK(has(out, "EUnknown,"));  // PascalCase("E_UNKNOWN")
    CHECK(has(out, "Red,"));
}

TEST_CASE("rustgen common: Rust keywords in value names are raw-escaped") {
    // `type` is a Rust keyword; `none` is not. Not strippable (no `E_` prefix), so names are kept.
    const std::string out = gen(R"(syntax = "proto3";
        enum E { none = 0; type = 1; })");

    CHECK(has(out, "pub const none: E = E(0);"));
    CHECK(has(out, "pub const r#type: E = E(1);"));  // keyword const escaped with r#
    CHECK(has(out, "Type,"));                        // PascalCase variant is not a keyword
    CHECK(has(out, "None,"));
}

TEST_CASE("rustgen common: alias values collapse to one match arm") {
    // Two names for number 0 (parse-level; duplicate match arms would not compile).
    const std::string out = gen(R"(syntax = "proto3";
        enum E { A = 0; B = 0; C = 1; })");

    CHECK(has(out, "pub const A: E = E(0);"));
    CHECK(has(out, "pub const B: E = E(0);"));
    CHECK(has(out, "0 => EKind::A,"));
    CHECK_FALSE(has(out, "0 => EKind::B,"));  // the second alias for 0 is skipped
    CHECK(has(out, "1 => EKind::C,"));
}

TEST_CASE("rustgen common: underscore/digit-head value names yield valid variants") {
    // protoc accepts these; `to_pascal_case` would otherwise emit an empty or digit-leading variant
    // (invalid Rust). Not strippable (no `E_` prefix on every value).
    const std::string out = gen(R"(syntax = "proto3";
        enum E { NORMAL = 0; _ = 1; _2D = 2; })");

    // consts keep the underscore forms (valid identifiers).
    CHECK(has(out, "pub const _x: E = E(1);"));  // escape_rust_ident("_") -> "_x"
    CHECK(has(out, "pub const _2D: E = E(2);"));
    // variants get a `V` head where PascalCase left an empty/digit-leading name.
    CHECK(has(out, "pub enum EKind {"));
    CHECK(has(out, "V,"));    // from value `_`
    CHECK(has(out, "V2d,"));  // from value `_2D`
    CHECK(has(out, "1 => EKind::V,"));
    CHECK(has(out, "2 => EKind::V2d,"));
}

TEST_CASE("rustgen common: empty file emits nothing") {
    CHECK(gen(R"(syntax = "proto3"; package x;)").find("pub struct") == std::string::npos);
}
