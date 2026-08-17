// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// CLI helpers for rapidprotoc: shared flag parsing, the resolve -> analyze pipeline, and writing the
// generated headers (and depfile) into the out-dir. The model-specific parts (which decoder text
// to emit, which runtime header(s) to drop) live in the CLI main. Header-only, like the main that
// includes it.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"
#include "rapidproto/version.hpp"

namespace rapidproto::cli {

// A --namespace-prefix is a NON-EMPTY dot-separated list of C++ identifiers
// (`[A-Za-z_][A-Za-z0-9_]*`). This catches CLI typos (e.g. `rp:`) up front instead of emitting
// uncompilable generated code.
//
// Empty is rejected rather than meaning "no prefix". The generated models sit under per-model root
// segments (`arena`, `stream`, `enums`), so an empty prefix would put those three ordinary words at
// GLOBAL scope, where a consumer's own `class arena` or `namespace stream` collides with them. The
// prefix is what keeps them in one namespace the consumer can rename; `rp` is only its default.
inline bool valid_namespace_prefix(std::string_view p) {
    if (p.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = p.find('.', start);
        const std::string_view comp =
            p.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (comp.empty() ||
            (std::isalpha(static_cast<unsigned char>(comp[0])) == 0 && comp[0] != '_')) {
            return false;
        }
        for (const char ch : comp) {
            if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_') {
                return false;
            }
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        start = dot + 1;
    }
}

// The flags shared by every generator CLI.
struct Options {
    ResolverConfig config;      // -I include paths, --no-wellknown
    std::string out_dir = ".";  // --out-dir
    // --namespace-prefix (dotted, prepended to each C++ namespace). See valid_namespace_prefix for
    // why it cannot be emptied.
    std::string namespace_prefix{codegen::kDefaultNsPrefix};
    std::string depfile;   // --depfile (emit a Make/Ninja depfile for incremental codegen)
    bool verbose = false;  // --verbose / -v: log each written file
    std::vector<std::string> entries;
};

// parse_args' result: `options` is engaged on a successful parse; otherwise the caller exits with
// `exit_code` (0 after --help/--version served an informational request, 2 on a usage error --
// everything needed was already printed).
struct ParseResult {
    std::optional<Options> options;
    int exit_code = 0;
};

// Parse argv into Options. `extra` is invoked for an argument none of the shared flags matched
// (a model-specific flag, e.g. the arena model's --unknown-present); it returns true if it consumed
// the argument. An unconsumed argument starting with '-' is an unknown flag (usage error), so a
// typo can't be silently treated as an entry file; anything else is a positional entry file.
// --help/-h prints `usage` to stdout and --version prints the tool version; both yield exit 0.
// Usage errors (a flag missing its value, no entries, a malformed --namespace-prefix) print to
// stderr and yield exit 2.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat flag-by-flag dispatch chain
inline ParseResult parse_args(int argc, char** argv, std::string_view usage,
                              const std::function<bool(std::string_view)>& extra = {}) {
    const auto usage_error = [&] {
        std::cerr << usage;
        return ParseResult{std::nullopt, 2};
    };
    Options opts;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is C's contract
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << usage;
            return {std::nullopt, 0};
        }
        if (arg == "--version") {
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is C's contract
            std::string tool = std::filesystem::path(argv[0]).filename().string();
            if (tool.empty()) {  // a pathological argv[0] (empty, or ending in '/')
                tool = argv[0];
            }
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            std::cout << tool << ' ' << kVersion << '\n';
            return {std::nullopt, 0};
        }
        if (arg == "-I") {
            if (++i >= args.size()) {
                return usage_error();
            }
            opts.config.include_paths.push_back(args[i]);
        } else if (arg.rfind("-I", 0) == 0) {
            opts.config.include_paths.push_back(arg.substr(2));
        } else if (arg == "--out-dir") {
            if (++i >= args.size()) {
                return usage_error();
            }
            opts.out_dir = args[i];
        } else if (arg.rfind("--out-dir=", 0) == 0) {
            opts.out_dir = arg.substr(std::string_view("--out-dir=").size());
        } else if (arg == "--no-wellknown") {
            opts.config.use_wellknown = false;
        } else if (arg == "--namespace-prefix") {
            if (++i >= args.size()) {
                return usage_error();
            }
            opts.namespace_prefix = args[i];
        } else if (arg.rfind("--namespace-prefix=", 0) == 0) {
            opts.namespace_prefix = arg.substr(std::string_view("--namespace-prefix=").size());
        } else if (arg == "--depfile") {
            if (++i >= args.size()) {
                return usage_error();
            }
            opts.depfile = args[i];
        } else if (arg.rfind("--depfile=", 0) == 0) {
            opts.depfile = arg.substr(std::string_view("--depfile=").size());
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (extra && extra(arg)) {
            // consumed by the generator-specific flag hook
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown flag '" << arg << "'\n";
            return usage_error();
        } else {
            // Absolutised here, which decides where the header lands. canonical_entry_name rebases
            // an entry onto the first -I that contains it and otherwise returns the spelling it was
            // given -- so a relative spelling became the header's PATH, and one with `../` put the
            // header outside --out-dir entirely. Absolute, the fallback is a path header_path
            // reduces to its basename, which is the rule the CMake helper documents and follows
            // (it absolutises too, which is why generated builds never saw this). An entry that
            // does resolve under an -I is unaffected: canonical_entry_name absolutises internally
            // before relativizing, so it rebases exactly as before.
            // absolute() only prepends the cwd; it does NOT collapse `..`, and must not -- that
            // is a textual rewrite, so `lnk/../x.proto` with `lnk` a symlink would name a file the
            // OS never would (measured: it generated from a different schema, exit 0). Resolution
            // stays with canonical_entry_name's weakly_canonical, which follows links.
            std::error_code ec;
            const std::filesystem::path abs = std::filesystem::absolute(arg, ec);
            opts.entries.push_back(ec ? arg : abs.string());
        }
    }
    if (opts.entries.empty()) {
        return usage_error();
    }
    if (!valid_namespace_prefix(opts.namespace_prefix)) {
        std::cerr << "error: --namespace-prefix must be dot-separated C++ identifiers, got '"
                  << opts.namespace_prefix << "'\n";
        return {std::nullopt, 2};
    }
    return {std::move(opts), 0};
}

