// Unit tests for the code emitter (Printer) and a smoke test of the generator pipeline
// (analyze -> emit).

#include <catch_amalgamated.hpp>

#include <string>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/streamgen/generator.hpp"

using namespace rapidproto;          // NOLINT(google-build-using-namespace): test convenience
using rapidproto::codegen::Printer;  // NOLINT(misc-include-cleaner)

namespace {

FileNode parse_file_text(std::string source) {
    auto lexed = lex(std::move(source));
    REQUIRE(lexed.is_ok());
    auto parsed = parse_file(Range<Token>(lexed.value().tokens));
    REQUIRE(parsed.is_ok());
    return std::move(parsed.value().value);
}

}  // namespace

TEST_CASE("printer: plain text passes through", "[printer]") {
    Printer printer;
    printer.print("hello\nworld\n");
    CHECK(printer.str() == "hello\nworld\n");
}

TEST_CASE("printer: inline variable substitution", "[printer]") {
    Printer printer;
    printer.print("ns=$ns$ name=$name$\n", {{"ns", "a::b"}, {"name", "Foo"}});
    CHECK(printer.str() == "ns=a::b name=Foo\n");
}

TEST_CASE("printer: persistent variables and $$ escape", "[printer]") {
    Printer printer;
    printer.set("who", "world");
    printer.print("$$ hi $who$\n");
    CHECK(printer.str() == "$ hi world\n");
}

TEST_CASE("printer: inline bindings override persistent ones", "[printer]") {
    Printer printer;
    printer.set("x", "persistent");
    printer.print("$x$\n", {{"x", "inline"}});
    CHECK(printer.str() == "inline\n");
}

TEST_CASE("printer: an unresolved variable is left visible", "[printer]") {
    Printer printer;
    printer.print("$missing$\n");
    CHECK(printer.str() == "$missing$\n");
}

TEST_CASE("printer: indentation is applied at the start of each line", "[printer]") {
    Printer printer;
    printer.print("a\n");
    printer.indent();
    printer.print("b\nc\n");
    printer.outdent();
    printer.print("d\n");
    CHECK(printer.str() == "a\n  b\n  c\nd\n");
}

TEST_CASE("printer: a blank line gets no indentation", "[printer]") {
    Printer printer;
    printer.indent();
    printer.print("a\n\nb\n");
    CHECK(printer.str() == "  a\n\n  b\n");
}

TEST_CASE("printer: a multi-line substituted value is re-indented", "[printer]") {
    Printer printer;
    printer.indent();
    printer.print("body=$v$\n", {{"v", "line1\nline2"}});
    CHECK(printer.str() == "  body=line1\n  line2\n");
}

TEST_CASE("printer: a substituted value is not re-scanned for variables", "[printer]") {
    Printer printer;
    printer.print("$v$\n", {{"v", "$x$"}});  // the $x$ inside the value stays literal
    CHECK(printer.str() == "$x$\n");
}

TEST_CASE("generator: header carries the namespace and a decoder per message", "[printer]") {
    const FileNode file =
        parse_file_text(R"(syntax = "proto3"; package a.b; message Foo {} message Bar {})");
    const std::string header = rapidproto::streamgen::generate_header(file);

    CHECK(header.find("#pragma once") != std::string::npos);
    CHECK(header.find("#include \"rapidproto/runtime.hpp\"") != std::string::npos);
    CHECK(header.find("namespace a::b::stream {") != std::string::npos);
    CHECK(header.find("struct Foo {") != std::string::npos);
    CHECK(header.find("struct Bar {") != std::string::npos);
    CHECK(header.find("}  // namespace a::b::stream") != std::string::npos);
}

TEST_CASE("generator: a package-less file still nests under the model namespace", "[printer]") {
    const FileNode file = parse_file_text(R"(syntax = "proto3"; message Solo {})");
    const std::string header = rapidproto::streamgen::generate_header(file);
    // Streaming always nests under `stream` (so it coexists with the arena model's global `Solo`),
    // even with no package: the namespace is just `stream`.
    CHECK(header.find("namespace stream {") != std::string::npos);
    CHECK(header.find("struct Solo {") != std::string::npos);
}

TEST_CASE("generator: field names colliding with generated identifiers are sanitized",
          "[printer]") {
    // `value` is a read() local; `read` is the method; `rp_value` matches the reserved generator
    // prefix (any `rp_`-prefixed name is escaped by the single sanitize rule) — all would break the
    // generated code.
    const FileNode file = parse_file_text(
        R"(syntax = "proto3"; message M { int32 value = 1; int32 read = 2; int32 rp_value = 3; })");
    const std::string header = rapidproto::streamgen::generate_header(file);
    CHECK(header.find("struct value_ {") != std::string::npos);
    CHECK(header.find("struct read_ {") != std::string::npos);
    CHECK(header.find("struct rp_value_ {") != std::string::npos);  // the `rp_` prefix rule
    CHECK(header.find("kName = \"value\"") != std::string::npos);   // the wire name is preserved
    CHECK(header.find("kName = \"read\"") != std::string::npos);
    CHECK(header.find("kName = \"rp_value\"") != std::string::npos);
}
