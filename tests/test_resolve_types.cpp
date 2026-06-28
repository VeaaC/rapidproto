#include <catch_amalgamated.hpp>

#include <string>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

FileNode parse_only(std::string src) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    return std::move(r.value().value);
}

// Build a ResolvedFileSet from (canonical-name, source) pairs and run FQN computation, leaving it
// ready for resolve_types. Imports in a source must reference the other files by their names here.
ResolvedFileSet make_set(std::vector<std::pair<std::string, std::string>> files) {
    ResolvedFileSet set;
    for (auto& [name, src] : files) {
        FileNode file = parse_only(src);
        file.filename = name;
        set.file_index[name] = set.files.size();
        set.files.push_back(std::move(file));
    }
    compute_fqns(set);
    return set;
}

ResolvedFileSet one(std::string src) {
    return make_set({{"a.proto", std::move(src)}});
}

}  // namespace

TEST_CASE("types: a relative reference in the same message scope") {
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg;
        message M { N n = 1; message N {} }
    )");
    auto table = resolve_types(set);
    REQUIRE(table.is_ok());
    const FieldNode& f = set.files[0].messages[0].fields[0];
    CHECK(f.resolved_type_fqn == ".pkg.M.N");
    CHECK(f.is_message_type);
    CHECK_FALSE(f.is_enum_type);
}

TEST_CASE("types: a relative reference resolves in a parent scope") {
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg;
        message Outer {
            message Inner { Sibling s = 1; }
            message Sibling {}
        }
    )");
    REQUIRE(resolve_types(set).is_ok());
    const FieldNode& f = set.files[0].messages[0].nested_messages[0].fields[0];
    CHECK(f.resolved_type_fqn == ".pkg.Outer.Sibling");  // found in the parent (Outer) scope
}

TEST_CASE("types: an absolute reference") {
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg;
        message M { .pkg.Other o = 1; }
        message Other {}
    )");
    REQUIRE(resolve_types(set).is_ok());
    CHECK(set.files[0].messages[0].fields[0].resolved_type_fqn == ".pkg.Other");
}

TEST_CASE("types: enum references and scalar skipping") {
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg;
        message M { Color c = 1; int32 n = 2; enum Color { UNSET = 0; } }
    )");
    REQUIRE(resolve_types(set).is_ok());
    const FieldNode& c = set.files[0].messages[0].fields[0];
    CHECK(c.resolved_type_fqn == ".pkg.M.Color");
    CHECK(c.is_enum_type);
    CHECK_FALSE(c.is_message_type);
    const FieldNode& n = set.files[0].messages[0].fields[1];
    CHECK(n.resolved_type_fqn.empty());  // scalar: not resolved
    CHECK_FALSE(n.is_message_type);
    CHECK_FALSE(n.is_enum_type);
}

TEST_CASE("types: a map value type is resolved (key stays scalar)") {
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg;
        message M { map<string, V> m = 1; message V {} }
    )");
    REQUIRE(resolve_types(set).is_ok());
    const MapFieldNode& m = set.files[0].messages[0].map_fields[0];
    CHECK(m.resolved_value_type_fqn == ".pkg.M.V");
    CHECK(m.value_is_message);
}

TEST_CASE("types: a message-typed map key is rejected") {
    // The parser accepts any key type; analysis must reject a non-scalar key so it never reaches a
    // code generator (which assumes a scalar key).
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg;
        message M { map<V, string> m = 1; message V {} }
    )");
    auto table = resolve_types(set);
    REQUIRE(table.is_err());
    CHECK(table.error().message.find("map key") != std::string::npos);
}

TEST_CASE("types: a float map key is rejected (only integral/string keys are valid)") {
    ResolvedFileSet set =
        one(R"(syntax = "proto3"; package pkg; message M { map<float, string> m = 1; })");
    auto table = resolve_types(set);
    REQUIRE(table.is_err());
    CHECK(table.error().message.find("map key") != std::string::npos);
}

TEST_CASE("types: an unknown type name fails") {
    ResolvedFileSet set = one(R"(syntax = "proto3"; package pkg; message M { Nope x = 1; })");
    auto table = resolve_types(set);
    REQUIRE(table.is_err());
    CHECK(table.error().message.find("unresolved type") != std::string::npos);
}

TEST_CASE("types: an unresolved map value type fails") {
    ResolvedFileSet set =
        one(R"(syntax = "proto3"; package pkg; message M { map<string, Nope> m = 1; })");
    auto table = resolve_types(set);
    REQUIRE(table.is_err());
    CHECK(table.error().message.find("map value") != std::string::npos);
}

