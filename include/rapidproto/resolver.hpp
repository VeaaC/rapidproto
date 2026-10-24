#pragma once

// Import resolver: given an entry .proto file, read and parse it and all transitive
// imports, returning the file set in topological order (dependencies before
// dependents). Imports are looked up in the configured include paths first, then the
// embedded well-known types. Import cycles, missing imports, and parse errors are
// reported as errors.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"

namespace rapidproto {

struct ResolverConfig {
    // Reads a file's text by name, or nullopt if it has no such file (see source_provider).
    using SourceProvider = std::function<std::optional<std::string>(const std::string&)>;

    std::vector<std::string> include_paths;  // searched in order for imports
    bool use_wellknown = true;               // fall back to embedded WKTs after include paths
    // In-memory source override, for a host that sends the sources (the request-frame front-end,
    // native or wasm): when set, every
    // source is read through this -- the entry by the name it was given, each import by its
    // canonical path -- instead of the disk, and `include_paths` is ignored. The embedded
    // well-known types are still consulted as a fallback, so a provider need not carry them. Returns
    // the file's text, or nullopt if it has no such file (resolution then falls back to the WKTs, or
    // reports the import as not found, exactly as the disk path would).
    //
    // Contract: imports are looked up by their canonical path (canonical_import_path), but an ENTRY
    // is looked up verbatim by the name it was given (no filesystem canonicalization runs). So the
    // host must pass an entry under the SAME name any importer would use for it -- otherwise the same
    // file resolves twice (once as the entry, once as the import) and its types collide downstream.
    SourceProvider source_provider;
};

struct ResolvedFileSet {
    std::vector<FileNode> files;  // topological order: a file's imports precede it
    std::unordered_map<std::string, std::size_t> file_index;  // canonical name -> index in `files`
};

// Resolve `entry_file` (read directly from disk) and its transitive imports, registering every
// loaded file (filename + text) in `sources` so a returned Error -- which carries a SourceId + byte
// offset -- can be rendered as `file:line:col` (see render_error). `sources` is populated even on
// failure (the file the error points into is registered before the error is produced), so the
// caller can render the error after a non-ok return.
Result<ResolvedFileSet> resolve(const std::string& entry_file, const ResolverConfig& config,
                                SourceRegistry& sources);

// Convenience overload for callers that do not render errors with line:col (the source registry is
// discarded). Errors still carry their message; only the file:line:col rendering is unavailable.
Result<ResolvedFileSet> resolve(const std::string& entry_file, const ResolverConfig& config);

// Resolve MANY entries into ONE union set (a batch generation): entries visit in argument order,
// and a file reached twice -- listed twice, or listed and also imported by another entry --
// resolves once (canonical-name identity), so shared imports parse once and the whole batch
// shares one topological order. A one-entry batch is identical to the single-entry overloads.
Result<ResolvedFileSet> resolve(const std::vector<std::string>& entry_files,
                                const ResolverConfig& config, SourceRegistry& sources);

// Normalize an import path to a stable canonical key (collapses "./" and "x/../"), so the same file
// imported via different spellings maps to one entry. Used by the resolver and the type resolver's
// visibility lookup so both agree on file identity.
std::string canonical_import_path(const std::string& import_path);

// The canonical key for an ENTRY given as a disk path: its import path relative to the first
// include path containing it (so an entry that is also imported maps to the same key), else the
// path as given. The CLI uses it to match entries against a batch's resolved files.
std::string canonical_entry_name(const std::string& entry,
                                 const std::vector<std::string>& include_paths);

}  // namespace rapidproto
