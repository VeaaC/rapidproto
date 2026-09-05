// Golden tests: resolve + analyze each corpus scenario, serialize the resulting syntax
// tree with the AST dumper, and assert it matches a checked-in expected dump byte-for-byte.
// This pins the exact shape of the parsed/normalized tree for every protobuf feature we
// support. Regenerate with `tests/regen_goldens.sh` (all goldens), or just these with
// `RAPIDPROTO_REGEN_GOLDEN=1 ./build/gcc/rapidproto_tests "[golden]"`.
// Review the diff CAREFULLY: unlike the wire and streamgen goldens (which are also compiled
// and decoded at runtime), these AST dumps have no behavioral backstop, so a wrong regeneration is
// caught only by reading the diff.

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "ast_dump.hpp"
#include "golden_file.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

struct Scenario {
    std::string name;                          // golden basename (<name>.txt)
    std::string entry;                         // entry file, relative to the corpus dir
    std::vector<std::string> include_subdirs;  // include paths relative to corpus dir (empty=root)
};

std::string corpus_path(const std::string& rel) {
    return std::string(RAPIDPROTO_CORPUS_DIR) + "/" + rel;
}

std::string produce_dump(const Scenario& scenario) {
    ResolverConfig config;
    if (scenario.include_subdirs.empty()) {
        config.include_paths = {RAPIDPROTO_CORPUS_DIR};
    } else {
        for (const auto& sub : scenario.include_subdirs) {
            config.include_paths.push_back(std::string(RAPIDPROTO_CORPUS_DIR) + "/" + sub);
        }
    }
    auto resolved = resolve(corpus_path(scenario.entry), config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    return astdump::dump_ast(set, analyzed.value());
}

// Locate the first line that differs, for a readable failure message on large dumps.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): expected vs actual, distinct roles
}  // namespace

TEST_CASE("golden: corpus AST dumps match expectations", "[golden]") {
    const std::vector<Scenario> scenarios = {
        {"proto2", "proto2.proto", {}},
        {"proto3", "proto3.proto", {}},
        {"editions2023", "editions2023.proto", {}},
        {"editions2024", "editions2024.proto", {}},
        {"options", "options.proto", {}},
        {"imports", "imports/main.proto", {"imports"}},
    };

    for (const auto& scenario : scenarios) {
        test::check_golden(std::string(RAPIDPROTO_GOLDEN_DIR) + "/" + scenario.name + ".txt",
                           scenario.name, produce_dump(scenario));
    }
}
