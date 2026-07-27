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
#include <initializer_list>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "arena_layout_dump.hpp"
#include "arena_modes_profile.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
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

// A planned schema, kept together because a LayoutSet holds raw AST back-pointers (MessageLayout::
// message, MemberPlan::field, ...) into the ResolvedFileSet it was planned from: dropping the set
// would dangle every one of them.
struct Planned {
    ResolvedFileSet set;
    SymbolTable symbols;
    arenagen::LayoutSet layouts;
};

// Plan an inline schema at a chosen flatten budget.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): temp-dir name vs schema text, distinct roles
Planned plan_at_budget(const std::string& name, const std::string& schema, std::size_t budget) {
    const test::TempDir dir(name);
    dir.write("s.proto", schema);
    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto resolved = resolve(dir.path("s.proto"), config);
    REQUIRE(resolved.is_ok());
    Planned out;
    out.set = std::move(resolved).value();
    auto analyzed = analyze(out.set);
    REQUIRE(analyzed.is_ok());
    out.symbols = std::move(analyzed).value();
    arenagen::LayoutOptions options;
    options.flatten_budget = budget;
    out.layouts = arenagen::plan_layouts(out.set, out.symbols, options);
    return out;
}

const arenagen::MessageLayout& planned(const Planned& plan, const std::string& fqn) {
    const arenagen::MessageLayout* layout = plan.layouts.find(fqn);
    REQUIRE(layout != nullptr);
    return *layout;
}

bool marked(const Planned& plan, const std::string& fqn) {
    return planned(plan, fqn).noinline_decode;
}

}  // namespace

// The budget pass is otherwise covered only by golden dumps, which pin whatever it happens to do.
// These pin the RULES, so a retune shows up as a deliberate golden change and a rule change fails
// a named assertion instead.

TEST_CASE("arena-layout: the flatten budget never marks a message that inlines no closure",
          "[arena-layout]") {
    // Leaf6 is wide but flat; Inner blows the budget through Leaf6 and is marked. Wide then inlines
    // NOTHING (its only target is already marked), so marking it would bound nothing below and only
    // cost a call -- exactly the reason a leaf is exempt. WideLeaf is the control: the same ten arms
    // with no message field at all.
    const std::string schema =
        "syntax = \"proto3\";\npackage f;\n"
        "message Leaf6 { int32 a=1; int32 b=2; int32 c=3; int32 d=4; int32 e=5; int32 f=6; }\n"
        "message Inner { int32 a=1; int32 b=2; int32 c=3; int32 d=4; Leaf6 l=5; }\n"
        "message Wide { int32 a=1; int32 b=2; int32 c=3; int32 d=4; int32 e=5; int32 f=6;"
        " int32 g=7; int32 h=8; int32 i=9; int32 j=10; Inner n=11; }\n"
        "message WideLeaf { int32 a=1; int32 b=2; int32 c=3; int32 d=4; int32 e=5; int32 f=6;"
        " int32 g=7; int32 h=8; int32 i=9; int32 j=10; }\n";
    const Planned plan = plan_at_budget("layout_flatten_leaf", schema, 4);

    CHECK(marked(plan, ".f.Inner"));        // 5 own arms + Leaf6's 6 = 11 > 4, and it inlines Leaf6
    CHECK_FALSE(marked(plan, ".f.Leaf6"));  // a leaf, however wide
    CHECK_FALSE(marked(plan, ".f.WideLeaf"));  // a leaf, however wide
    CHECK_FALSE(marked(plan, ".f.Wide"));      // inlines nothing: Inner is already marked
    // Wide's cost counts its own 11 arms and NOTHING from the marked Inner -- the marked flag, not
    // the cost value, is what stops a closure accumulating upward.
    CHECK(planned(plan, ".f.Wide").flatten_cost == 11);
    CHECK(planned(plan, ".f.Inner").flatten_cost == 11);  // kept after marking, for review
}

TEST_CASE("arena-layout: the flatten budget marks strictly above the budget, not at it",
          "[arena-layout]") {
    // Pins `cost > budget` against `cost >= budget`: a `>=` typo would flip AtBudget and read as a
    // plausible retune rather than a rule change.
    const std::string schema =
        "syntax = \"proto3\";\npackage b;\n"
        "message L { int32 a=1; }\n"
        "message AtBudget { int32 x=1; int32 y=2; L l=3; }\n"                // 3 own + L's 1 = 4
        "message OverBudget { int32 x=1; int32 y=2; int32 z=3; L l=4; }\n";  // 4 own + 1 = 5
    const Planned plan = plan_at_budget("layout_flatten_edge", schema, 4);

    CHECK_FALSE(marked(plan, ".b.AtBudget"));  // cost == budget stays flattened
    CHECK(marked(plan, ".b.OverBudget"));      // cost == budget + 1 is marked
}

