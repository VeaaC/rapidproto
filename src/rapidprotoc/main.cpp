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
#include <fstream>
#include <ios>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>  // std::error_code: the non-throwing is_regular_file overload
#include <utility>
#include <vector>

#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
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
        "  --unknown-present        arena: reserve the \"unknown fields present\" bit on every"
        " message\n"
        "  --unknown=<msg>          arena: reserve that bit on one message (repeatable)\n"
        "  --field-modes=<file>     arena: a decode profile"
        " (`name|drop|raw|unknown-fields <name>` lines; repeatable)\n"
        "  --drop=<name>            arena: drop a field or type (no storage, no accessor)\n"
        "  --raw=<name>             arena: keep a message field's or type's payloads for deferred"
        " decodes\n"
        "  --namespace-prefix <ns>  dot-separated prefix prepended to every C++ namespace\n"
        "  --depfile <file>         write a Make/Ninja depfile covering every input .proto\n"
        "  --no-wellknown           don't load the bundled well-known-type definitions\n"
        "  -v, --verbose            log each written file\n"
        "  -h, --help               show this help\n"
        "  --version                print the version\n";
    bool arena = false;
    bool stream = false;
    std::vector<std::string> modes_files;
    rapidproto::arenagen::FieldModesSpec
        modes_spec;  // direct --drop/--raw/--unknown + file entries
    const auto parsed = rapidproto::cli::parse_args(argc, argv, usage, [&](std::string_view arg) {
        constexpr std::string_view kModesFile = "--field-modes=";
        constexpr std::string_view kDrop = "--drop=";
        constexpr std::string_view kRaw = "--raw=";
        constexpr std::string_view kUnknown = "--unknown=";
        if (arg == "--arena") {
            arena = true;
            return true;
        }
        if (arg == "--stream") {
            stream = true;
            return true;
        }
        if (arg == "--unknown-present") {
            modes_spec.unknown_all = true;  // the unknown bit on every message (folds into the id)
            return true;
        }
        if (arg.rfind(kModesFile, 0) == 0) {
            modes_files.emplace_back(arg.substr(kModesFile.size()));
            return true;
        }
        if (arg.rfind(kDrop, 0) == 0) {
            modes_spec.entries.push_back({rapidproto::arenagen::FieldMode::Drop,
                                          std::string(arg.substr(kDrop.size())), "--drop"});
            return true;
        }
        if (arg.rfind(kRaw, 0) == 0) {
            modes_spec.entries.push_back({rapidproto::arenagen::FieldMode::Raw,
                                          std::string(arg.substr(kRaw.size())), "--raw"});
            return true;
        }
        if (arg.rfind(kUnknown, 0) == 0) {
            modes_spec.unknowns.push_back({std::string(arg.substr(kUnknown.size())), "--unknown"});
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
    const bool modes_requested = !modes_files.empty() || !modes_spec.entries.empty() ||
                                 !modes_spec.unknowns.empty() || modes_spec.unknown_all;
    if (modes_requested && !arena) {
        std::cerr << "error: field modes (--field-modes/--drop/--raw/--unknown[-present]) apply to"
                     " the arena decoder; add --arena\n";
        return 2;
    }
    for (const std::string& file : modes_files) {
        // is_regular_file: ifstream opens a DIRECTORY successfully on Linux and reads nothing --
        // a typo'd path would silently decay into an empty (no-op) profile.
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            std::cerr << "error: field-modes file " << file << " is not a readable file\n";
            return 1;
        }
        const std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::cerr << "error: cannot read field-modes file " << file << '\n';
            return 1;
        }
        std::ostringstream text;
        text << in.rdbuf();
        if (auto r = rapidproto::arenagen::parse_modes_file(text.str(), file, modes_spec);
            r.is_err()) {
            std::cerr << "error: " << r.error().message << '\n';
            return 1;
        }
    }

    // The entries resolve as ONE batch: a union closure in which shared imports parse once, every
    // file generates once, and a field-modes profile resolves against every entry's symbols at
    // once -- so one profile can span schemas that live in different entry files.
    auto analyzed = rapidproto::cli::resolve_and_analyze(opts->entries, opts->config);
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
            set.files.front(), set.files, rapidproto::codegen::namespace_of(opts->namespace_prefix),
            "stream");
    }
    std::optional<rapidproto::arenagen::LayoutSet> layouts;
    rapidproto::arenagen::FieldModes modes;  // inactive unless a selection resolved
    if (arena) {
        if (modes_requested) {
            auto resolved = rapidproto::arenagen::resolve_field_modes(modes_spec, set, symbols);
            if (resolved.is_err()) {
                std::cerr << "error: " << resolved.error().message << '\n';
                return 1;
            }
            modes = std::move(resolved).value();
        }
        rapidproto::arenagen::LayoutOptions options;
        options.modes = &modes;
        layouts = rapidproto::arenagen::plan_layouts(set, symbols, options);
    }

    for (const rapidproto::FileNode& file : set.files) {
        // The shared common header (the schema's top-level enums) every selected decoder includes.
        if (!rapidproto::cli::write_shared_file(
                rapidproto::cli::header_path(opts->out_dir, file, ".rp.common.hpp"),
                rapidproto::codegen::emit_common_header(file, names), opts->verbose)) {
            return 1;
        }
        if (arena && !rapidproto::cli::write_header(opts->out_dir, file, ".rp.hpp",
                                                    rapidproto::arenagen::generate_header(
                                                        file, names, *layouts, symbols, &modes),
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
    std::vector<std::filesystem::path> targets;  // entry decoder headers: the depfile's targets
    std::vector<std::filesystem::path> prereqs;  // every input .proto (+ profiles): prerequisites
    if (!opts->depfile.empty() && !set.files.empty()) {
        // The depfile's targets are the ENTRIES' selected decoder headers (one batch = one rule
        // producing them all); imports' headers regenerate with them, so their staleness rides on
        // the entry targets, mirroring what the CMake helper declares as the command's OUTPUT.
        for (const std::string& entry : opts->entries) {
            const std::string name =
                rapidproto::canonical_entry_name(entry, opts->config.include_paths);
            const auto it = set.file_index.find(name);
            if (it == set.file_index.end()) {
                continue;  // unreachable: every entry resolves into the set
            }
            const rapidproto::FileNode& file = set.files[it->second];
            if (arena) {
                targets.push_back(rapidproto::cli::header_path(opts->out_dir, file, ".rp.hpp"));
            }
            if (stream) {
                targets.push_back(
                    rapidproto::cli::header_path(opts->out_dir, file, ".rp.stream.hpp"));
            }
        }
        prereqs = rapidproto::cli::disk_proto_paths(opts->entries, set, opts->config);
        // Editing a decode profile changes the generated shape, so profiles are prerequisites
        // exactly like the .proto inputs.
        prereqs.insert(prereqs.end(), modes_files.begin(), modes_files.end());
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
