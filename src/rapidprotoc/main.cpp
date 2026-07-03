// rapidprotoc: the rapidproto code-generator CLI. Resolves a .proto entry file and its imports, runs
// the semantic pipeline, and emits the selected decoder model(s) into the output directory:
//   --arena   an arena object-tree decoder   `<stem>.rp.hpp`         (the default if neither is given)
//   --stream  a streaming callback decoder    `<stem>.rp.stream.hpp`
// plus the shared common header `<stem>.rp.common.hpp` (the schema's top-level enums, one C++ type both
// models include) per file, plus a self-contained copy of each model's runtime. Parsing once and
// emitting both models from one name analysis is what lets the two decoders coexist in a single TU
// (arena at `pkg::Msg`, streaming at `pkg::stream::Msg`, the enum shared at `pkg::State`). A thin driver
// over the library; not linted. The shared flag parsing / resolve-analyze / file writing live in
// rapidproto/cli/driver.hpp.

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/runtime_embedded.hpp"
#include "rapidproto/cli/driver.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/runtime_embedded.hpp"
#include "rapidproto/streamgen/generator.hpp"

int main(int argc, char** argv) {
    const std::string usage =
        std::string("usage: ") + argv[0] + " [options] <entry.proto>...\n" +
        "  -I <dir>                 add an import search path (repeatable)\n"
        "  --out-dir <dir>          write the generated headers here (default: .)\n"
        "  --arena                  emit the arena object-tree decoder (<stem>.rp.hpp) [default]\n"
        "  --stream                 emit the streaming callback decoder (<stem>.rp.stream.hpp)\n"
        "  --unknown-present        arena: reserve a per-message \"unknown fields present\" bit\n"
        "  --namespace-prefix <ns>  dot-separated prefix prepended to every C++ namespace\n"
        "  --depfile <file>         write a Make/Ninja depfile covering the entry's imports\n"
        "  --no-wellknown           don't load the bundled well-known-type definitions\n"
        "  -v, --verbose            log each written file\n"
        "  -h, --help               show this help\n"
        "  --version                print the version\n";
    bool arena = false;
    bool stream = false;
    bool unknown_present = false;
    const auto parsed = rapidproto::cli::parse_args(argc, argv, usage, [&](std::string_view arg) {
        if (arg == "--arena") {
            arena = true;
            return true;
        }
        if (arg == "--stream") {
            stream = true;
            return true;
        }
        if (arg == "--unknown-present") {
            unknown_present = true;
            return true;
        }
        return false;
    });
    if (!parsed.options) {
        return parsed.exit_code;
    }
    const auto& opts = parsed.options;
    if (!arena && !stream) {
        arena = true;  // arena is the default model
    }
    if (!opts->depfile.empty() && opts->entries.size() > 1) {
        std::cerr << "error: --depfile expects a single entry (a depfile describes one rule)\n";
        return 2;
    }

    std::vector<std::filesystem::path>
        targets;  // the entry's selected decoder header(s): depfile targets
    std::vector<std::filesystem::path> prereqs;  // every input .proto: the depfile prerequisites
    for (const std::string& entry : opts->entries) {
        auto analyzed = rapidproto::cli::resolve_and_analyze(entry, opts->config);
        if (!analyzed) {
            return 1;
        }
        const rapidproto::ResolvedFileSet& set = analyzed->first;
        const rapidproto::SymbolTable& symbols = analyzed->second;

        // Build the name table(s) ONCE for the whole resolved set (identical for every file), then emit
        // per file. `names` has NO model namespace: arena types sit at pkg::Msg and enums at pkg::State
        // (the common header's home), so it drives both the arena decoder and the model-agnostic common.
        // `names_stream` nests messages under pkg::stream (enums stay shared); built only when needed.
        const rapidproto::codegen::CppNameTable names =
            set.files.empty() ? rapidproto::codegen::CppNameTable{}
                              : rapidproto::codegen::build_cpp_names(
                                    set.files.front(), set.files,
                                    rapidproto::codegen::namespace_of(opts->namespace_prefix));
        rapidproto::codegen::CppNameTable names_stream;
        if (stream && !set.files.empty()) {
            names_stream = rapidproto::codegen::build_cpp_names(
                set.files.front(), set.files,
                rapidproto::codegen::namespace_of(opts->namespace_prefix), "stream");
        }
        std::optional<rapidproto::arenagen::LayoutSet> layouts;
        if (arena) {
            rapidproto::arenagen::LayoutOptions options;
            options.unknown_present = unknown_present;
            layouts = rapidproto::arenagen::plan_layouts(set, symbols, options);
        }

        for (const rapidproto::FileNode& file : set.files) {
            // The shared common header (the schema's top-level enums) every selected decoder includes.
            if (!rapidproto::cli::write_shared_file(
                    rapidproto::cli::header_path(opts->out_dir, file, ".rp.common.hpp"),
                    rapidproto::codegen::emit_common_header(file, names), opts->verbose)) {
                return 1;
            }
            if (arena && !rapidproto::cli::write_header(
                             opts->out_dir, file, ".rp.hpp",
                             rapidproto::arenagen::generate_header(file, names, *layouts, symbols),
                             opts->verbose)) {
                return 1;
            }
            if (stream &&
                !rapidproto::cli::write_header(
                    opts->out_dir, file, ".rp.stream.hpp",
                    rapidproto::streamgen::generate_header(file, names_stream), opts->verbose)) {
                return 1;
            }
        }
        if (!opts->depfile.empty() && !set.files.empty()) {
            // The depfile's targets are the entry's own selected decoder header(s) (set.files.back());
            // re-running the CLI regenerates the whole closure, so their staleness gates the rebuild.
            // --depfile is rejected above for >1 entry, so this rule has the one entry's targets.
            if (arena) {
                targets.push_back(
                    rapidproto::cli::header_path(opts->out_dir, set.files.back(), ".rp.hpp"));
            }
            if (stream) {
                targets.push_back(rapidproto::cli::header_path(opts->out_dir, set.files.back(),
                                                               ".rp.stream.hpp"));
            }
            const std::vector<std::filesystem::path> deps =
                rapidproto::cli::disk_proto_paths(entry, set, opts->config);
            prereqs.insert(prereqs.end(), deps.begin(), deps.end());
        }
    }

    // Drop the self-contained runtime headers so the generated #includes resolve with no rapidproto
    // build-tree dependency. runtime.hpp serves both models; arena_runtime.hpp (which #includes
    // runtime.hpp) only the arena decoder.
    const std::filesystem::path dir = std::filesystem::path(opts->out_dir) / "rapidproto";
    if (!rapidproto::cli::write_shared_file(dir / "runtime.hpp",
                                            rapidproto::codegen::runtime_header(), opts->verbose)) {
        return 1;
    }
    if (arena && !rapidproto::cli::write_shared_file(dir / "arena_runtime.hpp",
                                                     rapidproto::arenagen::arena_runtime_header(),
                                                     opts->verbose)) {
        return 1;
    }

    if (!opts->depfile.empty() &&
        !rapidproto::cli::write_depfile(opts->depfile, targets, prereqs)) {
        return 1;
    }

    return 0;
}
