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

#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
// Checked-in generated headers (compile-smoke: they must be valid C++).
// IWYU pragma: begin_keep
#include "arenagen_golden/arena_layout.rp.hpp"
#include "arenagen_golden/arena_manyreq.rp.hpp"  // >64 required: multi-word rp_req
#include "arenagen_golden/arena_naming.rp.hpp"   // identifier dedup: must compile
#include "arenagen_golden/editions2023.rp.hpp"
#include "arenagen_golden/main.rp.hpp"  // cross-file imports: transitively pulls dep/forward/pub
#include "arenagen_golden/prefixed/main.rp.hpp"  // --namespace-prefix + imports (pulls prefixed dep/...)
#include "arenagen_golden/proto2.rp.hpp"
#include "arenagen_golden/proto3.rp.hpp"
#include "arenagen_golden/samepkg_a.rp.hpp"  // same-package multi-file (pulls samepkg_b): ODR guard
#include "arenagen_golden/weakmain.rp.hpp"  // weak import (pulls weakdep): filtered like a normal one
#include "arenagen_golden/wire_all.rp.hpp"  // group + packed (generated from the fixtures dir)
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
