// Tests for the shared common header (codegen::emit_common_header) and the name table's
// model_namespace hook. The common header holds a schema's enums -- top-level ones directly, nested
// ones through the namespace mirror -- shared by both decoder models under `<prefix>::common`;
// model_namespace is the ROOT each decoder's messages sit under (`<prefix>::arena` /
// `<prefix>::stream`), which is what lets the two coexist in one TU. The decoders' use of these is covered later (the arena/stream golden suites +
// the same-TU consumer test); here we test the emitter and the hook in isolation.

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kArenaRoot));
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

TEST_CASE("common-header: identical whichever model asked for it", "[common]") {
    // The header both decoders include must not depend on which one triggered generation. Emitting
    // it per model and comparing the TEXT is what pins that: the enum-name checks elsewhere in this
    // file compare names the table resolves, and would still pass if the header's includes, its
    // namespace, or the mirror's shape differed between the two runs.
    //
    // A schema with both a top-level enum and a nested one (the mirror), plus imports, so the
    // comparison covers every part of the header rather than a bare enum block.
    for (const char* entry : {"editions2023.proto", "naming.proto", "proto2.proto"}) {
        INFO("schema " << entry);
        const ResolvedFileSet set = resolve_set(RAPIDPROTO_CORPUS_DIR, entry);
        const std::string prefix = codegen::effective_ns_prefix({});
        const std::string from_arena = codegen::emit_common_header(
            set.files.back(), codegen::build_cpp_names(set.files.back(), set.files, prefix,
                                                       std::string(codegen::kArenaRoot)));
        const std::string from_stream = codegen::emit_common_header(
            set.files.back(), codegen::build_cpp_names(set.files.back(), set.files, prefix,
                                                       std::string(codegen::kStreamRoot)));
        CHECK(from_arena == from_stream);
        // Not vacuous: two empty strings compare equal too. The header must carry the shared root
        // and NEITHER model root -- text equality alone would still hold if both runs emitted the
        // model they were asked for and the comparison were made against the wrong pair.
        CHECK(from_arena.find("namespace rp::common") != std::string::npos);
        CHECK(from_arena.find("rp::arena") == std::string::npos);
        CHECK(from_arena.find("rp::stream") == std::string::npos);
    }
}

TEST_CASE("naming: same-package files share one dedup scope, and real names win", "[common]") {
    // escdedup_a declares `enum decode`, which is reserved (the decoders expose a static decode()),
    // so it sanitizes onto `decode_` -- the name escdedup_b really gives a message. Deduping per
    // file gave both `rp::arena::escdedup::decode_`, and any TU including the two headers stopped compiling.
    const ResolvedFileSet set =
        resolve_set(std::string(RAPIDPROTO_CORPUS_DIR) + "/imports", "escdedup_a.proto");
    const codegen::CppNameTable names =
        codegen::build_cpp_names(set.files.back(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kArenaRoot));
    const std::string enum_name = codegen::cpp_type_name(names, ".escdedup.decode");
    const std::string message_name = codegen::cpp_type_name(names, ".escdedup.decode_");
    INFO("enum -> " << enum_name << ", message -> " << message_name);
    CHECK(enum_name != message_name);
    // The literal identifier keeps its spelling; the escape is what moves.
    CHECK(message_name == "::rp::arena::escdedup::decode_");
    CHECK(enum_name == "::rp::common::escdedup::decode__");

    // ...and that does not depend on the order the files are indexed in. Reversing the set used to
    // hand the contested id to whichever file came first, so one schema generated two different
    // sets of C++ names.
    std::vector<FileNode> reversed(set.files.rbegin(), set.files.rend());
    const codegen::CppNameTable flipped =
        codegen::build_cpp_names(reversed.front(), reversed, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kArenaRoot));
    CHECK(codegen::cpp_type_name(flipped, ".escdedup.decode") == enum_name);
    CHECK(codegen::cpp_type_name(flipped, ".escdedup.decode_") == message_name);

    // ...nor on the model. The dedup scope is keyed on the PACKAGE namespace precisely so the two
    // models agree here: this enum is emitted once, into the shared common header, so an id that
    // differed between the arena and streaming runs would leave that header contradicting the
    // streaming decoder's references to it.
    const codegen::CppNameTable streamed =
        codegen::build_cpp_names(set.files.back(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kStreamRoot));
    CHECK(codegen::cpp_type_name(streamed, ".escdedup.decode") == enum_name);
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
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kStreamRoot));
    CHECK(codegen::cpp_type_name(nested, msg_fqn).find("::stream::") != std::string::npos);
    CHECK(codegen::cpp_type_name(nested, enum_fqn).find("::stream::") == std::string::npos);

    // The arena root: messages sit under `<prefix>::arena::<pkg>` while the enum keeps its own
    // `<prefix>::common::<pkg>` home, identical from either model -- the coexistence invariant.
    const codegen::CppNameTable plain =
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kArenaRoot));
    CHECK(codegen::cpp_type_name(plain, msg_fqn).find("::arena::") != std::string::npos);
    CHECK(codegen::cpp_type_name(nested, enum_fqn) == codegen::cpp_type_name(plain, enum_fqn));

    // --namespace-prefix wraps the ROOTS: ::rp::stream::p3::Msg, and the enum keeps its own root
    // at ::rp::common::p3::State -- model-independent, which is what lets both models alias one type.
    const codegen::CppNameTable prefixed =
        codegen::build_cpp_names(set.files.front(), set.files, codegen::namespace_of("rp"),
                                 std::string(codegen::kStreamRoot));
    CHECK(codegen::cpp_type_name(prefixed, msg_fqn).find("::rp::stream::p3::") !=
          std::string::npos);
    CHECK(codegen::cpp_type_name(prefixed, enum_fqn).find("::rp::common::p3::") !=
          std::string::npos);
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
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kStreamRoot));
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat sweep over every nested enum
TEST_CASE("common-header: a nested enum is shared, mirrored under the common root", "[common]") {
    // A nested enum is DEFINED once in the common header's namespace mirror and aliased into each
    // model, so its absolute name carries the common root and is byte-identical whichever model asked
    // -- the property that makes one enum usable across both decoders.
    const ResolvedFileSet set = resolve_set(RAPIDPROTO_CORPUS_DIR, "editions2023.proto");
    const codegen::CppNameTable stream_names =
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kStreamRoot));
    const codegen::CppNameTable arena_names =
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kArenaRoot));
    bool saw_nested_enum = false;
    for (const FileNode& file : set.files) {
        for (const auto& message : file.messages) {
            for (const auto& nested_enum : message.enums) {
                INFO("nested enum " << nested_enum.fqn);
                const std::string from_stream =
                    codegen::cpp_type_name(stream_names, nested_enum.fqn);
                CHECK(from_stream.find("::common::") != std::string::npos);
                CHECK(from_stream.find("::stream::") == std::string::npos);
                CHECK(from_stream == codegen::cpp_type_name(arena_names, nested_enum.fqn));
                saw_nested_enum = true;
            }
        }
    }
    CHECK(
        saw_nested_enum);  // editions2023 must contain a nested enum for this test to mean anything
}
