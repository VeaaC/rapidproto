#include <catch_amalgamated.hpp>

#include "parse_helpers.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/interpret.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

FileNode parse_only(std::string src) {
    return test::parse_file_ok(std::move(src));
}

FileNode interpret_ok(std::string src) {
    FileNode file = parse_only(std::move(src));
    compute_fqns(file);  // message_set_wire_format diagnostics use the FQN
    auto result = interpret_options(file);
    REQUIRE(result.is_ok());
    return file;
}

// Read a field's interpreted default value. The has_value guard (and the unreachable early return)
// keep clang-tidy's optional-access model happy.
template <typename T>
T default_as(const FieldNode& field) {
    REQUIRE(field.default_value.has_value());
    if (!field.default_value.has_value()) {
        return T{};
    }
    return std::get<T>(field.default_value->value);
}

}  // namespace

TEST_CASE("interpret: [packed=true] overrides the proto2 expanded default") {
    FileNode f =
        interpret_ok(R"(syntax = "proto2"; message M { repeated int32 a = 1 [packed = true]; })");
    CHECK(f.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Packed);
}

TEST_CASE("interpret: [packed=false] overrides the proto3 packed default") {
    FileNode f =
        interpret_ok(R"(syntax = "proto3"; message M { repeated int32 a = 1 [packed = false]; })");
    CHECK(f.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Expanded);
}

TEST_CASE("interpret: no packed option leaves the syntax default in place") {
    FileNode p2 = interpret_ok(R"(syntax = "proto2"; message M { repeated int32 a = 1; })");
    CHECK(p2.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Expanded);
    FileNode p3 = interpret_ok(R"(syntax = "proto3"; message M { repeated int32 a = 1; })");
    CHECK(p3.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Packed);
}

TEST_CASE("interpret: [packed = true] on a non-packable field is ignored") {
    // string/bytes are repeated-but-not-packable: the wire format has no packed form for
    // length-delimited elements, and the feature pass refuses the equivalent feature. The
    // interpreter must gate on the same is_packable_scalar predicate -- before it did, this
    // exact shape flipped repeated_encoding to Packed while resolve_features said Expanded.
    FileNode f =
        interpret_ok(R"(syntax = "proto3"; message M { repeated string s = 1 [packed = true]; })");
    CHECK(f.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Expanded);
}

TEST_CASE("interpret: proto2 [default] is captured as the typed default_value") {
    FileNode f = interpret_ok(R"(
        syntax = "proto2";
        message M {
            optional int32 n = 1 [default = 42];
            optional string s = 2 [default = "hi"];
            optional E e = 3 [default = B];
            enum E { A = 0; B = 1; }
        }
    )");
    const auto& fields = f.messages[0].fields;
    CHECK(default_as<std::uint64_t>(fields[0]) == 42);
    CHECK(default_as<std::string>(fields[1]) == "hi");
    CHECK(default_as<Identifier>(fields[2]).name == "B");
}

TEST_CASE("interpret: a custom (pkg.packed) extension option does not trigger builtin packed") {
    FileNode f = interpret_ok(
        R"(syntax = "proto2"; message M { repeated int32 a = 1 [(pkg.packed) = true]; })");
    // The extension option must NOT be mistaken for the builtin `packed`.
    CHECK(f.messages[0].fields[0].repeated_encoding == RepeatedEncoding::Expanded);
}

TEST_CASE("interpret: a negative default is captured as int64") {
    FileNode f =
        interpret_ok(R"(syntax = "proto2"; message M { optional int32 n = 1 [default = -5]; })");
    CHECK(default_as<std::int64_t>(f.messages[0].fields[0]) == -5);
}

TEST_CASE("interpret: a field with no default has no default_value") {
    FileNode f = interpret_ok(R"(syntax = "proto2"; message M { optional int32 a = 1; })");
    CHECK_FALSE(f.messages[0].fields[0].default_value.has_value());
}

TEST_CASE("interpret: group fields keep DELIMITED + is_group") {
    FileNode f = interpret_ok(
        R"(syntax = "proto2"; message M { optional group G = 1 { optional int32 v = 2; } })");
    const FieldNode& g = f.messages[0].fields[0];
    CHECK(g.is_group);
    CHECK(g.message_encoding == MessageEncoding::Delimited);
}

TEST_CASE("interpret: custom and unknown options pass through raw, unmodified") {
    FileNode f = interpret_ok(R"(
        syntax = "proto2";
        message M { optional int32 a = 1 [default = 5, (my.custom) = 7, deprecated = true]; }
    )");
    const FieldNode& field = f.messages[0].fields[0];
    CHECK(field.default_value.has_value());  // default interpreted...
    // ...but every option (incl. default, the custom extension, deprecated) is retained raw.
    REQUIRE(field.options.size() == 3);
    CHECK(field.options[0].name[0].name == "default");
    CHECK(field.options[1].name[0].name == "my.custom");
    CHECK(field.options[1].name[0].is_extension);
    CHECK(field.options[2].name[0].name == "deprecated");
}

// A MessageSet holds only extensions, which no emitter materializes, so its contents are
// unreadable either way -- but the encoding is well-formed and skips cleanly as unknown fields.
// Rejecting the option would fail the whole FILE, taking every unrelated message with it.
TEST_CASE("interpret: message_set_wire_format = true warns, and does not reject") {
    FileNode f = parse_only(R"(
        syntax = "proto2";
        message M { option message_set_wire_format = true; extensions 1 to max; }
    )");
    compute_fqns(f);
    std::vector<std::string> warnings;
    REQUIRE(interpret_options(f, &warnings).is_ok());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("MessageSet wire format") != std::string::npos);
    CHECK(warnings[0].find(".M") != std::string::npos);  // names the message, by FQN
}

TEST_CASE("interpret: message_set_wire_format in a nested message warns") {
    FileNode f = parse_only(R"(
        syntax = "proto2";
        message Outer { message Inner { option message_set_wire_format = true; } }
    )");
    compute_fqns(f);
    std::vector<std::string> warnings;
    REQUIRE(interpret_options(f, &warnings).is_ok());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find(".Outer.Inner") != std::string::npos);
}

TEST_CASE("interpret: the warnings sink is optional") {
    FileNode f = parse_only(R"(
        syntax = "proto2";
        message M { option message_set_wire_format = true; extensions 1 to max; }
    )");
    compute_fqns(f);
    CHECK(interpret_options(f).is_ok());  // nullptr sink: no diagnostic, still no error
}

TEST_CASE("interpret: message_set_wire_format = false is allowed and silent") {
    FileNode f =
        parse_only(R"(syntax = "proto2"; message M { option message_set_wire_format = false; })");
    compute_fqns(f);
    std::vector<std::string> warnings;
    CHECK(interpret_options(f, &warnings).is_ok());
    CHECK(warnings.empty());
}

TEST_CASE("interpret: default applies to extension fields") {
    FileNode f = interpret_ok(R"(
        syntax = "proto2";
        message Target { extensions 100 to max; }
        extend Target { optional int32 ext = 100 [default = 7]; }
    )");
    REQUIRE(f.extends.size() == 1);
    CHECK(default_as<std::uint64_t>(f.extends[0].fields[0]) == 7);
}

TEST_CASE("interpret: a oneof body is walked without error") {
    FileNode f = interpret_ok(R"(
        syntax = "proto2";
        message M { oneof o { int32 a = 1; } }
    )");
    CHECK(f.messages[0].oneofs[0].fields[0].number == 1);
}
