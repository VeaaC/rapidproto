// rapidprotoc: the rapidproto code-generator CLI. Resolves a .proto entry file and its imports, runs
// the semantic pipeline, and emits the selected decoder model(s) into the output directory:
//   --arena   an arena object-tree decoder   `<stem>.rp.hpp`         (the default if neither is given)
//   --stream  a streaming callback decoder    `<stem>.rp.stream.hpp`
// plus the shared common header `<stem>.rp.common.hpp` (the schema's enums, one C++ type both
// models include) per file, plus a self-contained copy of each model's runtime. Parsing once and
// emitting both models from one name analysis is what lets the two decoders coexist in a single TU
// (arena at `rp::arena::pkg::Msg`, streaming at `rp::stream::pkg::Msg`, enums shared at
// `rp::common::pkg::State`). A thin driver
// over the library; not linted. The shared flag parsing / resolve-analyze / file writing live in
// rapidproto/cli/driver.hpp.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <map>
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
#include "rapidproto/dumpgen/generator.hpp"
#include "rapidproto/dumpgen/runtime_embedded.hpp"
#include "rapidproto/streamgen/generator.hpp"

int main(int argc, char** argv) {
    const std::string usage =
        std::string("usage: ") + argv[0] + " [options] <entry.proto>...\n" +
        "  -I <dir>                 add an import search path (repeatable)\n"
        "  --out-dir <dir>          write the generated headers here (default: .)\n"
        "  --arena                  emit the arena object-tree decoder (<stem>.rp.hpp) [default]\n"
        "  --stream                 emit the streaming callback decoder (<stem>.rp.stream.hpp)\n"
        "  --dump                   emit a JSON-like debug dumper (<stem>.rp.dump.hpp; implies"
        " --arena)\n"
        "  --unknown-present        arena: reserve the \"unknown fields present\" bit on every"
        " message\n"
        "  --unknown=<msg>          arena: reserve that bit on one message (repeatable)\n"
        "  --field-modes=<file>     arena: a decode profile"
        " (`name|drop|raw|unknown-fields <name>` lines; repeatable)\n"
        "  --drop=<name>            arena: drop a field or type (no storage, no accessor)\n"
        "  --raw=<name>             arena: keep a message field's or type's payloads for deferred"
        " decodes\n"
        "  --namespace-prefix <ns>  root namespace for generated code (dot-separated; "
        "default: rp; cannot be empty)\n"
        "  --depfile <file>         write a Make/Ninja depfile covering every input .proto\n"
        "  --list-outputs           dry run: print every path generation would write, relative\n"
        "                           to --out-dir, one per line; write nothing\n"
        "  --list-inputs            dry run: print the on-disk .proto closure (absolute paths;\n"
        "                           embedded well-known types excluded); write nothing\n"
        "  --no-wellknown           don't load the bundled well-known-type definitions\n"
        "  -v, --verbose            log each written file\n"
        "  -h, --help               show this help\n"
        "  --version                print the version\n";
    bool arena = false;
    bool stream = false;
    bool dump = false;  // emit <stem>.rp.dump.hpp alongside the arena header
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
        if (arg == "--dump") {  // debug dumper (implies --arena; needs the arena header)
            dump = true;
            arena = true;
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

    // Non-fatal analysis diagnostics. Generation continues -- these say the OUTPUT is narrower than
    // the schema, not that the schema is bad -- so they go to stderr and never change the exit code.
    for (const std::string& warning : symbols.warnings) {
        std::cerr << warning << '\n';
    }

    // Three things are refused before ANY file is written, so a refused run leaves nothing behind.
    // Each is a way two schemas silently lose work rather than a malformed input.
    //
    // Two distinct schemas must not resolve to one canonical name. They are deduplicated by that
    // name -- the mechanism that makes a shared import parse once -- so when two different files
    // collide the second is silently DROPPED, and the user gets a decoder for one of the two
    // schemas they asked for. Entries are absolute here, so weakly_canonical separates "the same
    // file twice" (fine, and deduplicated on purpose) from "two files, one name".
    std::map<std::string, std::string> by_canonical_name;
    for (const std::string& entry : opts->entries) {
        const std::string name =
            rapidproto::canonical_entry_name(entry, opts->config.include_paths);
        std::error_code ec;
        const std::string disk = std::filesystem::weakly_canonical(entry, ec).string();
        const auto [it, fresh] = by_canonical_name.emplace(name, ec ? entry : disk);
        if (!fresh && it->second != (ec ? entry : disk)) {
            std::cerr << "error: '" << it->second << "' and '" << (ec ? entry : disk)
                      << "' are both '" << name << "' relative to the include paths\n"
                      << "  only one of them would be generated; give them include paths that "
                         "keep their names distinct\n";
            return 1;
        }
    }

    for (const rapidproto::FileNode& file : set.files) {
        // Only an IMPORT can still land outside the out-dir: entries are absolute by now, so their
        // fallback name is one header_path reduces to a basename. An import's name is the import
        // string itself -- canonical_import_path normalizes it but never rebases it on an include
        // dir -- so a `..` there escapes, and no -I can move it.
        if (rapidproto::cli::header_escapes_out_dir(file)) {
            // Name the file that has to be edited, not just the one that cannot be placed: the
            // offending path is an import STATEMENT, which lives in whoever imports it.
            std::string importer;
            for (const rapidproto::FileNode& other : set.files) {
                for (const rapidproto::ImportNode& import : other.imports) {
                    if (rapidproto::canonical_import_path(import.path) == file.filename) {
                        importer = other.filename;
                        break;
                    }
                }
                if (!importer.empty()) {
                    break;
                }
            }
            std::cerr << "error: '" << file.filename << "' would write outside --out-dir ("
                      << opts->out_dir << ")\n";
            if (importer.empty()) {
                std::cerr << "  rewrite it relative to an -I directory (protoc rejects '..' in an "
                             "import too)\n";
            } else {
                std::cerr << "  it is imported by '" << importer
                          << "'; rewrite that import relative to an -I directory (protoc rejects "
                             "'..' in an import too)\n";
            }
            return 1;
        }
    }

    // ...and two schemas must not generate the SAME header. Distinct canonical names can still
    // share one output stem, because a name that is not import-relative reduces to its basename:
    // `/a/x.proto` and `/b/x.proto` both wrote x.rp.hpp, the second over the first, exit 0.
    std::map<std::string, std::string> by_output;
    for (const rapidproto::FileNode& file : set.files) {
        const std::string stem = rapidproto::cli::header_path(opts->out_dir, file, "").string();
        const auto [it, fresh] = by_output.emplace(stem, file.filename);
        if (!fresh) {
            std::cerr << "error: '" << it->second << "' and '" << file.filename
                      << "' both generate " << stem << ".*\n"
                      << "  one would overwrite the other; pass an -I they share, so each keeps a "
                         "distinct path relative to it\n";
            return 1;
        }
    }

    // Build the name table(s) ONCE for the whole resolved set (identical for every file), then emit
    // per file. `names` carries the ARENA root, so it drives both the arena decoder and the
    // model-agnostic common header (enums sit under their own root either way). `names_stream`
    // carries the stream root; built only when needed.
    const rapidproto::codegen::CppNameTable names =
        set.files.empty() ? rapidproto::codegen::CppNameTable{}
                          : rapidproto::codegen::build_cpp_names(
                                set.files.front(), set.files,
                                rapidproto::codegen::effective_ns_prefix(opts->namespace_prefix),
                                std::string(rapidproto::codegen::kArenaRoot));
    rapidproto::codegen::CppNameTable names_stream;
    if (stream && !set.files.empty()) {
        names_stream = rapidproto::codegen::build_cpp_names(
            set.files.front(), set.files,
            rapidproto::codegen::effective_ns_prefix(opts->namespace_prefix),
            std::string(rapidproto::codegen::kStreamRoot));
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

    // ONE enumeration of everything this invocation writes -- see cli::plan_outputs for the
    // ordering contract it carries (the CMake helper anchors on the first path; the depfile's
    // targets are derived from it below).
    const std::vector<rapidproto::cli::PlannedOutput> plan = rapidproto::cli::plan_outputs(
        *opts, set, {/*arena=*/arena, /*stream=*/stream, /*dump=*/dump});

    // The dry runs exit here: everything above ran (resolve, analyze, the out-dir and collision
    // refusals, field-modes resolution), so a schema that cannot generate fails a listing the
    // same way -- which is the point, a build system learns of the error at configure time.
    if (opts->list_outputs) {
        for (const rapidproto::cli::PlannedOutput& out : plan) {
            std::cout << out.rel.generic_string() << '\n';
        }
        return 0;
    }
    if (opts->list_inputs) {
        std::vector<std::string> inputs;
        for (const std::filesystem::path& in :
             rapidproto::cli::disk_proto_paths(opts->entries, set, opts->config)) {
            // Absolutized: an import resolved through a relative -I comes back relative to the
            // CWD, and a build system consuming this list (CMAKE_CONFIGURE_DEPENDS) would anchor
            // it somewhere else. Deduplicated after absolutizing, the same way the depfile dedups
            // its prerequisites: one file reached under two spellings is one input.
            inputs.push_back(std::filesystem::absolute(in).lexically_normal().generic_string());
        }
        std::sort(inputs.begin(), inputs.end());
        inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
        for (const std::string& in : inputs) {
            std::cout << in << '\n';
        }
        return 0;
    }

    for (const rapidproto::cli::PlannedOutput& out : plan) {
        const std::filesystem::path path = std::filesystem::path(opts->out_dir) / out.rel;
        bool ok = true;
        switch (out.kind) {
            case rapidproto::cli::OutputKind::Common:
                // The shared common header (the schema's enums, nested ones via the mirror) every
                // selected decoder includes.
                ok = rapidproto::cli::write_shared_file(
                         path, rapidproto::codegen::emit_common_header(*out.file, names),
                         opts->verbose)
                         .has_value();
                break;
            case rapidproto::cli::OutputKind::Arena:
                ok = rapidproto::cli::write_file(path,
                                                 rapidproto::arenagen::generate_header(
                                                     *out.file, names, *layouts, symbols, &modes),
                                                 opts->verbose)
                         .has_value();
                break;
            case rapidproto::cli::OutputKind::Stream:
                ok = rapidproto::cli::write_file(
                         path, rapidproto::streamgen::generate_header(*out.file, names_stream),
                         opts->verbose)
                         .has_value();
                break;
            case rapidproto::cli::OutputKind::Dump:
                // --dump implies --arena, so `layouts` is always engaged here.
                ok = rapidproto::cli::write_file(
                         path, rapidproto::dumpgen::generate_header(*out.file, names, *layouts),
                         opts->verbose)
                         .has_value();
                break;
            // The self-contained runtime copies, so the generated #includes resolve with no
            // rapidproto build-tree dependency. runtime.hpp serves both models;
            // arena_runtime.hpp (which #includes runtime.hpp) only the arena decoder;
            // dump_runtime.hpp the debug dumper's escaper/hex/Writer support.
            case rapidproto::cli::OutputKind::Runtime:
                ok = rapidproto::cli::write_shared_file(path, rapidproto::codegen::runtime_header(),
                                                        opts->verbose)
                         .has_value();
                break;
            case rapidproto::cli::OutputKind::ArenaRuntime:
                ok = rapidproto::cli::write_shared_file(
                         path, rapidproto::arenagen::arena_runtime_header(), opts->verbose)
                         .has_value();
                break;
            case rapidproto::cli::OutputKind::DumpRuntime:
                ok = rapidproto::cli::write_shared_file(
                         path, rapidproto::dumpgen::dump_runtime_header(), opts->verbose)
                         .has_value();
                break;
        }
        if (!ok) {
            return 1;
        }
    }
    std::vector<std::filesystem::path> targets;  // entry decoder headers: the depfile's targets
    std::vector<std::filesystem::path> prereqs;  // every input .proto (+ profiles): prerequisites
    if (!opts->depfile.empty() && !set.files.empty()) {
        // The depfile's targets are the ENTRIES' selected decoder headers (one batch = one rule
        // producing them all); imports' headers regenerate with them, so their staleness rides on
        // the entry targets, mirroring what the CMake helper declares as the command's OUTPUT.
        // DERIVED from the plan rather than re-enumerated, so the depfile's first target is the
        // plan's first path by construction -- the anchor contract plan_outputs documents.
        for (const rapidproto::cli::PlannedOutput& out : plan) {
            // POSITIVE filter: only the decoder kinds may ever be depfile targets, so a future
            // per-file kind added to plan_outputs stays out until someone decides otherwise.
            const bool decoder = out.kind == rapidproto::cli::OutputKind::Arena ||
                                 out.kind == rapidproto::cli::OutputKind::Stream ||
                                 out.kind == rapidproto::cli::OutputKind::Dump;
            if (out.entry && decoder) {
                targets.push_back(std::filesystem::path(opts->out_dir) / out.rel);
            }
        }
        prereqs = rapidproto::cli::disk_proto_paths(opts->entries, set, opts->config);
        // Editing a decode profile changes the generated shape, so profiles are prerequisites
        // exactly like the .proto inputs.
        prereqs.insert(prereqs.end(), modes_files.begin(), modes_files.end());
    }

    if (!opts->depfile.empty() &&
        !rapidproto::cli::write_depfile(opts->depfile, targets, prereqs)) {
        return 1;
    }

    return 0;
}
