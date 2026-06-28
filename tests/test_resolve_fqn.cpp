#include <catch_amalgamated.hpp>

#include <string>
#include <utility>

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

FileNode fqn_of(std::string src) {
    FileNode file = parse_only(std::move(src));
    compute_fqns(file);
    return file;
}

}  // namespace

TEST_CASE("fqn: a top-level message with a package") {
    const FileNode f = fqn_of(R"(syntax = "proto3"; package pkg; message MyMessage {})");
    CHECK(f.messages[0].fqn == ".pkg.MyMessage");
}

TEST_CASE("fqn: a top-level message with no package") {
    const FileNode f = fqn_of(R"(syntax = "proto3"; message MyMessage {})");
    CHECK(f.messages[0].fqn == ".MyMessage");
}

TEST_CASE("fqn: a multi-component package") {
    const FileNode f = fqn_of(R"(syntax = "proto3"; package a.b.c; message M {})");
    CHECK(f.messages[0].fqn == ".a.b.c.M");
}

TEST_CASE("fqn: deeply nested messages") {
    const FileNode f = fqn_of(R"(
        syntax = "proto3";
        package pkg;
        message Outer { message Inner { message DeepNested {} } }
    )");
    const MessageNode& outer = f.messages[0];
    CHECK(outer.fqn == ".pkg.Outer");
    CHECK(outer.nested_messages[0].fqn == ".pkg.Outer.Inner");
    CHECK(outer.nested_messages[0].nested_messages[0].fqn == ".pkg.Outer.Inner.DeepNested");
}

TEST_CASE("fqn: a file-level enum and its values are sibling-scoped") {
    const FileNode f = fqn_of(R"(
        syntax = "proto3";
        package pkg;
        enum MyEnum { UNSET = 0; VALUE = 1; }
    )");
    CHECK(f.enums[0].fqn == ".pkg.MyEnum");
    // Enum values live in the ENCLOSING scope, not under the enum.
    CHECK(f.enums[0].values[0].fqn == ".pkg.UNSET");
    CHECK(f.enums[0].values[1].fqn == ".pkg.VALUE");
}

TEST_CASE("fqn: an enum nested in a message (values sibling-scoped to the message)") {
    const FileNode f = fqn_of(R"(
        syntax = "proto3";
        package pkg;
        message Msg { enum Status { OK = 0; } }
    )");
    const MessageNode& msg = f.messages[0];
    CHECK(msg.enums[0].fqn == ".pkg.Msg.Status");
    CHECK(msg.enums[0].values[0].fqn == ".pkg.Msg.OK");  // not ".pkg.Msg.Status.OK"
}

TEST_CASE("fqn: enums and values with no package") {
    const FileNode f = fqn_of(R"(syntax = "proto3"; enum E { A = 0; })");
    CHECK(f.enums[0].fqn == ".E");
    CHECK(f.enums[0].values[0].fqn == ".A");
}

TEST_CASE("fqn: extension fields derive from the declaration scope, not the extendee") {
    const FileNode f = fqn_of(R"(
        syntax = "proto2";
        package pkg;
        extend google.protobuf.FileOptions { optional int32 my_opt = 50000; }
        message Holder { extend Foo { optional int32 nested_ext = 100; } }
    )");
    // file-level extend: declaration scope is the file package.
    CHECK(f.extends[0].fields[0].fqn == ".pkg.my_opt");
    // nested extend: declaration scope is the enclosing message.
    CHECK(f.messages[0].extends[0].fields[0].fqn == ".pkg.Holder.nested_ext");
    // regular (non-extension) fields have no FQN.
    CHECK(f.messages[0].fields.empty());
}

TEST_CASE("fqn: a value in a deeply nested enum is sibling-scoped to its message") {
    const FileNode f =
        fqn_of(R"(syntax = "proto3"; package pkg; message A { message B { enum C { Y = 0; } } })");
    const EnumNode& c = f.messages[0].nested_messages[0].enums[0];
    CHECK(c.fqn == ".pkg.A.B.C");
    CHECK(c.values[0].fqn == ".pkg.A.B.Y");  // sibling of C, scoped to B
}

TEST_CASE("fqn: a multi-file file set uses each file's own package") {
    ResolvedFileSet set;
    set.files.push_back(parse_only(R"(syntax = "proto3"; package a; message A {})"));
    set.files.push_back(parse_only(R"(syntax = "proto3"; package b.c; message B {})"));
    compute_fqns(set);
    CHECK(set.files[0].messages[0].fqn == ".a.A");
    CHECK(set.files[1].messages[0].fqn == ".b.c.B");
}

TEST_CASE("fqn: compute_fqns is idempotent") {
    FileNode f = fqn_of(R"(syntax = "proto3"; package pkg; message M { enum E { A = 0; } })");
    REQUIRE(f.messages[0].enums[0].values[0].fqn == ".pkg.M.A");
    compute_fqns(f);  // running again must not change anything
    CHECK(f.messages[0].enums[0].values[0].fqn == ".pkg.M.A");
    CHECK(f.messages[0].enums[0].fqn == ".pkg.M.E");
}

TEST_CASE("fqn: a synthesized group message is named in its declaration scope") {
    const FileNode f = fqn_of(R"(
        syntax = "proto2";
        package pkg;
        message M { optional group Result = 1 { optional int32 score = 2; } }
    )");
    // The group's synthesized message is a nested message of M.
    REQUIRE(f.messages[0].nested_messages.size() == 1);
    CHECK(f.messages[0].nested_messages[0].fqn == ".pkg.M.Result");
}