// Resolve `entries` (one union batch) and their imports, then run the semantic pipeline. On error
// prints to stderr and returns nullopt; on success returns the analyzed file set and its symbol
// table. (Moving the set is safe for the table: its node pointers reference the set's vector
// storage, which survives the move.)
inline std::optional<std::pair<ResolvedFileSet, SymbolTable>> resolve_and_analyze(
    const std::vector<std::string>& entries, const ResolverConfig& config) {
    SourceRegistry sources;
    auto resolved = resolve(entries, config, sources);
    if (resolved.is_err()) {
        std::cerr << "error: " << render_error(resolved.error(), sources) << '\n';
        return std::nullopt;
    }
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    if (analyzed.is_err()) {
        std::cerr << "error: " << render_error(analyzed.error(), sources) << '\n';
        return std::nullopt;
    }
    return std::make_pair(std::move(set), std::move(analyzed).value());
}

// Write `content` to `path`, creating parent directories; `log_write` (--verbose) logs
// "wrote <path>" to stdout. Returns `path` on success (so a caller can collect every written
// output, e.g. to list them as a depfile's targets); on failure prints an error to stderr and
// returns nullopt -- never reports a file it didn't write.
[[nodiscard]] inline std::optional<std::filesystem::path> write_file(
    const std::filesystem::path& path, std::string_view content, bool log_write = false) {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {  // a bare-filename output has an empty parent -- nothing to create
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            std::cerr << "error: cannot create directory " << parent.string() << ": "
                      << error.message() << '\n';
            return std::nullopt;
        }
    }
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();  // flushes; a full-disk or unwritable-path failure surfaces in the stream state
    if (!out) {
        std::cerr << "error: cannot write " << path.string() << '\n';
        return std::nullopt;
    }
    if (log_write) {
        std::cout << "wrote " << path.string() << '\n';
    }
    return path;
}