TEST_CASE("arena-layout: a oneof member's sub-message counts toward the flatten budget",
          "[arena-layout]") {
    // Oneof members are decode arms and their targets are inlined like any other, but a golden
    // whose message is over budget on its plain fields alone cannot tell. Holder trips the budget
    // ONLY through the oneof: 2 own arms + Leaf6's 6. OneofOnly is the leaf control -- five oneof
    // members, no sub-message, so it stays flattened however many arms it has.
    const std::string schema =
        "syntax = \"proto3\";\npackage o;\n"
        "message Leaf6 { int32 a=1; int32 b=2; int32 c=3; int32 d=4; int32 e=5; int32 f=6; }\n"
        "message Holder { oneof k { int32 i=1; Leaf6 l=2; } }\n"
        "message OneofOnly { oneof k { int32 a=1; int32 b=2; int32 c=3; int32 d=4; int32 e=5; } "
        "}\n";
    const Planned plan = plan_at_budget("layout_flatten_oneof", schema, 4);

    CHECK(planned(plan, ".o.Holder").flatten_cost == 8);  // 2 oneof arms + Leaf6's 6
    CHECK(marked(plan, ".o.Holder"));
    CHECK(planned(plan, ".o.OneofOnly").flatten_cost == 5);  // oneof members are arms
    CHECK_FALSE(marked(plan, ".o.OneofOnly"));               // ... but it inlines nothing
}

TEST_CASE("arena-layout: a flatten-budget cycle is broken at one message", "[arena-layout]") {
    // Mutual recursion has to be broken somewhere, and exactly one message is enough: the other
    // stays flattened, and its flatten stops at the marked one.
    const std::string schema =
        "syntax = \"proto3\";\npackage c;\n"
        "message A { int32 a=1; B b=2; }\n"
        "message B { int32 x=1; A a=2; }\n";
    const Planned plan = plan_at_budget("layout_flatten_cycle", schema, 4);

    const int marks =
        static_cast<int>(marked(plan, ".c.A")) + static_cast<int>(marked(plan, ".c.B"));
    CHECK(marks == 1);
    // The unmarked one accumulates only its own arms, because its target is marked.
    const std::string open = marked(plan, ".c.A") ? ".c.B" : ".c.A";
    CHECK(planned(plan, open).flatten_cost == 2);
}

TEST_CASE("arena-layout: flatten budget 0 disables the pass", "[arena-layout]") {
    // Budget 0 means flatten everything, and is otherwise unreachable from the test suite --
    // nothing else exercises the pass's early return.
    const std::string schema =
        "syntax = \"proto3\";\npackage z;\n"
        "message Leaf6 { int32 a=1; int32 b=2; int32 c=3; int32 d=4; int32 e=5; int32 f=6; }\n"
        "message Inner { int32 a=1; int32 b=2; int32 c=3; int32 d=4; Leaf6 l=5; }\n"
        "message Outer { int32 a=1; Inner n=2; }\n";
    const Planned plan = plan_at_budget("layout_flatten_off", schema, 0);

    for (const arenagen::MessageLayout& layout : plan.layouts.layouts) {
        INFO("message " << layout.fqn);
        CHECK_FALSE(layout.noinline_decode);
        CHECK(layout.flatten_cost == 0);  // nothing was computed, not "costs nothing"
    }
    // Same schema at budget 4 does mark, so the case above is a real disable and not a schema that
    // simply never trips the budget.
    CHECK(marked(plan_at_budget("layout_flatten_on", schema, 4), ".z.Inner"));
}

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
TEST_CASE("arena-layout: field modes reshape the plan (raw members, dropped fields)",
          "[arena-layout]") {
    ResolverConfig config;
    config.include_paths = {RAPIDPROTO_CORPUS_DIR};
    auto resolved = resolve(corpus_path("arena_modes.proto"), config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    const arenagen::FieldModes modes = test::arena_modes_profile(set, symbols);
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    const std::string actual = arenalayoutdump::dump_layouts(layouts);

    const std::string golden = std::string(RAPIDPROTO_ARENA_LAYOUT_GOLDEN_DIR) + "/arena_modes.txt";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    if (std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr) {
        std::ofstream(golden, std::ios::binary) << actual;
        WARN("regenerated arena-layout golden: arena_modes");
        return;
    }
    const std::string expected = read_file(golden);
    INFO(first_difference(expected, actual));
    CHECK(actual == expected);
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
