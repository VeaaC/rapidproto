#include <catch_amalgamated.hpp>

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"
#include "temp_dir.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

using rapidproto::test::TempDir;

// `<file>:<line>:<col>: msg` -> the line number, or 0 when the rendering does not name `file`.
int rendered_line(const std::string& rendered, const std::string& file) {
    const std::size_t at = rendered.find(file + ":");
    if (at == std::string::npos) {
        return 0;
    }
    const std::size_t start = at + file.size() + 1;
    const std::size_t end = rendered.find(':', start);
    if (end == std::string::npos || end == start) {
        return 0;
    }
    int line = 0;
    for (std::size_t i = start; i < end; ++i) {
        if (std::isdigit(static_cast<unsigned char>(rendered[i])) == 0) {
            return 0;  // not a line number: let the caller's REQUIRE say so rather than throwing
        }
        line = line * 10 + (rendered[i] - '0');
    }
    return line;
}

// Keeps a trailing partial line: dropping it would put a "line is inside the file" check off
// by one for any text not ending in a newline.
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    for (std::size_t next = text.find('\n'); next != std::string::npos;
         next = text.find('\n', pos)) {
        lines.push_back(text.substr(pos, next - pos));
        pos = next + 1;
    }
    if (pos < text.size()) {
        lines.push_back(text.substr(pos));  // a trailing partial line still counts
    }
    return lines;
}

// `depth` nested messages, one per line, so a reported line number is countable by hand.
std::string nested_messages(int depth) {
    std::string text = "syntax = \"proto3\";\n";
    for (int i = 0; i < depth; ++i) {
        text += "message M" + std::to_string(i) + " {\n";
    }
    for (int i = 0; i < depth; ++i) {
        text += "}\n";
    }
    return text;
}

// Resolves `text` and returns the line its depth-cap diagnostic names, checking on the way that the
// position is INSIDE the file (the byte offset ran off the end) and names a line that opens a
// message (the offending token), rather than one a byte count happened to land on.
int depth_error_line(const TempDir& dir, const std::string& text) {
    dir.write("deep.proto", text);
    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    auto result = resolve(dir.path("deep.proto"), config, sources);
    REQUIRE(result.is_err());
    const std::string rendered = render_error(result.error(), sources);
    INFO(rendered);
    CHECK(rendered.find("maximum nesting depth exceeded") != std::string::npos);
    const std::vector<std::string> lines = split_lines(text);
    const int line = rendered_line(rendered, "deep.proto");
    REQUIRE(line >= 1);
    REQUIRE(line <= static_cast<int>(lines.size()));
    CHECK(lines[static_cast<std::size_t>(line) - 1].rfind("message ", 0) == 0);
    return line;
}

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

TEST_CASE("resolver: a depth-cap error lands on the offending token, not a byte offset") {
    // The parser reports failure positions as TOKEN INDICES and the resolver maps them back to a
    // source position. too_deep() stored the token's BYTE offset in that slot, so the diagnostic
    // landed in the wrong column -- and on a file with more bytes than tokens, past the end of it.
    //
    // Both files are over the cap, so the position is decided by the CAP and must be identical --
    // which is what this can assert without seeing kMaxParseDepth. A position that tracks the file
    // instead (a byte offset, or one lifted by the wrong amount) moves with the depth.
    const TempDir dir("deepcap");
    CHECK(depth_error_line(dir, nested_messages(60)) == depth_error_line(dir, nested_messages(80)));
}

TEST_CASE("resolver: a depth-cap error inside an option value lands on the right column") {
    // The same bug in an aggregate. This is the tighter of the two shapes: the exact column pins
    // the token, where a line check would still pass if the offset were lifted by the wrong amount
    // within the line. (On this fixture the old code also ran past EOF, reporting 3:1.)
    const TempDir dir("deepcol");
    const std::string opens(51, '[');
    const std::string closes(51, ']');
    dir.write("d.proto", "syntax = \"proto3\";\noption x = " + opens + closes + ";\n");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    auto result = resolve(dir.path("d.proto"), config, sources);
    REQUIRE(result.is_err());
    const std::string rendered = render_error(result.error(), sources);
    INFO(rendered);
    CHECK(rendered.find("maximum nesting depth exceeded") != std::string::npos);
    // "option x = " is 11 characters, so the k-th '[' sits at column 11 + k. The cap is 50, so the
    // 51st bracket is the first one over it: column 62.
    CHECK(rendered.find("d.proto:2:62:") != std::string::npos);
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

TEST_CASE("resolver: a batch resolves shared imports once, in one topological order") {
    const TempDir dir("batch");
    dir.write("a.proto", R"(syntax = "proto3"; import "shared.proto"; message A {})");
    dir.write("b.proto", R"(syntax = "proto3"; import "shared.proto"; message B {})");
    dir.write("shared.proto", R"(syntax = "proto3"; message S {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    auto result = resolve({dir.path("a.proto"), dir.path("b.proto")}, config, sources);

    REQUIRE(result.is_ok());
    const ResolvedFileSet& set = result.value();
    REQUIRE(set.files.size() == 3);  // shared.proto once, not per entry
    CHECK(set.file_index.at("shared.proto") < set.file_index.at("a.proto"));
    CHECK(set.file_index.at("shared.proto") < set.file_index.at("b.proto"));
}

TEST_CASE("resolver: an entry that another entry imports resolves once, in either order") {
    const TempDir dir("batch_entryimport");
    dir.write("a.proto", R"(syntax = "proto3"; import "b.proto"; message A {})");
    dir.write("b.proto", R"(syntax = "proto3"; message B {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    for (const auto& entries :
         {std::vector<std::string>{dir.path("a.proto"), dir.path("b.proto")},
          std::vector<std::string>{dir.path("b.proto"), dir.path("a.proto")},
          // b spelled as a DISK PATH entry while a's import says "b.proto":
          // the canonical-name identity must collapse the two spellings.
          std::vector<std::string>{dir.root() + "/b.proto", dir.path("a.proto")}}) {
        SourceRegistry sources;
        auto result = resolve(entries, config, sources);
        REQUIRE(result.is_ok());
        const ResolvedFileSet& set = result.value();
        REQUIRE(set.files.size() == 2);  // b once: entry and import are the same file
        CHECK(set.file_index.at("b.proto") < set.file_index.at("a.proto"));
    }
}

TEST_CASE("resolver: a batch spans nested folders with cross-folder imports") {
    const TempDir dir("batch_nested");
    dir.write("top.proto", R"(syntax = "proto3"; import "sub/inner.proto"; message T {})");
    dir.write("sub/inner.proto", R"(syntax = "proto3"; import "sub/deep/leaf.proto";
                                    message I {})");
    dir.write("sub/deep/leaf.proto", R"(syntax = "proto3"; message L {})");

    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    // The nested files are BOTH entries and imports; each must key to its import-relative name.
    auto result = resolve(
        {dir.path("top.proto"), dir.path("sub/inner.proto"), dir.path("sub/deep/leaf.proto")},
        config, sources);

    REQUIRE(result.is_ok());
    const ResolvedFileSet& set = result.value();
    REQUIRE(set.files.size() == 3);
    CHECK(set.file_index.at("sub/deep/leaf.proto") < set.file_index.at("sub/inner.proto"));
    CHECK(set.file_index.at("sub/inner.proto") < set.file_index.at("top.proto"));
}