// Like write_file (same nullopt-after-error contract), but skips the write when `path` already
// holds exactly `content`. For the shared,
// fixed-content runtime drops, which every invocation writes into a possibly shared out-dir: skipping
// avoids truncate+rewriting the file under a concurrent reader (a GENERATOR=both target, or two targets
// sharing an out-dir) and avoids bumping its mtime, which would force needless consumer recompiles. Do
// NOT use for a tracked build output, whose mtime must advance each run.
[[nodiscard]] inline std::optional<std::filesystem::path> write_shared_file(
    const std::filesystem::path& path, std::string_view content, bool log_write = false) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        const std::ifstream in(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string current = buffer.str();
        if (std::string_view(current) == content) {
            return path;
        }
    }
    return write_file(path, content, log_write);
}

// The output path for `file`'s generated header under `out_dir`: the file's import-relative path with
// ".proto" swapped for `extension` (foo/bar.proto -> <out_dir>/foo/bar<ext>). The mirrored layout
// matches the include-root the generated headers reference.
inline std::filesystem::path header_path(const std::string& out_dir, const FileNode& file,
                                         std::string_view extension) {
    std::filesystem::path rel = file.filename;
    if (rel.is_absolute()) {
        rel = rel.filename();
    }
    // Strip a trailing ".proto" exactly (case-sensitively), not replace_extension() which drops any
    // last extension. This agrees with the CMake helper's `.proto$` rule on names the helper must
    // predict, e.g. `a.b.proto` (-> `a.b`) or `Foo.PROTO` (left as-is by both).
    std::string stem = rel.generic_string();
    static constexpr std::string_view kProto = ".proto";
    if (stem.size() >= kProto.size() &&
        std::string_view(stem).substr(stem.size() - kProto.size()) == kProto) {
        stem.erase(stem.size() - kProto.size());
    }
    // Normalized, so the path WRITTEN is the one header_escapes_out_dir tested. Without this a stem
    // like `a/b/../../d/e/f` -- which normalizes clean, so the check accepts it -- was written
    // literally: it creates the intermediate directories, and traverses any symlink among them, so
    // the header could still land outside the out-dir. lexically_normal also collapses `a/./b`.
    stem = std::filesystem::path(stem).lexically_normal().generic_string();
    return std::filesystem::path(out_dir) / (stem + std::string(extension));
}

// True when `file`'s generated header would land OUTSIDE the out-dir.
//
// Only an IMPORT can reach this: an entry's canonical name is either rebased onto an include dir or
// is the absolute path the CLI stored, and header_path reduces the latter to a basename. An import's
// name is the import string itself -- canonical_import_path normalizes it but never rebases it --
// so `import "../up/u.proto"` names a header outside the out-dir, and no -I can move it. Normalized
// first, so `a/../../b` counts and not just a leading `../`.
[[nodiscard]] inline bool header_escapes_out_dir(const FileNode& file) {
    const std::filesystem::path normalized =
        std::filesystem::path(file.filename).lexically_normal();
    return std::any_of(normalized.begin(), normalized.end(),
                       [](const std::filesystem::path& part) { return part == ".."; });
}

// Write a generated header for `file` under `out_dir` (path per header_path). Returns the path, or
// nullopt after printing an error (see write_file).
[[nodiscard]] inline std::optional<std::filesystem::path> write_header(const std::string& out_dir,
                                                                       const FileNode& file,
                                                                       std::string_view extension,
                                                                       std::string_view content,
                                                                       bool log_write = false) {
    return write_file(header_path(out_dir, file, extension), content, log_write);
}

// `path` made absolute (against the cwd) and lexically normalized, but WITHOUT resolving symlinks --
// matching how CMake and Ninja canonicalize depfile paths (lexically). Produced this way, a depfile
// entry compares equal to the same file as CMake/Ninja name it, so the dependency edge connects.
inline std::filesystem::path lexically_absolute(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path abs = std::filesystem::absolute(path, error);
    return (error ? path : abs).lexically_normal();
}

