// Golden tests for the arena layout planner: resolve + analyze each corpus scenario, plan the
// arena layout of every message, serialize it with the layout dumper, and assert it matches a
// checked-in expected dump byte-for-byte. This pins every layout decision (field kind, padding-
// minimized member order + offsets, the bit-packed presence/value mask, fixed-size verdict, inline-
// vs-pointer sub-message choice, bool-wrapper collapse, oneof union) before any C++ is emitted.
// Regenerate with `RAPIDPROTO_REGEN_GOLDEN=1 ./build/gcc/rapidproto_tests "[arena-layout]"` and read
// the diff CAREFULLY -- like the AST goldens, these have no behavioral backstop yet.

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "arena_layout_dump.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string corpus_path(const std::string& rel) {
    return std::string(RAPIDPROTO_CORPUS_DIR) + "/" + rel;
}

std::string produce_dump(const std::string& entry) {
    ResolverConfig config;
    config.include_paths = {RAPIDPROTO_CORPUS_DIR};
    auto resolved = resolve(corpus_path(entry), config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols);
    return arenalayoutdump::dump_layouts(layouts);
}

// Locate the first differing line, for a readable failure on large dumps.
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

TEST_CASE("arena-layout: corpus layout dumps match expectations", "[arena-layout]") {
    const std::vector<std::string> scenarios = {
        "arena_layout", "proto2", "proto3", "editions2023", "xref", "packed",
    };

    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    const bool regen = std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr;

    for (const auto& name : scenarios) {
        const std::string actual = produce_dump(name + ".proto");
        const std::string golden =
            std::string(RAPIDPROTO_ARENA_LAYOUT_GOLDEN_DIR) + "/" + name + ".txt";

        if (regen) {
            std::ofstream(golden, std::ios::binary) << actual;
            WARN("regenerated arena-layout golden: " << name);
            continue;
        }

        const std::string expected = read_file(golden);
        INFO("scenario: " << name);
        INFO(first_difference(expected, actual));
        CHECK(actual == expected);
    }
}
