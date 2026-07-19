// Generated arena-decoder tests for struct + accessor emission. (1) A golden test that regenerates
// each *.rp.hpp and compares byte-for-byte to the checked-in copy. (2) A compile-smoke: every
// checked-in golden is #included below, so the generated structs/accessors must be valid C++.
// Runtime decode behavior is covered separately in test_arena_decode.cpp.
//
// Regenerate after an intentional generator change with: tests/regen_goldens.sh (the in-test
// RAPIDPROTO_REGEN_GOLDEN mode can't rebuild this binary when a change breaks the headers it
// #includes).

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "arena_modes_profile.hpp"
#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "temp_dir.hpp"
// Checked-in generated headers (compile-smoke: they must be valid C++).
// IWYU pragma: begin_keep
#include "arenagen_golden/arena_layout.rp.hpp"
#include "arenagen_golden/arena_manyreq.rp.hpp"  // >64 required: multi-word rp_req
#include "arenagen_golden/arena_modes.rp.hpp"    // field modes: raw payloads + dropped fields
#include "arenagen_golden/arena_naming.rp.hpp"   // identifier dedup: must compile
#include "arenagen_golden/editions2023.rp.hpp"
#include "arenagen_golden/editions2024.rp.hpp"  // 2024: decode-relevant defaults match 2023
#include "arenagen_golden/main.rp.hpp"   // cross-file imports: transitively pulls dep/forward/pub
#include "arenagen_golden/nopkg.rp.hpp"  // NO package: types land at global scope
#include "arenagen_golden/prefixed/main.rp.hpp"  // --namespace-prefix + imports (pulls prefixed dep/...)
#include "arenagen_golden/proto2.rp.hpp"
#include "arenagen_golden/proto3.rp.hpp"
#include "arenagen_golden/samepkg_a.rp.hpp"  // same-package multi-file (pulls samepkg_b): ODR guard
#include "arenagen_golden/weakmain.rp.hpp"  // weak import (pulls weakdep): filtered like a normal one
#include "arenagen_golden/wire_all.rp.hpp"  // group + packed (generated from the fixtures dir)
#include "arenagen_golden/xpkg.rp.hpp"  // dotted package (pulls deep): namespace com::example::deep
#include "arenagen_golden/xref.rp.hpp"
#include "arenagen_golden/xref_prefixed/xref.rp.hpp"  // --namespace-prefix=rp -> namespace rp::xr
// IWYU pragma: end_keep

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): include dir, entry file, namespace prefix
std::string generate(const std::string& dir, const std::string& entry,
                     const std::string& prefix = {}) {
    ResolverConfig config;
    config.include_paths = {dir};
    auto resolved = resolve(dir + "/" + entry, config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    return arenagen::generate_header(set.files.back(), set, symbols, prefix);
}
std::string generate_corpus(const std::string& entry, const std::string& prefix = {}) {
    return generate(RAPIDPROTO_CORPUS_DIR, entry, prefix);
}

// Generate arena_modes.rp.hpp + its sibling common header under the shared test profile. The
// regen script produces the same pair through the CLI and tests/corpus/arena_modes.modes; this
// in-process generation is the DRIFT CHECK between the two profile spellings (the golden
// comparison fails if the .modes file and arena_modes_profile.hpp ever disagree).
struct ModesOutput {
    std::string header;
    std::string common;
};
ModesOutput generate_modes_golden() {
    ResolverConfig config;
    config.include_paths = {RAPIDPROTO_CORPUS_DIR};
    auto resolved = resolve(std::string(RAPIDPROTO_CORPUS_DIR) + "/arena_modes.proto", config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    const arenagen::FieldModes modes = test::arena_modes_profile(set, symbols);
    const codegen::CppNameTable names = codegen::build_cpp_names(set.files.back(), set.files, "");
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    return {arenagen::generate_header(set.files.back(), names, layouts, symbols, &modes),
            codegen::emit_common_header(set.files.back(), names)};
}

// Generate arena_unknown.rp.hpp under --unknown-present (unknown_all): every message reserves its
// has_unknown_fields() bit, and the flag folds into the profile identity (an inline rp_modes_<id>
// namespace). Byte-pinning this golden guards the fold -- the banner id and the inline namespace must
// not drift silently, since a mismatch is exactly the ODR hazard the fold closes.
std::string generate_unknown_present_golden() {
    ResolverConfig config;
    config.include_paths = {RAPIDPROTO_CORPUS_DIR};
    auto resolved = resolve(std::string(RAPIDPROTO_CORPUS_DIR) + "/arena_unknown.proto", config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    arenagen::FieldModesSpec spec;
    spec.unknown_all = true;
    auto resolved_modes = arenagen::resolve_field_modes(spec, set, symbols);
    REQUIRE(resolved_modes.is_ok());
    const arenagen::FieldModes modes = std::move(resolved_modes).value();
    const codegen::CppNameTable names = codegen::build_cpp_names(set.files.back(), set.files, "");
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    return arenagen::generate_header(set.files.back(), names, layouts, symbols, &modes);
}

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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): golden name vs generated content
void check_golden(const std::string& name, const std::string& actual) {
    const std::string golden = std::string(RAPIDPROTO_ARENAGEN_GOLDEN_DIR) + "/" + name + ".rp.hpp";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    if (std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr) {
        std::ofstream(golden, std::ios::binary) << actual;
        WARN("regenerated arenagen golden: " << name);
        return;
    }
    INFO("golden: " << name);
    INFO(first_difference(read_file(golden), actual));
    CHECK(actual == read_file(golden));
}

}  // namespace

TEST_CASE("arenagen: generated headers match the goldens", "[arenagen]") {
    check_golden("arena_layout", generate_corpus("arena_layout.proto"));
    check_golden("arena_manyreq", generate_corpus("arena_manyreq.proto"));
    check_golden("arena_naming", generate_corpus("arena_naming.proto"));
    check_golden("proto2", generate_corpus("proto2.proto"));
    check_golden("proto3", generate_corpus("proto3.proto"));
    check_golden("editions2023", generate_corpus("editions2023.proto"));
    check_golden("editions2024", generate_corpus("editions2024.proto"));
    // drop/raw under the shared profile; the sibling common is golden-checked too, so it cannot
    // drift from emit_common_header output between regens.
    const ModesOutput modes = generate_modes_golden();
    check_golden("arena_modes", modes.header);
    {
        const std::string common =
            std::string(RAPIDPROTO_ARENAGEN_GOLDEN_DIR) + "/arena_modes.rp.common.hpp";
        // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
        if (std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr) {
            std::ofstream(common, std::ios::binary) << modes.common;
            WARN("regenerated arenagen golden: arena_modes.rp.common.hpp");
        } else {
            INFO(first_difference(read_file(common), modes.common));
            CHECK(modes.common == read_file(common));
        }
    }
    // --unknown-present: every message gets has_unknown_fields(), and the flag folds into the
    // profile identity (inline rp_modes_<id>). Byte-pinned so that fold cannot drift silently.
    check_golden("arena_unknown", generate_unknown_present_golden());
    check_golden("xref", generate_corpus("xref.proto"));
    check_golden("xref_prefixed/xref", generate_corpus("xref.proto", "rp"));
    check_golden("wire_all", generate(RAPIDPROTO_WIRE_FIXTURE_DIR, "wire_all.proto"));
    // Cross-file imports (distinct packages): the closure regenerates stably and (via the compile-smoke
    // #include above) compiles -- guarding cross-file message-field decoding through the decoder.
    const std::string imports = std::string(RAPIDPROTO_CORPUS_DIR) + "/imports";
    check_golden("dep", generate(imports, "dep.proto"));
    check_golden("pub", generate(imports, "pub.proto"));
    check_golden("forward", generate(imports, "forward.proto"));
    check_golden("main", generate(imports, "main.proto"));
    // Same package, two files in one TU (the compile-smoke #includes both): guards against the decoder
    // becoming a single per-package entity that redefines across files.
    check_golden("samepkg_b", generate(imports, "samepkg_b.proto"));
    check_golden("samepkg_a", generate(imports, "samepkg_a.proto"));
    // Weak import: arena filters `weak` like a standard import (parity with streamgen).
    check_golden("weakdep", generate(imports, "weakdep.proto"));
    check_golden("weakmain", generate(imports, "weakmain.proto"));
    // --namespace-prefix + imports: the prefixed cross-file closure regenerates stably and (via the
    // compile-smoke #include) compiles -- guarding prefixed cross-file emission, not just substring checks.
    check_golden("prefixed/main", generate(imports, "main.proto", "rp"));
    check_golden("prefixed/dep", generate(imports, "dep.proto", "rp"));
    // Package SHAPES no other entry has: every other corpus file declares a single-component package.
    // deep -> `namespace com::example::deep`, nopkg -> types at GLOBAL scope, and xpkg -> a cross-file
    // reference INTO a dotted package. A namespace derived from a type FQN rather than looked up
    // breaks exactly here, so these pin the emitted namespaces.
    const std::string nsedge = std::string(RAPIDPROTO_CORPUS_DIR) + "/nsedge";
    check_golden("deep", generate(nsedge, "deep.proto"));
    check_golden("nopkg", generate(nsedge, "nopkg.proto"));
    check_golden("xpkg", generate(nsedge, "xpkg.proto"));
}

// The --namespace-prefix nests every generated namespace under the prefix, so the arena types coexist
// with protoc's (and the streamgen) headers in one TU. xref_prefixed (also #included above) proves
// the prefixed output is valid C++ and is a distinct type from the unprefixed one.
TEST_CASE("arenagen: namespace prefix nests the generated namespace", "[arenagen]") {
    const std::string prefixed = generate_corpus("xref.proto", "rp");
    CHECK(prefixed.find("namespace rp::xr {") != std::string::npos);
    CHECK(prefixed.find("const ::rp::xr::B*") != std::string::npos);  // refs are prefixed
    const std::string plain = generate_corpus("xref.proto");
    CHECK(plain.find("namespace xr {") != std::string::npos);
    CHECK(plain.find("rp::xr") == std::string::npos);
    static_assert(!std::is_same_v<rp::xr::A, xr::A>);  // distinct, coexisting types

    // --namespace-prefix combined with imports: user types nest under rp::, but the cross-file
    // decoder call stays the absolute ::rapidproto::arena_detail::decode_into, so the prefix can't
    // break it (the forwarder lives in the runtime, not under the prefixed namespace).
    const std::string main_rp =
        generate(std::string(RAPIDPROTO_CORPUS_DIR) + "/imports", "main.proto", "rp");
    CHECK(main_rp.find("namespace rp::main {") != std::string::npos);
    CHECK(main_rp.find("const ::rp::dep::Dep*") !=
          std::string::npos);  // cross-file ref is prefixed
    CHECK(main_rp.find("::rapidproto::arena_detail::decode_into(out.m_d") != std::string::npos);
}

TEST_CASE("arenagen: a long sibling dependency chain is emitted in dependency order",
          "[arenagen]") {
    // Each M(k) holds a field of M(k-1).Inner: naming a type NESTED under a sibling requires that
    // sibling's definition first (a forward declaration is not enough), so the chain is one long
    // must-precede dependency path, whose length is unbounded in a protoc-valid schema (why
    // ordered_siblings walks it iteratively -- an overflow-provoking length is not reproducible
    // at test-friendly sizes, so this pins the ordering contract under a long chain).
    constexpr int kChainLen = 300;
    std::string schema = "syntax = \"proto3\";\npackage chain;\n";
    for (int i = kChainLen; i >= 2; --i) {
        schema += "message M" + std::to_string(i) + " { M" + std::to_string(i - 1) +
                  ".Inner f = 1; message Inner { int32 v = 1; } }\n";
    }
    schema += "message M1 { message Inner { int32 v = 1; } }\n";
    const test::TempDir dir("arenagen_chain");
    dir.write("chain.proto", schema);

    const std::string header = generate(dir.root(), "chain.proto");
    // Dependencies force definition order M1 < M2 < ... despite the reversed declaration order
    // ("class Mx {" is exact: the trailing " {" keeps M1 from matching M19 etc.; a missing class
    // fails loudly because npos < npos is false).
    for (int i = 2; i <= kChainLen; ++i) {
        CHECK(header.find("class M" + std::to_string(i - 1) + " {") <
              header.find("class M" + std::to_string(i) + " {"));
    }
}