// The on-disk .proto files `set` (the union closure of `entries`) was built from: each entry plus
// every import found under an include path. Well-known types loaded from the embedded definitions
// are not on disk, so the include-path search misses them and they are correctly excluded --
// unless the user shadows a WKT with their own copy on an include path, in which case that copy
// IS a real dependency and is listed. These are the depfile's prerequisites.
inline std::vector<std::filesystem::path> disk_proto_paths(const std::vector<std::string>& entries,
                                                           const ResolvedFileSet& set,
                                                           const ResolverConfig& config) {
    std::vector<std::filesystem::path> paths;
    // Entries are given as disk paths: list those spellings directly, and skip their canonical
    // names below (the include-resolved spelling can differ from the given one, and dedup would
    // not collapse the two).
    std::unordered_set<std::string> entry_names;
    for (const std::string& entry : entries) {
        paths.emplace_back(entry);
        entry_names.insert(canonical_entry_name(entry, config.include_paths));
    }
    for (const FileNode& file : set.files) {
        if (entry_names.count(file.filename) != 0) {
            continue;
        }
        for (const std::string& include : config.include_paths) {
            const std::filesystem::path full = std::filesystem::path(include) / file.filename;
            std::error_code error;  // an unstattable path (e.g. EACCES) is "not found", not a throw
            if (std::filesystem::exists(full, error)) {
                paths.push_back(full);
                break;
            }
        }
    }
    return paths;
}

