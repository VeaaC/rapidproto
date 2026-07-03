// Golden tests for the arena layout planner: resolve + analyze each corpus scenario, plan the
// arena layout of every message, serialize it with the layout dumper, and assert it matches a
// checked-in expected dump byte-for-byte. This pins every layout decision (field kind, padding-
// minimized member order + offsets, the bit-packed presence/value mask, fixed-size verdict, inline-
// vs-pointer sub-message choice, oneof union) before any C++ is emitted.
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
#include "temp_dir.hpp"

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear pipeline of assertions
TEST_CASE("arena-layout: a deep wrapper-first reference chain is planned without deep recursion",
          "[arena-layout]") {
    // M300 { required M299 f; } ... M2 { required M1 f; } M1 { required int32 v; }, declared
    // wrapper-first so the planner must chase forward references (declared bottom-up, memoization
    // keeps the chain shallow and nothing interesting happens). `required` carries no presence
    // bit, so every link is fixed-size 4 bytes and would inline FOREVER -- the reference-chain
    // recursion is unbounded in a protoc-valid schema. Past kMaxChainDepth the planner degrades
    // the sub-message to pointer storage -- the cycle-back-edge fallback -- instead of recursing
    // toward a stack overflow. Links computed later (shallow, memoized) still inline.
    constexpr int kChainLen = 300;  // > the planner's kMaxChainDepth (200)
    std::string schema = "syntax = \"proto2\";\npackage chain;\n";
    for (int i = kChainLen; i >= 2; --i) {
        schema += "message M" + std::to_string(i) + " { required M" + std::to_string(i - 1) +
                  " f = 1; }\n";
    }
    schema += "message M1 { required int32 v = 1; }\n";
    const test::TempDir dir("layout_chain");
    dir.write("chain.proto", schema);

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto resolved = resolve(dir.path("chain.proto"), config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols);

    REQUIRE(layouts.layouts.size() == kChainLen);  // every message got a plan
    // A shallow link inlines its 4-byte fixed-size target (without the depth cap the WHOLE chain
    // would: every link is fixed-size 4); the top of the chain, planned at capped depth, holds a
    // pointer instead.
    const arenagen::MessageLayout* m2 = layouts.find(".chain.M2");
    REQUIRE(m2 != nullptr);
    REQUIRE(m2->members.size() == 1);
    CHECK(m2->members[0].kind == arenagen::FieldKind::InlineFixedSubMsg);
    CHECK(m2->size == 4);
    const arenagen::MessageLayout* top = layouts.find(".chain.M" + std::to_string(kChainLen));
    REQUIRE(top != nullptr);
    REQUIRE(top->members.size() == 1);
    CHECK(top->members[0].kind == arenagen::FieldKind::PointerSubMsg);
}
