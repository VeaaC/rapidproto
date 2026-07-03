// Tests for the shared CLI driver's --depfile support (rapidproto/cli/driver.hpp): disk_proto_paths
// reports the on-disk .proto closure (entry + transitive imports) and excludes well-known types that
// come from the embedded definitions, and write_depfile emits a well-formed `outputs : prereqs` Make
// depfile. The CLIs' end-to-end depfile + incremental regeneration is exercised by the consumer
// example the gate builds.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "rapidproto/cli/driver.hpp"
#include "rapidproto/resolver.hpp"
#include "temp_dir.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

bool has_filename(const std::vector<std::filesystem::path>& paths, const std::string& name) {
    return std::any_of(paths.begin(), paths.end(), [&](const std::filesystem::path& p) {
        return p.filename().string() == name;
    });
}

bool mentions(const std::vector<std::filesystem::path>& paths, const std::string& needle) {
    return std::any_of(paths.begin(), paths.end(), [&](const std::filesystem::path& p) {
        return p.generic_string().find(needle) != std::string::npos;
    });
}

std::vector<std::filesystem::path> deps_of(const std::string& dir, const std::string& entry) {
    ResolverConfig config;
    config.include_paths = {dir};
    const std::string path = dir + "/" + entry;
    auto resolved = resolve(path, config);
    REQUIRE(resolved.is_ok());
    const ResolvedFileSet set = std::move(resolved).value();
    return cli::disk_proto_paths(path, set, config);
}

std::string read_text(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("driver: disk_proto_paths lists the entry + transitive imports", "[cli]") {
    const std::string imports = std::string(RAPIDPROTO_CORPUS_DIR) + "/imports";
    const auto deps = deps_of(imports, "main.proto");
    // main -> {dep, forward -> pub}: the whole on-disk closure, so touching any of them re-triggers
    // codegen (the point of the depfile -- a plain output-only DEPENDS would miss the imports).
    CHECK(has_filename(deps, "main.proto"));
    CHECK(has_filename(deps, "dep.proto"));
    CHECK(has_filename(deps, "forward.proto"));
    CHECK(has_filename(deps, "pub.proto"));
}

TEST_CASE("driver: disk_proto_paths excludes embedded well-known types", "[cli]") {
    const auto deps = deps_of(RAPIDPROTO_CORPUS_DIR, "usewkt.proto");
    CHECK(has_filename(deps, "usewkt.proto"));
    // The WKT resolves from the embedded definitions, not a file on an include path, so it is not a
    // build dependency and must not appear -- else CMake would stat a path that does not exist.
    CHECK_FALSE(mentions(deps, "google/protobuf"));
}

TEST_CASE("driver: write_depfile emits `outputs : prereqs`", "[cli]") {
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "rp_cli_driver_test";
    std::filesystem::create_directories(tmp);
    const std::filesystem::path depfile = tmp / "out.d";
    CHECK(cli::write_depfile(depfile, {tmp / "a.rp.stream.hpp", tmp / "b.rp.stream.hpp"},
                             {tmp / "a.proto", tmp / "b.proto"}));
    const std::string text = read_text(depfile);
    INFO("depfile: " << text);
    const auto colon = text.find(':');
    REQUIRE(colon != std::string::npos);
    // Generated headers are targets (before the colon); the .proto inputs are prerequisites (after it).
    CHECK(text.find("a.rp.stream.hpp") < colon);
    CHECK(text.find("b.rp.stream.hpp") < colon);
    CHECK(text.find("a.proto") > colon);
    CHECK(text.find("b.proto") > colon);
    CHECK(text.back() == '\n');
}

TEST_CASE("driver: write_file reports an unwritable path instead of crashing", "[cli]") {
    const test::TempDir tmp("cli_write_fail");
    tmp.write("blocker", "not a directory");
    // The target's parent "directory" is a regular file: create_directories must fail cleanly
    // (this used to escape as an uncaught std::filesystem_error and abort the CLI).
    const auto under_file =
        cli::write_file(std::filesystem::path(tmp.path("blocker")) / "sub" / "out.hpp", "content");
    CHECK_FALSE(under_file.has_value());
    // The target path itself is an existing directory: the stream open fails, and it must be
    // REPORTED (this used to print "wrote ..." and succeed with nothing written).
    std::filesystem::create_directories(tmp.path("adir"));
    CHECK_FALSE(cli::write_file(tmp.path("adir"), "content").has_value());
    // Success still returns the path and writes the content.
    const auto ok = cli::write_file(tmp.path("ok/out.hpp"), "content");
    REQUIRE(ok.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(read_text(*ok) == "content");
}

TEST_CASE("driver: write_depfile reports an unwritable path", "[cli]") {
    const test::TempDir tmp("cli_depfile_fail");
    tmp.write("blocker", "not a directory");
    CHECK_FALSE(cli::write_depfile(std::filesystem::path(tmp.path("blocker")) / "d" / "out.d",
                                   {tmp.path("a.hpp")}, {tmp.path("a.proto")}));
}
