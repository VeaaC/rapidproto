#include <catch_amalgamated.hpp>

#include <string>

#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"
#include "rapidproto/source_id.hpp"

using rapidproto::Error;
using rapidproto::render_error;
using rapidproto::SourceId;
using rapidproto::SourceRegistry;

TEST_CASE("SourceId default-constructs to Invalid") {
    constexpr SourceId id;
    STATIC_REQUIRE_FALSE(id.valid());
    STATIC_REQUIRE(id == SourceId::invalid());
}

TEST_CASE("SourceId wraps an index") {
    constexpr SourceId id{42};
    STATIC_REQUIRE(id.valid());
    STATIC_REQUIRE(id.index() == 42);
}

TEST_CASE("SourceId equality") {
    CHECK(SourceId{1} == SourceId{1});
    CHECK(SourceId{1} != SourceId{2});
    CHECK(SourceId::invalid() == SourceId{});
    CHECK(SourceId{} != SourceId{0});  // Invalid is distinct from index 0
}

TEST_CASE("source: line_col maps byte offsets to 1-based line:col", "[source]") {
    SourceRegistry reg;
    const SourceId id = reg.add("foo.proto", "abc\ndef\nghi");  // 3 lines

    CHECK(reg.line_col(id, 0).line == 1);  // 'a'
    CHECK(reg.line_col(id, 0).column == 1);
    CHECK(reg.line_col(id, 2).line == 1);  // 'c'
    CHECK(reg.line_col(id, 2).column == 3);
    CHECK(reg.line_col(id, 3).line == 1);  // the '\n' itself still ends line 1
    CHECK(reg.line_col(id, 4).line == 2);  // 'd'
    CHECK(reg.line_col(id, 4).column == 1);
    CHECK(reg.line_col(id, 6).line == 2);  // 'f'
    CHECK(reg.line_col(id, 6).column == 3);
    CHECK(reg.line_col(id, 8).line == 3);  // 'g'
    CHECK(reg.line_col(id, 8).column == 1);

    // An offset past the end clamps to the end (does not run off the buffer).
    CHECK(reg.line_col(id, 9999).line == 3);
}

TEST_CASE("source: a second source gets a distinct SourceId", "[source]") {
    SourceRegistry reg;
    const SourceId a = reg.add("a.proto", "x");
    const SourceId b = reg.add("b.proto", "y\nz");
    CHECK(a != b);
    CHECK(reg.filename(a) == "a.proto");
    CHECK(reg.filename(b) == "b.proto");
    CHECK(reg.line_col(b, 2).line == 2);  // 'z' is on line 2 of b
}

TEST_CASE("source: render_error formats file:line:col: message", "[source]") {
    SourceRegistry reg;
    const SourceId id = reg.add("a/b.proto", "syntax = \"proto3\";\nmessage M {");
    const Error err{id, 19, "expected '}'"};  // offset 19 = first column of line 2
    CHECK(render_error(err, reg) == "a/b.proto:2:1: expected '}'");
}

TEST_CASE("source: render_error returns the bare message when untied to a source", "[source]") {
    SourceRegistry reg;
    reg.add("a.proto", "x");
    const Error untied{5, "no source"};  // SourceId default-constructed == invalid()
    CHECK(render_error(untied, reg) == "no source");

    // An out-of-range SourceId is treated as unknown (no crash, bare message).
    const Error stray{SourceId{99}, 0, "stray"};
    CHECK(render_error(stray, reg) == "stray");
}
