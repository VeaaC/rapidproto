#include <catch_amalgamated.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

// The corpus (google WKTs + the HERE schemas) lives in the gitignored build/schema.
// Locate it relative to a few plausible CWDs; nullopt if it isn't present.
std::optional<std::filesystem::path> find_corpus_root() {
    const std::filesystem::path candidates[] = {
        "build/schema",        // CWD = repo root (how check.sh runs the binary)
        "../schema",           // CWD = build/<preset>
        "../../build/schema",  // CWD = build/<preset> (alternate layout)
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "google" / "protobuf" / "descriptor.proto")) {
            return std::filesystem::canonical(candidate);
        }
    }
    return std::nullopt;
}

std::string read_file(const std::filesystem::path& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST_CASE("integration: every corpus .proto lexes and parses end to end") {
    const auto root = find_corpus_root();
    if (!root) {
        SUCCEED("corpus (build/schema) not present; skipping");
        return;
    }

    std::vector<std::string> failures;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(*root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".proto") {
            continue;
        }
        ++count;
        const std::string name = entry.path().string();
        auto lexed = lex(read_file(entry.path()));
        if (!lexed) {
            failures.push_back(name + ": lex: " + lexed.error().message);
            continue;
        }
        const LexResult tokens = std::move(lexed).value();
        auto parsed = parse_file(Range<Token>(tokens.tokens));
        if (!parsed) {
            failures.push_back(name + ": parse: " + parsed.error().message);
        } else if (!parsed.value().remaining.empty()) {
            failures.push_back(name + ": trailing tokens unconsumed");
        }
    }

    INFO("parsed " << count << " files; " << failures.size() << " failed");
    for (const auto& failure : failures) {
        INFO(failure);
    }
    CHECK(count > 0);
    CHECK(failures.empty());
}

TEST_CASE("integration: resolve a real multi-file schema with WKT imports") {
    const auto root = find_corpus_root();
    if (!root) {
        SUCCEED("corpus (build/schema) not present; skipping");
        return;
    }

    ResolverConfig config;
    config.include_paths = {root->string()};
    const std::string rel = "com/here/schema/datetime/v2/datetime.proto";
    auto result = resolve((*root / rel).string(), config);

    REQUIRE(result.is_ok());
    const ResolvedFileSet& set = result.value();
    // datetime.proto + its two google WKT imports (duration, timestamp).
    CHECK(set.file_index.count("google/protobuf/duration.proto") == 1);
    CHECK(set.file_index.count("google/protobuf/timestamp.proto") == 1);
    // dependencies precede the entry (keyed by its include-relative name).
    CHECK(set.file_index.at("google/protobuf/timestamp.proto") < set.file_index.at(rel));
}

TEST_CASE("integration: descriptor.proto resolves end to end") {
    const auto root = find_corpus_root();
    if (!root) {
        SUCCEED("corpus (build/schema) not present; skipping");
        return;
    }

    ResolverConfig config;
    config.include_paths = {root->string()};
    auto result = resolve((*root / "google/protobuf/descriptor.proto").string(), config);

    REQUIRE(result.is_ok());
    REQUIRE(result.value().files.size() == 1);  // descriptor.proto has no imports
    const FileNode& descriptor = result.value().files[0];
    CHECK(descriptor.syntax_level == SyntaxLevel::Proto2);
    CHECK_FALSE(descriptor.messages.empty());
}