namespace detail {

// Collapse duplicates in place, keeping the FIRST occurrence and the caller's order.
//
// Depfile OUTPUTS must use this: Ninja accepts a depfile only when its first target is the edge's
// first output, and a mismatch is not an error -- it reports the outputs dirty and re-runs the
// command on every build ("expected depfile to mention 'x.rp.hpp', got 'x.rp.dump.hpp'"). Sorting
// them did exactly that whenever the first output was not alphabetically first: `<stem>.rp.dump.hpp`
// sorts ahead of the `<stem>.rp.hpp` anchor, so every DUMP consumer regenerated its whole closure
// every time. (GNU Make is unaffected -- it reads the depfile as ordinary rules, in any order.)
inline void dedup_keep_order(std::vector<std::filesystem::path>& paths) {
    std::vector<std::filesystem::path> unique;
    unique.reserve(paths.size());
    for (std::filesystem::path& path : paths) {
        if (std::find(unique.begin(), unique.end(), path) == unique.end()) {
            unique.push_back(std::move(path));
        }
    }
    paths = std::move(unique);
}

// Collapse duplicates by sorting. Only for depfile PREREQUISITES: the build tool merely stats
// them, so their order carries no meaning and sorting keeps the output stable.
inline void dedup_sorted(std::vector<std::filesystem::path>& paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

}  // namespace detail

// Write a Make/Ninja-style depfile (`out1 out2 ... : in1 in2 ...`) declaring that the outputs
// depend on every input. add_custom_command(DEPFILE ...) reads it so codegen re-runs when any input
// .proto changes, including transitive imports a plain DEPENDS list (outputs only) would never catch.
//
// Callers pass the primary (entry) header(s) as `outputs`: re-running the CLI regenerates the whole
// closure, so the build tool's rule that the depfile target match the command's OUTPUT is met with one
// target. That target is named relative to the CLI's working directory, which the build wrapper points
// at the directory the build tool interprets depfile paths against (CMAKE_CURRENT_BINARY_DIR when CMake
// transforms the depfile under CMP0116 NEW, else the top build dir) -- so an output under it gets the
// build node's relative name. An output OUTSIDE that dir (an out-of-tree OUT_DIR) is named absolutely,
// matching how the tool names an out-of-tree node. Prerequisites stay absolute; the build tool only
// stats them. Spaces, '#', '$', and backslash are escaped; duplicates collapsed.
//
// Output ORDER is preserved as given -- Ninja requires the depfile's first target to be the rule's
// first output; it ignores the rest. (An undeclared name is a hard error only once the FIRST target
// has matched -- in first position it produces the same silent every-build rebuild, which is why
// order is the whole contract here.) Callers must pass outputs in the rule's declared order.
// Returns false after printing an error when the depfile cannot be written.
//
// NOLINTBEGIN(bugprone-easily-swappable-parameters): outputs and prereqs are both path vectors and
// a caller that swapped them would emit an inverted rule. Measured, a swap is NOT loud: the first
// target becomes a .proto path, Ninja's first-target comparison mismatches and short-circuits before
// it ever validates the remaining names, so the result is the same silent rebuild-every-time as the
// bug this order exists to prevent. What actually catches it is the depfile stage in check.sh
// (tests/depfile_norebuild.sh), which builds a generated target twice and requires the second build
// to do nothing. Kept as two parameters: one call site, and that stage covers it. (The check stopped
// suppressing this when the two stopped being passed to the same dedup helper: outputs must keep
// their order, prerequisites may be sorted. Bracketed rather than a next-line suppression because
// the diagnostic is reported on the second PARAMETER's line, not the declaration's first line --
// and the marker words are deliberately not spelled out in this prose, since clang-tidy parses
// them wherever they appear in a comment.)
[[nodiscard]] inline bool write_depfile(const std::filesystem::path& depfile_path,
                                        std::vector<std::filesystem::path> outputs,
                                        std::vector<std::filesystem::path> prereqs) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    std::error_code cwd_error;
    const std::filesystem::path base = std::filesystem::current_path(cwd_error);
    const auto as_target = [&](const std::filesystem::path& path) {
        // abs/rel deliberately non-const: they are returned, and const would block the move.
        std::filesystem::path abs = lexically_absolute(path);
        if (cwd_error || base.empty()) {
            return abs;  // no cwd to relativize against -- absolute is the best we can do
        }
        std::filesystem::path rel = abs.lexically_relative(base);
        // Empty (unrelatable, e.g. a different Windows drive) or escaping the build dir (a "../"
        // prefix, i.e. OUT_DIR is outside the build tree)? Then the build tool names this out-of-tree
        // output by its ABSOLUTE path (verified for Ninja), so emit absolute to match. Only an output
        // UNDER the working dir gets a relative node.
        const std::string rels = rel.generic_string();
        if (rels.empty() || rels == ".." || rels.rfind("../", 0) == 0) {
            return abs;
        }
        return rel;
    };
    for (std::filesystem::path& path : outputs) {
        path = as_target(path);  // build-dir-relative, matching the build tool's output node
    }
    for (std::filesystem::path& path : prereqs) {
        path = lexically_absolute(path);
    }
    detail::dedup_keep_order(outputs);  // FIRST target must stay first -- see the helper
    detail::dedup_sorted(prereqs);
    // Escape per the depfile grammar GCC's -MD emits (what CMake and Ninja consume): a backslash before
    // a space, '#', or backslash; '$' doubled. (':' is left alone -- it does not occur in POSIX paths
    // and is the rule separator.)
    const auto escape = [](const std::filesystem::path& path) {
        std::string out;
        for (const char ch : path.generic_string()) {
            switch (ch) {
                case ' ':
                case '#':
                case '\\':
                    out.push_back('\\');
                    out.push_back(ch);
                    break;
                case '$':
                    out += "$$";
                    break;
                default:
                    out.push_back(ch);
            }
        }
        return out;
    };
    const std::filesystem::path depfile_dir = depfile_path.parent_path();
    if (!depfile_dir.empty()) {  // a bare-filename depfile (no dir) has an empty parent
        std::error_code error;
        std::filesystem::create_directories(depfile_dir, error);
        if (error) {
            std::cerr << "error: cannot create directory " << depfile_dir.string() << ": "
                      << error.message() << '\n';
            return false;
        }
    }
    std::ofstream depfile(depfile_path, std::ios::binary);
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        depfile << (i == 0 ? "" : " ") << escape(outputs[i]);
    }
    depfile << ':';
    for (const std::filesystem::path& prereq : prereqs) {
        depfile << ' ' << escape(prereq);
    }
    depfile << '\n';
    depfile.close();
    if (!depfile) {
        std::cerr << "error: cannot write " << depfile_path.string() << '\n';
        return false;
    }
    return true;
}

}  // namespace rapidproto::cli
