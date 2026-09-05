#pragma once

// Test-support: byte-compare EVERY `<name>.rp.common.hpp` under a golden directory against a fresh
// emit_common_header() of its entry. Shared by test_arenagen.cpp and test_dumpgen.cpp, whose model
// goldens byte-pin the decoders while the common header each of them #includes -- where every enum
// and the nesting mirror live -- would otherwise be compiled but compared by nothing.
//
// Directory-driven on purpose: an earlier version was a hand-maintained case list per suite, which
// then needed a completeness check policing the list -- two artifacts to keep in step per
// directory. The sweep derives each case from the golden's own path instead, following the regen
// scripts' layout conventions:
//
//   <stem>.rp.common.hpp                  default prefix; entry = <stem>.proto in the first of
//                                         `entry_dirs` that has it
//   prefixed/ , xref_prefixed/ <stem>...  --namespace-prefix pfx
//   unknown/ <stem>...                    --namespace-prefix unk (regen_arenagen_goldens.sh)
//
// A golden in a subdirectory outside those conventions, or whose entry no schema provides, fails
// loudly instead of being skipped.

#include <catch_amalgamated.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "golden_file.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

namespace rapidproto::test {

inline void check_all_common_goldens(const std::string& golden_dir,
                                     const std::vector<std::string>& entry_dirs) {
    int seen = 0;
    int models_seen = 0;
    const std::filesystem::path root{golden_dir};
    // Every arena/dump model golden in THIS directory must have its sibling common beside it
    // (test_streamgen.cpp's hand-maintained list covers the streaming dir the same way). This is
    // the "a golden must exist to be compared" direction a directory walk alone loses: a deleted
    // common simply stops being visited, where the model header it belonged to is still here to
    // miss it.
    for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(root)) {
        const std::string path = dir_entry.path().generic_string();
        constexpr std::string_view kHpp = ".rp.hpp";
        constexpr std::string_view kDump = ".rp.dump.hpp";
        std::size_t cut = 0;
        if (path.size() > kDump.size() &&
            path.compare(path.size() - kDump.size(), kDump.size(), kDump) == 0) {
            cut = kDump.size();
        } else if (path.size() > kHpp.size() &&
                   path.compare(path.size() - kHpp.size(), kHpp.size(), kHpp) == 0) {
            cut = kHpp.size();
        } else {
            continue;
        }
        const std::string sibling = path.substr(0, path.size() - cut) + ".rp.common.hpp";
        INFO(path << " has no sibling common golden -- deleted, or never regenerated");
        CHECK(std::filesystem::exists(sibling));
        ++models_seen;
    }
    // Anti-vacuity, same reasoning as the compare loop's floor below: a walk that matched no
    // model golden at all checked nothing.
    CHECK(models_seen >= 10);
    for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(root)) {
        const std::string rel = dir_entry.path().lexically_relative(root).generic_string();
        constexpr std::string_view kExt = ".rp.common.hpp";
        if (rel.size() <= kExt.size() ||
            rel.compare(rel.size() - kExt.size(), kExt.size(), kExt) != 0) {
            continue;
        }
        INFO("common golden: " << rel);
        const std::string name = rel.substr(0, rel.size() - kExt.size());
        const auto slash = name.rfind('/');
        const std::string subdir = slash == std::string::npos ? "" : name.substr(0, slash);
        const std::string stem = slash == std::string::npos ? name : name.substr(slash + 1);
        std::string prefix;
        if (subdir == "prefixed" || subdir == "xref_prefixed") {
            prefix = "pfx";
        } else if (subdir == "unknown") {
            prefix = "unk";
        } else if (!subdir.empty()) {
            FAIL("golden subdir '" << subdir << "' has no known prefix convention -- teach "
                                   << "common_golden_sweep.hpp about it");
        }
        std::string entry_dir;
        for (const std::string& dir : entry_dirs) {
            if (std::filesystem::exists(dir + "/" + stem + ".proto")) {
                entry_dir = dir;
                break;
            }
        }
        if (entry_dir.empty()) {
            FAIL("no entry schema '" << stem << ".proto' in any known fixture directory");
        }

        ResolverConfig config;
        config.include_paths = {entry_dir};
        auto resolved = resolve(entry_dir + "/" + stem + ".proto", config);
        REQUIRE(resolved.is_ok());
        ResolvedFileSet set = std::move(resolved).value();
        REQUIRE(analyze(set).is_ok());
        // The arena root regardless of which suite sweeps: the common header is model-independent
        // by construction, which test_common_header.cpp pins directly.
        const codegen::CppNameTable names = codegen::build_cpp_names(
            set.files.back(), set.files, codegen::effective_ns_prefix(prefix),
            std::string(codegen::kArenaRoot));
        const std::string actual = codegen::emit_common_header(set.files.back(), names);
        check_golden(golden_dir + "/" + rel, "common " + rel, actual);
        ++seen;
    }
    // Anti-vacuity: a moved or emptied golden directory must not read as "all compared".
    CHECK(seen >= 10);
}

}  // namespace rapidproto::test
