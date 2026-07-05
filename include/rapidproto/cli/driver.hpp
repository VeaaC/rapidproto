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

#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"
#include "rapidproto/version.hpp"

namespace rapidproto::cli {

// A --namespace-prefix is empty (no prefix) or a dot-separated list of C++ identifiers
// (`[A-Za-z_][A-Za-z0-9_]*`). This catches CLI typos (e.g. `rp:`) up front instead of emitting
// uncompilable generated code.
inline bool valid_namespace_prefix(std::string_view p) {
    if (p.empty()) {
        return true;
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
    ResolverConfig config;         // -I include paths, --no-wellknown
    std::string out_dir = ".";     // --out-dir
    std::string namespace_prefix;  // --namespace-prefix (dotted, prepended to each C++ namespace)
    std::string depfile;           // --depfile (emit a Make/Ninja depfile for incremental codegen)
    bool verbose = false;          // --verbose / -v: log each written file
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
            opts.entries.push_back(arg);
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
    return std::filesystem::path(out_dir) / (stem + std::string(extension));
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
// Returns false after printing an error when the depfile cannot be written.
[[nodiscard]] inline bool write_depfile(const std::filesystem::path& depfile_path,
                                        std::vector<std::filesystem::path> outputs,
                                        std::vector<std::filesystem::path> prereqs) {
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
    const auto dedup = [](std::vector<std::filesystem::path>& paths) {
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    };
    for (std::filesystem::path& path : outputs) {
        path = as_target(path);  // build-dir-relative, matching the build tool's output node
    }
    for (std::filesystem::path& path : prereqs) {
        path = lexically_absolute(path);
    }
    dedup(outputs);
    dedup(prereqs);
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
