// Tests for the shared common header (codegen::emit_common_header) and the name table's
// model_namespace hook (WS2 of the coexistence work). The common header holds a schema's top-level
// enums, shared by both decoder models; model_namespace nests top-level messages (the decoders) so the
// two coexist in one TU. The decoders' use of these is covered later (the arena/stream golden suites +
// the same-TU consumer test); here we test the emitter and the hook in isolation.

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Resolve + analyze `dir/entry` and its import closure; the entry is set.files.back().
ResolvedFileSet resolve_set(const std::string& dir, const std::string& entry) {
    ResolverConfig config;
    config.include_paths = {dir};
    auto resolved = resolve(dir + "/" + entry, config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    REQUIRE(analyze(set).is_ok());
    return set;
}

std::string common_header(const std::string& dir, const std::string& entry) {
    const ResolvedFileSet set = resolve_set(dir, entry);
    const codegen::CppNameTable names =
        codegen::build_cpp_names(set.files.front(), set.files, std::string{});
    return codegen::emit_common_header(set.files.back(), names);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): golden name vs content, distinct roles
void check_golden(const std::string& name, const std::string& actual) {
    const std::string golden =
        std::string(RAPIDPROTO_COMMON_GOLDEN_DIR) + "/" + name + ".rp.common.hpp";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    if (std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr) {
        std::ofstream(golden, std::ios::binary) << actual;
        WARN("regenerated common golden: " << name);
        return;
    }
    INFO("golden: " << name);
    CHECK(actual == read_file(golden));
}

}  // namespace

TEST_CASE("common-header: golden output (enums + import includes)", "[common]") {
    const std::string corpus = RAPIDPROTO_CORPUS_DIR;
    check_golden("proto2", common_header(corpus, "proto2.proto"));  // top-level enum (proto2)
    check_golden("proto3", common_header(corpus, "proto3.proto"));  // top-level enum (proto3)
    // editions2023 has a top-level enum AND a nested one: only the top-level enum is in the common
    // header (the nested one rides with its message in each decoder).
    check_golden("editions2023", common_header(corpus, "editions2023.proto"));
    check_golden("naming",
                 common_header(corpus, "naming.proto"));  // enum-value dedup in the common hdr
    // Cross-file: main imports dep/forward, so its common header includes their common headers; dep is
    // an imported file's own common header (its top-level enum is shared across the closure).
    check_golden("main", common_header(corpus + "/imports", "main.proto"));
    check_golden("dep", common_header(corpus + "/imports", "dep.proto"));
}

TEST_CASE("common-header: model_namespace nests messages, not enums", "[common]") {
    const ResolvedFileSet set = resolve_set(RAPIDPROTO_CORPUS_DIR, "proto3.proto");
    const FileNode& file = set.files.back();
    REQUIRE_FALSE(file.messages.empty());
    REQUIRE_FALSE(file.enums.empty());
    const std::string msg_fqn = file.messages.front().fqn;
    const std::string enum_fqn = file.enums.front().fqn;

    // model_namespace = "stream": the top-level message nests under ::stream::; the enum does not.
    const codegen::CppNameTable nested =
        codegen::build_cpp_names(set.files.front(), set.files, std::string{}, "stream");
    CHECK(codegen::cpp_type_name(nested, msg_fqn).find("::stream::") != std::string::npos);
    CHECK(codegen::cpp_type_name(nested, enum_fqn).find("::stream::") == std::string::npos);

    // Default (empty model_namespace): the message stays at package scope (today's arena layout), and
    // the enum's absolute name is identical either way (enums are never model-nested).
    const codegen::CppNameTable plain =
        codegen::build_cpp_names(set.files.front(), set.files, std::string{});
    CHECK(codegen::cpp_type_name(plain, msg_fqn).find("::stream::") == std::string::npos);
    CHECK(codegen::cpp_type_name(nested, enum_fqn) == codegen::cpp_type_name(plain, enum_fqn));

    // --namespace-prefix nests INSIDE the prefix: ::rp::p3::stream::Msg; the enum stays ::rp::p3::State.
    const codegen::CppNameTable prefixed = codegen::build_cpp_names(
        set.files.front(), set.files, codegen::namespace_of("rp"), "stream");
    CHECK(codegen::cpp_type_name(prefixed, msg_fqn).find("::rp::p3::stream::") !=
          std::string::npos);
    CHECK(codegen::cpp_type_name(prefixed, enum_fqn).find("::rp::p3::") != std::string::npos);
    CHECK(codegen::cpp_type_name(prefixed, enum_fqn).find("::stream::") == std::string::npos);
}

TEST_CASE("common-header: model_namespace nests every message (incl. imported), never an enum",
          "[common]") {
    // The load-bearing coexistence invariant, across a cross-file closure (main imports dep/forward):
    // with "stream", EVERY top-level message -- including imported ones -- nests under ::stream::, and
    // NO top-level enum does, so the two models share one enum type per schema.
    const ResolvedFileSet set =
        resolve_set(std::string(RAPIDPROTO_CORPUS_DIR) + "/imports", "main.proto");
    const codegen::CppNameTable names =
        codegen::build_cpp_names(set.files.front(), set.files, std::string{}, "stream");
    for (const FileNode& file : set.files) {
        for (const auto& message : file.messages) {
            INFO("message " << message.fqn);
            CHECK(codegen::cpp_type_name(names, message.fqn).find("::stream::") !=
                  std::string::npos);
        }
        for (const auto& node : file.enums) {
            INFO("top-level enum " << node.fqn);
            CHECK(codegen::cpp_type_name(names, node.fqn).find("::stream::") == std::string::npos);
        }
    }
}

TEST_CASE("common-header: a nested enum rides with its message under model_namespace", "[common]") {
    // A NESTED enum is not shared (not in the common header) -- it stays inside its message, so under
    // "stream" it nests too (::pkg::stream::Msg::Inner), distinct from the arena copy.
    const ResolvedFileSet set = resolve_set(RAPIDPROTO_CORPUS_DIR, "editions2023.proto");
    const codegen::CppNameTable names =
        codegen::build_cpp_names(set.files.front(), set.files, std::string{}, "stream");
    bool saw_nested_enum = false;
    for (const FileNode& file : set.files) {
        for (const auto& message : file.messages) {
            for (const auto& nested_enum : message.enums) {
                INFO("nested enum " << nested_enum.fqn);
                CHECK(codegen::cpp_type_name(names, nested_enum.fqn).find("::stream::") !=
                      std::string::npos);
                saw_nested_enum = true;
            }
        }
    }
    CHECK(
        saw_nested_enum);  // editions2023 must contain a nested enum for this test to mean anything
}
