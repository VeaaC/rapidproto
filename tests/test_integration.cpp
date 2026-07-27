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

// The real-world corpus lives in the gitignored build/corpus, fetched at pinned refs by
// tests/fetch_corpus.py. Nothing is vendored, so a contributor is never required to download
// it; these cases SKIP (not silently pass) when it is absent.
//
// Every case is tagged `[.corpus]`: the leading dot HIDES it from a default
// `rapidproto_tests` run, so a machine that HAS fetched the corpus still doesn't pay for it
// on the default gate path. Sweeping 8000+ schemas costs ~47s, and check.sh runs the test
// binary unfiltered four times (gcc, clang, and the deep tier's sanitizer + coverage builds),
// where under ASan it is far worse. Run them deliberately:
//     ./build/gcc/rapidproto_tests [corpus]
// One marker file per fetched source (mirrors each Source.probe in tests/fetch_corpus.py).
// All of them are checked, not just the first: googleapis alone is 99.7% of the corpus, so
// probing only protobuf would let a corpus that is 7 files out of 8018 look complete.
constexpr const char* kSourceProbes[] = {
    "protobuf/src/google/protobuf/descriptor.proto",
    "protobuf-benchmarks/benchmarks/benchmarks.proto",
    "googleapis/google/rpc/status.proto",
};

std::optional<std::filesystem::path> find_corpus_dir() {
    // Relative to the plausible CWDs the test binary is launched from.
    const std::filesystem::path candidates[] = {
        "build/corpus",        // CWD = repo root (how check.sh runs the binary)
        "../corpus",           // CWD = build/<preset>
        "../../build/corpus",  // CWD = build/<preset> (alternate layout)
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_directory(candidate)) {
            return std::filesystem::canonical(candidate);
        }
    }
    return std::nullopt;
}

// The protobuf checkout's include root: the directory an `import "google/protobuf/..."`
// resolves against.
std::filesystem::path protobuf_root(const std::filesystem::path& corpus) {
    return corpus / "protobuf" / "src";
}

// The corpus root, or SKIP out of the calling test case if it has not been fetched.
//
// SKIP rather than SUCCEED: an absent corpus must read as "not checked", never as a green
// assertion -- these cases are the only thing standing between the hand-written front-end and
// a schema shape protoc accepts but RapidProto does not. (Catch2's SKIP does not return, so
// the caller can use the result unconditionally.)
//
// A PARTIAL corpus is a hard failure, not a skip. Absent means "you chose not to fetch";
// partial means a fetch broke, and silently checking a fraction of the schemas while
// reporting the same green result is the failure mode worth being loudest about.
std::filesystem::path require_corpus() {
    const std::optional<std::filesystem::path> dir = find_corpus_dir();
    std::vector<std::string> missing;
    if (dir) {
        for (const char* probe : kSourceProbes) {
            if (!std::filesystem::is_regular_file(*dir / probe)) {
                missing.emplace_back(probe);
            }
        }
    }
    if (!dir || missing.size() == std::size(kSourceProbes)) {
        SKIP("corpus not fetched; run `python3 tests/fetch_corpus.py` (see CONTRIBUTING)");
    }
    for (const auto& gone : missing) {
        FAIL_CHECK("corpus is incomplete, missing: " << gone);
    }
    REQUIRE(missing.empty());  // re-run tests/fetch_corpus.py; a partial corpus checks nothing
    return *dir;
}

// nullopt when the file cannot be read, so an unreadable schema is reported rather than
// silently lexing as empty (which parses clean and would count as a pass).
std::optional<std::string> read_file(const std::filesystem::path& path) {
    const std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    // Not checking `contents` afterwards: inserting zero characters sets its failbit, which a
    // legitimately empty .proto (valid, and present in real trees) would trip. The open check
    // above is what catches an unreadable file.
    return contents.str();
}

}  // namespace

