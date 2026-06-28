#include <catch_amalgamated.hpp>

#include <cstddef>
#include <string>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"
#include "temp_dir.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

using rapidproto::test::TempDir;

}  // namespace

TEST_CASE("resolver: a single file with no imports") {
    const TempDir dir("single");
    dir.write("a.proto", R"(syntax = "proto3"; message A {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_ok());
    CHECK(result.value().files.size() == 1);
    CHECK(result.value().files[0].messages.size() == 1);
}

TEST_CASE("resolver: an import chain is returned dependencies-first") {
    const TempDir dir("chain");
    dir.write("a.proto", R"(syntax = "proto3"; import "b.proto"; message A {})");
    dir.write("b.proto", R"(syntax = "proto3"; import "c.proto"; message B {})");
    dir.write("c.proto", R"(syntax = "proto3"; message C {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_ok());
    const ResolvedFileSet& set = result.value();
    REQUIRE(set.files.size() == 3);
    // topological: c before b before a (the entry, keyed by its include-relative name).
    CHECK(set.file_index.at("c.proto") < set.file_index.at("b.proto"));
    CHECK(set.file_index.at("b.proto") < set.file_index.at("a.proto"));
}

TEST_CASE("resolver: a diamond dependency resolves the shared file once") {
    const TempDir dir("diamond");
    dir.write("a.proto", R"(syntax = "proto3"; import "b.proto"; import "c.proto"; message A {})");
    dir.write("b.proto", R"(syntax = "proto3"; import "d.proto"; message B {})");
    dir.write("c.proto", R"(syntax = "proto3"; import "d.proto"; message C {})");
    dir.write("d.proto", R"(syntax = "proto3"; message D {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_ok());
    const ResolvedFileSet& set = result.value();
    REQUIRE(set.files.size() == 4);  // d only once
    CHECK(set.file_index.at("d.proto") < set.file_index.at("b.proto"));
    CHECK(set.file_index.at("d.proto") < set.file_index.at("c.proto"));
    CHECK(set.file_index.at("b.proto") < set.file_index.at("a.proto"));
}

TEST_CASE("resolver: an import cycle is detected") {
    const TempDir dir("cycle");
    dir.write("a.proto", R"(syntax = "proto3"; import "b.proto"; message A {})");
    dir.write("b.proto", R"(syntax = "proto3"; import "a.proto"; message B {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_err());
    CHECK(result.error().message.find("cycle") != std::string::npos);
    // The entry is keyed by its include-relative name, so the cycle goes through it and
    // the diagnostic renders the path.
    CHECK(result.error().message.find("a.proto -> b.proto -> a.proto") != std::string::npos);
}

TEST_CASE("resolver: an import chain over the depth cap is rejected, not crashed") {
    // Import resolution is a recursive DFS; a deep chain (each file imports the next) would overflow
    // the stack without the cap. Exceed it and confirm a clean error, not a crash.
    const TempDir dir("deepchain");
    const int n = 120;  // exceeds kMaxImportDepth
    for (int i = 0; i < n; ++i) {
        std::string content = R"(syntax = "proto3"; )";
        if (i + 1 < n) {
            content += "import \"f" + std::to_string(i + 1) + ".proto\"; ";
        }
        content += "message M {}";
        dir.write("f" + std::to_string(i) + ".proto", content);
    }

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("f0.proto"), config);

    REQUIRE(result.is_err());
    CHECK(result.error().message.find("import chain") != std::string::npos);
}

TEST_CASE("resolver: the first include path holding the import wins") {
    const TempDir dir("incorder");
    dir.write("first/lib.proto", R"(syntax = "proto3"; message Lib { int32 first = 1; })");
    dir.write("second/lib.proto", R"(syntax = "proto3"; message Lib { int32 second = 1; })");
    dir.write("entry.proto", R"(syntax = "proto3"; import "lib.proto"; message E {})");

    ResolverConfig config;
    config.include_paths = {dir.path("first"), dir.path("second")};
    auto result = resolve(dir.path("entry.proto"), config);

    REQUIRE(result.is_ok());
    const std::size_t idx = result.value().file_index.at("lib.proto");
    REQUIRE(result.value().files[idx].messages.size() == 1);
    CHECK(result.value().files[idx].messages[0].fields[0].name == "first");  // first dir wins
}

TEST_CASE("resolver: different spellings of the same import map to one file") {
    const TempDir dir("canon");
    dir.write("a.proto",
              R"(syntax = "proto3"; import "lib.proto"; import "./lib.proto"; message A {})");
    dir.write("lib.proto", R"(syntax = "proto3"; message Lib {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_ok());
    // "lib.proto" and "./lib.proto" are the same file -> resolved exactly once (a + lib).
    CHECK(result.value().files.size() == 2);
    CHECK(result.value().file_index.count("lib.proto") == 1);
}

TEST_CASE("resolver: a missing import is reported") {
    const TempDir dir("missing");
    dir.write("a.proto", R"(syntax = "proto3"; import "nope.proto"; message A {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_err());
    CHECK(result.error().message.find("import not found") != std::string::npos);
}

TEST_CASE("resolver: a missing entry file is reported") {
    const TempDir dir("noentry");
    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("does_not_exist.proto"), config);

    REQUIRE(result.is_err());
    CHECK(result.error().message.find("entry file not found") != std::string::npos);
}

TEST_CASE("resolver: a propagated parse error renders with file:line:col") {
    const TempDir dir("parseerr");
    dir.write("a.proto", R"(syntax = "proto3"; message A { int32 x = ; })");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    auto result = resolve(dir.path("a.proto"), config, sources);

    REQUIRE(result.is_err());
    const std::string rendered = render_error(result.error(), sources);
    INFO(rendered);
    CHECK(rendered.find("a.proto:1:") != std::string::npos);  // file:line: attribution, not bare
}

TEST_CASE("resolver: a parse error in an imported file is attributed to that file") {
    const TempDir dir("importerr");
    dir.write("main.proto", R"(syntax = "proto3"; import "bad.proto";)");
    dir.write("bad.proto", R"(syntax = "proto3"; message B { int32 y = ; })");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    auto result = resolve(dir.path("main.proto"), config, sources);

    REQUIRE(result.is_err());
    const std::string rendered = render_error(result.error(), sources);
    INFO(rendered);
    CHECK(rendered.find("bad.proto:1:") != std::string::npos);  // the IMPORTED file
    CHECK(rendered.find("main.proto") == std::string::npos);    // not the entry file
}

TEST_CASE("resolver: a semantic error renders with file:line:col at the node") {
    const TempDir dir("semerr");
    dir.write("a.proto",
              "syntax = \"proto3\";\nmessage A { Missing x = 1; }");  // Missing undefined

    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    auto resolved = resolve(dir.path("a.proto"), config, sources);
    REQUIRE(
        resolved.is_ok());  // resolve succeeds; an unresolved type is a semantic (analyze) error
    ResolvedFileSet set = std::move(resolved).value();

    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_err());
    const std::string rendered = render_error(analyzed.error(), sources);
    INFO(rendered);
    // Exact line:col -- the caret lands on the field name token `x` (line 2, column 21).
    CHECK(rendered.find("a.proto:2:21:") != std::string::npos);
    CHECK(rendered.find("unresolved type") != std::string::npos);  // the analyze error message
}

TEST_CASE("resolver: well-known imports resolve from the embedded copy") {
    const TempDir dir("wkt");
    dir.write("a.proto", R"(
        syntax = "proto3";
        import "google/protobuf/timestamp.proto";
        message A {}
    )");

    ResolverConfig config;
    config.include_paths = {dir.root()};  // no WKT on disk -> embedded fallback
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_ok());
    CHECK(result.value().files.size() == 2);
    CHECK(result.value().file_index.count("google/protobuf/timestamp.proto") == 1);
}

TEST_CASE("resolver: a disk copy overrides the embedded well-known type") {
    const TempDir dir("override");
    dir.write("a.proto", R"(
        syntax = "proto3";
        import "google/protobuf/timestamp.proto";
        message A {}
    )");
    // A custom timestamp.proto on the include path shadows the embedded WKT.
    dir.write(
        "google/protobuf/timestamp.proto",
        R"(syntax = "proto3"; package google.protobuf; message Timestamp { int32 marker = 99; })");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_ok());
    const std::size_t idx = result.value().file_index.at("google/protobuf/timestamp.proto");
    const FileNode& ts = result.value().files[idx];
    REQUIRE(ts.messages.size() == 1);
    REQUIRE(ts.messages[0].fields.size() == 1);
    CHECK(ts.messages[0].fields[0].name == "marker");  // the disk copy, not the embedded one
}

TEST_CASE("resolver: use_wellknown=false disables the embedded fallback") {
    const TempDir dir("nowkt");
    dir.write("a.proto",
              R"(syntax = "proto3"; import "google/protobuf/timestamp.proto"; message A {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    config.use_wellknown = false;
    auto result = resolve(dir.path("a.proto"), config);

    REQUIRE(result.is_err());
    CHECK(result.error().message.find("import not found") != std::string::npos);
}