TEST_CASE("types: an unresolved extendee fails") {
    ResolvedFileSet set =
        one(R"(syntax = "proto2"; package pkg; extend Nope { optional int32 x = 100; })");
    auto table = resolve_types(set);
    REQUIRE(table.is_err());
    CHECK(table.error().message.find("unresolved extendee") != std::string::npos);
}

// --- imports / visibility ---------------------------------------------------

TEST_CASE("types: a reference into a directly imported file") {
    ResolvedFileSet set = make_set({
        {"a.proto",
         R"(syntax = "proto3"; package a; import "b.proto"; message A { b.B field = 1; })"},
        {"b.proto", R"(syntax = "proto3"; package b; message B {})"},
    });
    REQUIRE(resolve_types(set).is_ok());
    CHECK(set.files[0].messages[0].fields[0].resolved_type_fqn == ".b.B");
}

TEST_CASE("types: a reference into a non-imported file fails") {
    ResolvedFileSet set = make_set({
        {"a.proto", R"(syntax = "proto3"; package a; message A { b.B field = 1; })"},  // no import
        {"b.proto", R"(syntax = "proto3"; package b; message B {})"},
    });
    auto table = resolve_types(set);
    REQUIRE(table.is_err());  // B exists but is not visible to a.proto
    CHECK(table.error().message.find("unresolved type") != std::string::npos);
}

TEST_CASE("types: a transitively public-imported type is visible") {
    ResolvedFileSet set = make_set({
        {"a.proto",
         R"(syntax = "proto3"; package a; import "b.proto"; message A { c.C field = 1; })"},
        {"b.proto", R"(syntax = "proto3"; package b; import public "c.proto"; message B {})"},
        {"c.proto", R"(syntax = "proto3"; package c; message C {})"},
    });
    // a imports b; b publicly imports c -> C is visible to a.
    REQUIRE(resolve_types(set).is_ok());
    CHECK(set.files[0].messages[0].fields[0].resolved_type_fqn == ".c.C");
}

TEST_CASE("types: a NON-public transitive import is not visible") {
    ResolvedFileSet set = make_set({
        {"a.proto",
         R"(syntax = "proto3"; package a; import "b.proto"; message A { c.C field = 1; })"},
        {"b.proto",
         R"(syntax = "proto3"; package b; import "c.proto"; message B {})"},  // not public
        {"c.proto", R"(syntax = "proto3"; package c; message C {})"},
    });
    auto table = resolve_types(set);
    REQUIRE(table.is_err());  // c is imported by b but not re-exported, so invisible to a
}

// --- post-resolution fixup --------------------------------------------------

TEST_CASE("types: proto3 message-typed field without optional gets explicit presence") {
    ResolvedFileSet set = one(R"(syntax = "proto3"; package pkg; message M { M self = 1; })");
    REQUIRE(resolve_types(set).is_ok());
    const FieldNode& f = set.files[0].messages[0].fields[0];
    CHECK(f.is_message_type);
    CHECK(f.presence == FieldPresence::Explicit);  // fixed up from the parse-time Implicit
}

TEST_CASE("types: proto3 enum-typed field stays implicit") {
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg; message M { E e = 1; enum E { Z = 0; } }
    )");
    REQUIRE(resolve_types(set).is_ok());
    const FieldNode& f = set.files[0].messages[0].fields[0];
    CHECK(f.is_enum_type);
    CHECK(f.presence == FieldPresence::Implicit);  // enums keep implicit presence in proto3
}

TEST_CASE("types: a repeated message field is expanded") {
    ResolvedFileSet set = one(
        R"(syntax = "proto3"; package pkg; message M { repeated Item items = 1; message Item {} })");
    REQUIRE(resolve_types(set).is_ok());
    const FieldNode& f = set.files[0].messages[0].fields[0];
    CHECK(f.is_message_type);
    CHECK(f.repeated_encoding == RepeatedEncoding::Expanded);
}

// --- symbol table + extension registry --------------------------------------