// Also tagged [sweep] so the gate can exclude it: check.sh's corpus stage drives every schema
// through rapidprotoc, which parses all of them on the way to generating, so re-lexing and
// re-parsing the same 8018 files costs ~47s to re-prove a strict subset. Kept for direct
// front-end diagnosis: `rapidproto_tests [sweep]` reports lex/parse errors per file, where
// rapidprotoc reports only the first error of a whole pipeline run.
TEST_CASE("integration: every corpus .proto lexes and parses end to end", "[.corpus][sweep]") {
    const std::filesystem::path root = require_corpus();

    std::vector<std::string> failures;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".proto") {
            continue;
        }
        ++count;
        const std::string name = entry.path().string();
        auto text = read_file(entry.path());
        if (!text) {
            failures.push_back(name + ": unreadable");
            continue;
        }
        auto lexed = lex(*std::move(text));
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

    // One INFO carrying every failure, not one per iteration: a scoped INFO inside the loop
    // is destroyed at the end of its iteration, so by the time the CHECK below runs none of
    // them are live and the report says only how many failed, never which.
    std::string detail;
    for (const auto& failure : failures) {
        detail += "\n  " + failure;
    }
    INFO("parsed " << count << " files; " << failures.size() << " failed" << detail);
    // googleapis alone is ~7993 schemas. A floor well below that catches a truncated fetch
    // (an interrupted checkout leaves a partial tree) without being brittle to a pin bump.
    CHECK(count > 5000);
    CHECK(failures.empty());
}

TEST_CASE("integration: resolve a real multi-file schema with WKT imports", "[.corpus]") {
    const std::filesystem::path root = require_corpus();

    // The conformance suite's proto3 message schema imports seven well-known types. None of
    // the seven is fetched to disk (the sparse patterns pull only descriptor.proto,
    // plugin.proto and the two test_messages schemas), so resolving them can only come from
    // the EMBEDDED well-known types -- and every dependency must precede the entry in the
    // topological order. If a future pin bump adds any of these seven to the fetched set,
    // this silently becomes a disk-resolution test instead.
    ResolverConfig config;
    config.include_paths = {protobuf_root(root).string()};
    const std::string rel = "google/protobuf/test_messages_proto3.proto";
    auto result = resolve((protobuf_root(root) / rel).string(), config);

    REQUIRE(result.is_ok());
    const ResolvedFileSet& set = result.value();
    for (const char* wkt : {"google/protobuf/any.proto", "google/protobuf/duration.proto",
                            "google/protobuf/empty.proto", "google/protobuf/field_mask.proto",
                            "google/protobuf/struct.proto", "google/protobuf/timestamp.proto",
                            "google/protobuf/wrappers.proto"}) {
        INFO("well-known import: " << wkt);
        REQUIRE(set.file_index.count(wkt) == 1);  // REQUIRE: .at() below would throw instead
        CHECK(set.file_index.at(wkt) < set.file_index.at(rel));
    }
}

TEST_CASE("integration: descriptor.proto resolves end to end", "[.corpus]") {
    const std::filesystem::path root = require_corpus();

    ResolverConfig config;
    config.include_paths = {protobuf_root(root).string()};
    auto result = resolve(
        (protobuf_root(root) / "google" / "protobuf" / "descriptor.proto").string(), config);

    REQUIRE(result.is_ok());
    REQUIRE(result.value().files.size() == 1);  // descriptor.proto has no imports
    const FileNode& descriptor = result.value().files[0];
    CHECK(descriptor.syntax_level == SyntaxLevel::Proto2);
    CHECK_FALSE(descriptor.messages.empty());
}

TEST_CASE("integration: the editions conformance schema resolves end to end", "[.corpus]") {
    const std::filesystem::path root = require_corpus();

    // test_messages_edition2023.proto is the only real-world EDITIONS schema published
    // anywhere; RapidProto's editions support is otherwise exercised solely against its own
    // synthetic tests/corpus/editions*.proto. It imports nothing, so a default ResolverConfig
    // (no include paths at all) must be enough to resolve it.
    const ResolverConfig config;
    const std::filesystem::path entry =
        root / "protobuf" / "conformance" / "test_protos" / "test_messages_edition2023.proto";
    auto result = resolve(entry.string(), config);

    REQUIRE(result.is_ok());
    REQUIRE(result.value().files.size() == 1);
    const FileNode& editions = result.value().files[0];
    CHECK(editions.syntax_level == SyntaxLevel::Edition);
    CHECK(editions.edition == "2023");
    CHECK_FALSE(editions.messages.empty());
}
