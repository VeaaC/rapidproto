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
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ast_dump.hpp"
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

std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

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
std::string first_difference(const std::string& expected, const std::string& actual) {
    std::istringstream exp(expected);
    std::istringstream act(actual);
    std::string exp_line;
    std::string act_line;
    int number = 1;
    while (true) {
        const bool exp_ok = static_cast<bool>(std::getline(exp, exp_line));
        const bool act_ok = static_cast<bool>(std::getline(act, act_line));
        if (!exp_ok && !act_ok) {
            return "(no line difference; trailing-newline mismatch?)";
        }
        if (exp_ok != act_ok || exp_line != act_line) {
            return "first diff at line " + std::to_string(number) +
                   ":\n  expected: " + (exp_ok ? exp_line : "<eof>") +
                   "\n  actual:   " + (act_ok ? act_line : "<eof>");
        }
        ++number;
    }
}

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

    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    const bool regen = std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr;

    for (const auto& scenario : scenarios) {
        const std::string actual = produce_dump(scenario);
        const std::string golden =
            std::string(RAPIDPROTO_GOLDEN_DIR) + "/" + scenario.name + ".txt";

        if (regen) {
            std::ofstream(golden, std::ios::binary) << actual;
            WARN("regenerated golden: " << scenario.name);
            continue;
        }

        const std::string expected = read_file(golden);
        INFO("scenario: " << scenario.name);
        INFO(first_difference(expected, actual));
        CHECK(actual == expected);
    }
}