TEST_CASE("types: analyze() composes resolution + interpretation and returns the symbol table") {
    ResolvedFileSet set = make_set({{"a.proto", R"(
        syntax = "proto2";
        package pkg;
        message M {
            optional Inner child = 1;
            optional int32 n = 2 [default = 9];
            message Inner {}
        }
    )"}});
    auto table = analyze(set);  // runs features -> fqns -> types -> interpret
    REQUIRE(table.is_ok());
    const MessageNode& m = set.files[0].messages[0];
    CHECK(m.fields[0].resolved_type_fqn == ".pkg.M.Inner");  // type resolution
    CHECK(m.fields[0].is_message_type);
    CHECK(m.fields[1]
              .default_value.has_value());  // option interpretation ran (value in test_interpret)
    CHECK(table.value().symbols.at(".pkg.M.Inner") == SymbolKind::Message);
}

TEST_CASE("types: analyze() applies the editions feature then the message-presence fixup") {
    ResolvedFileSet set = make_set({{"a.proto", R"(
        edition = "2023";
        package pkg;
        option features.field_presence = IMPLICIT;
        message M { int32 scalar = 1; M child = 2; }
    )"}});
    REQUIRE(analyze(set).is_ok());
    const MessageNode& m = set.files[0].messages[0];
    CHECK(m.fields[0].presence == FieldPresence::Implicit);  // feature pass
    CHECK(m.fields[1].presence ==
          FieldPresence::Explicit);  // type resolution promotes message-typed
}

TEST_CASE("types: the symbol table records every message and enum FQN with its kind") {
    ResolvedFileSet set = one(R"(
        syntax = "proto3"; package pkg;
        message M { message Inner {} enum E { Z = 0; } }
    )");
    auto table = resolve_types(set);
    REQUIRE(table.is_ok());
    CHECK(table.value().symbols.at(".pkg.M") == SymbolKind::Message);
    CHECK(table.value().symbols.at(".pkg.M.Inner") == SymbolKind::Message);
    CHECK(table.value().symbols.at(".pkg.M.E") == SymbolKind::Enum);
}

TEST_CASE("types: a group field resolves to its synthesized message (preserving delimited)") {
    ResolvedFileSet set = one(R"(
        syntax = "proto2"; package pkg;
        message M { optional group G = 1 { optional int32 v = 2; } }
    )");
    REQUIRE(resolve_types(set).is_ok());
    const FieldNode& g = set.files[0].messages[0].fields[0];
    CHECK(g.is_group);
    CHECK(g.is_message_type);
    CHECK(g.resolved_type_fqn == ".pkg.M.G");
    CHECK(g.message_encoding == MessageEncoding::Delimited);  // survives resolution
}

TEST_CASE("types: weak imports are visible (weak only affects linking)") {
    ResolvedFileSet set = make_set({
        {"a.proto",
         R"(syntax = "proto3"; package a; import weak "b.proto"; message A { b.B f = 1; })"},
        {"b.proto", R"(syntax = "proto3"; package b; message B {})"},
    });
    REQUIRE(resolve_types(set).is_ok());
    CHECK(set.files[0].messages[0].fields[0].resolved_type_fqn == ".b.B");
}

TEST_CASE("types: extending a non-message fails") {
    ResolvedFileSet set = one(R"(
        syntax = "proto2"; package pkg;
        enum E { Z = 0; }
        extend E { optional int32 x = 1; }
    )");
    auto table = resolve_types(set);
    REQUIRE(table.is_err());
    CHECK(table.error().message.find("not a message") != std::string::npos);
}

TEST_CASE("types: a cross-file extension with a message-typed extension field") {
    ResolvedFileSet set = make_set({
        {"a.proto",
         R"(syntax = "proto2"; package a; import "b.proto";
            message Payload {}
            extend b.Target { optional Payload p = 100; })"},
        {"b.proto", R"(syntax = "proto2"; package b; message Target { extensions 100 to max; })"},
    });
    auto table = resolve_types(set);
    REQUIRE(table.is_ok());
    REQUIRE(table.value().extensions.count({".b.Target", 100}) == 1);  // keyed by imported FQN
    const FieldNode* ext = table.value().extensions.at({".b.Target", 100});
    CHECK(ext->name == "p");
    CHECK(ext->is_message_type);  // the extension field's own type is resolved
    CHECK(ext->resolved_type_fqn == ".a.Payload");
}

TEST_CASE("types: the extension registry keys on (extendee FQN, number)") {
    ResolvedFileSet set = one(R"(
        syntax = "proto2"; package pkg;
        message Target { extensions 100 to max; }
        extend Target { optional int32 ext_a = 100; optional string ext_b = 101; }
    )");
    auto table = resolve_types(set);
    REQUIRE(table.is_ok());
    const SymbolTable& t = table.value();
    REQUIRE(t.extensions.count({".pkg.Target", 100}) == 1);
    REQUIRE(t.extensions.count({".pkg.Target", 101}) == 1);
    CHECK(t.extensions.at({".pkg.Target", 100})->name == "ext_a");
    CHECK(t.extensions.at({".pkg.Target", 101})->name == "ext_b");
}

TEST_CASE("types: a duplicate extension number is rejected") {
    // Two extensions of the same message at the same number collide; protoc rejects this, so analysis
    // does too rather than silently last-winning.
    ResolvedFileSet set = one(R"(
        syntax = "proto2"; package pkg;
        message Target { extensions 100 to 200; }
        extend Target { optional int32 a = 100; optional int32 b = 100; }
    )");
    auto table = resolve_types(set);
    REQUIRE(table.is_err());
    CHECK(table.error().message.find("duplicate extension") != std::string::npos);
}
