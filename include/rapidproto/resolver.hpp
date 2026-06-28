#pragma once

// Import resolver: given an entry .proto file, read and parse it and all transitive
// imports, returning the file set in topological order (dependencies before
// dependents). Imports are looked up in the configured include paths first, then the
// embedded well-known types. Import cycles, missing imports, and parse errors are
// reported as errors.

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"

namespace rapidproto {

struct ResolverConfig {
    std::vector<std::string> include_paths;  // searched in order for imports
    bool use_wellknown = true;               // fall back to embedded WKTs after include paths
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

// Normalize an import path to a stable canonical key (collapses "./" and "x/../"), so the same file
// imported via different spellings maps to one entry. Used by the resolver and the type resolver's
// visibility lookup so both agree on file identity.
std::string canonical_import_path(const std::string& import_path);

}  // namespace rapidproto
