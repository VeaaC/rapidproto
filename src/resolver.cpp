#include "rapidproto/resolver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"
#include "rapidproto/source_id.hpp"
#include "rapidproto/wellknown.hpp"

namespace rapidproto {
namespace {

// The import graph is resolved by a recursive DFS (Resolver::visit). A deep import CHAIN (a file
// imports a file imports ... thousands deep) would overflow the stack, so cap the chain depth and
// fail cleanly -- the safety floor is unconditional: never crash, even on a hostile input set. Real
// import chains are double-digit at most.
constexpr std::size_t kMaxImportDepth = 100;

std::optional<std::string> read_file_contents(const std::filesystem::path& path) {
    const std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// Lex + parse one source into a FileNode (filename set), tagging any lex/parse error with `id` and
// a source byte offset, so it renders as file:line:col.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): filename vs source, distinct roles
Result<FileNode> parse_source(const std::string& filename, std::string source, SourceId id) {
    auto lexed_result = lex(std::move(source));
    if (!lexed_result) {
        Error error = std::move(lexed_result).error();
        error.source = id;  // the lexer's byte offset indexes into this source
        return error;
    }
    const LexResult lexed = std::move(lexed_result).value();

    auto parsed = parse_file(Range<Token>(lexed.tokens));
    if (!parsed) {
        // Parser offsets are token indices; map back to a source byte offset.
        Error error = std::move(parsed).error();
        const std::size_t token_index = error.byte_offset;
        std::size_t byte_offset = lexed.source ? lexed.source->size() : 0;
        if (token_index < lexed.tokens.size()) {
            byte_offset = lexed.tokens[token_index].byte_offset;
        }
        error.byte_offset = byte_offset;
        error.source = id;
        return error;
    }
    if (!parsed.value().remaining.empty()) {
        const std::size_t consumed = lexed.tokens.size() - parsed.value().remaining.size();
        const std::size_t byte_offset =
            consumed < lexed.tokens.size() ? lexed.tokens[consumed].byte_offset : 0;
        return Error{id, byte_offset, "unexpected trailing input"};
    }

    FileNode file = std::move(parsed.value().value);
    file.filename = filename;
    return file;
}

enum class Color : std::uint8_t { Gray, Black };

// DFS import resolver with white/gray/black cycle detection and post-order (topological)
// collection.
class Resolver {
public:
    Resolver(ResolverConfig config, SourceRegistry& sources)
        : m_config(std::move(config)), m_sources(sources) {}

    // Resolve every entry into ONE union set. Entries visit in argument order; a file reached
    // twice -- listed twice, or listed AND imported by an earlier entry -- resolves once (the
    // canonical name is the identity), so the union stays a set and the topological order holds
    // across entries.
    Result<ResolvedFileSet> run(const std::vector<std::string>& entry_files) {
        for (const std::string& entry_file : entry_files) {
            const std::string entry_name = canonical_entry_name(entry_file, m_config.include_paths);
            if (m_parsed.count(entry_name) != 0) {
                continue;  // already resolved via an earlier entry (as it, or as its import)
            }
            auto source = read_file_contents(entry_file);
            if (!source) {
                return Error{0, "entry file not found: " + entry_file};
            }
            if (auto error = visit(entry_name, std::move(*source))) {
                return *error;
            }
        }

        ResolvedFileSet set;
        for (auto& name : m_order) {
            set.file_index.emplace(name, set.files.size());
            set.files.push_back(std::move(m_parsed[name]));
        }
        return set;
    }

private:
    // Look up an import in the include paths, then the embedded well-known types.
    std::optional<std::string> read_import(const std::string& import_path) {
        for (const auto& include : m_config.include_paths) {
            if (include.empty()) {
                continue;  // an empty include entry would silently mean "search CWD"
            }
            const std::filesystem::path full = std::filesystem::path(include) / import_path;
            if (std::filesystem::exists(full)) {
                // The first include path that has the file owns it: do not fall through to a
                // shadowed copy (or the embedded WKT) if it happens to be unreadable.
                return read_file_contents(full);
            }
        }
        if (m_config.use_wellknown) {
            if (auto embedded = wellknown_source(import_path)) {
                return std::string(*embedded);
            }
        }
        return std::nullopt;
    }

    // Render the current gray DFS stack as a cycle path ending back at `import_path`.
    std::string render_cycle(const std::string& import_path) const {
        std::string message = "import cycle: ";
        const auto start = std::find(m_stack.begin(), m_stack.end(), import_path);
        for (auto it = start; it != m_stack.end(); ++it) {
            message += *it;
            message += " -> ";
        }
        message += import_path;
        return message;
    }

    // Returns an Error on failure, nullopt on success.
    std::optional<Error> visit(const std::string& name, std::string source) {
        if (m_stack.size() >= kMaxImportDepth) {  // m_stack.size() == current import-chain depth
            std::string message = "import chain exceeds maximum depth (";
            message += std::to_string(kMaxImportDepth);
            message += ")";
            return Error{0, std::move(message)};
        }
        m_color[name] = Color::Gray;
        m_stack.push_back(name);

        // Register the source (copies the text) so a parse error -- or a later semantic error that
        // points into this file -- can be rendered as file:line:col.
        const SourceId id = m_sources.add(name, source);
        auto file_result = parse_source(name, std::move(source), id);
        if (!file_result) {
            return std::move(file_result).error();
        }
        FileNode file = std::move(file_result).value();
        file.source = id;  // so a semantic error pointing at one of this file's nodes can render

        std::vector<std::string> imports;
        imports.reserve(file.imports.size());
        for (const auto& import : file.imports) {
            imports.push_back(import.path);
        }
        m_parsed.emplace(name, std::move(file));

        for (const auto& raw_import : imports) {
            // Canonicalize so different spellings of the same path map to one entry.
            const std::string import_path = canonical_import_path(raw_import);
            if (const auto found = m_color.find(import_path); found != m_color.end()) {
                if (found->second == Color::Gray) {
                    return Error{0, render_cycle(import_path)};
                }
                continue;  // Black: already fully resolved (diamond dependency)
            }
            auto import_source = read_import(import_path);
            if (!import_source) {
                std::string message = "import not found: ";
                message += import_path;
                message += " (imported by ";
                message += name;
                message += ")";
                // The most common new-user failure is a missing -I, so say what was searched.
                message += m_config.include_paths.empty()
                               ? "; no include paths were given -- add -I <dir>"
                               : "; searched " + std::to_string(m_config.include_paths.size()) +
                                     " include path(s) -- add the right -I <dir>";
                return Error{0, std::move(message)};
            }
            if (auto error = visit(import_path, std::move(*import_source))) {
                return error;
            }
        }

        m_color[name] = Color::Black;
        m_stack.pop_back();
        m_order.push_back(name);  // post-order => topological
        return std::nullopt;
    }

    ResolverConfig m_config;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): borrowed for the run
    SourceRegistry& m_sources;  // every loaded file is registered here (filename + text)
    std::unordered_map<std::string, Color> m_color;
    std::unordered_map<std::string, FileNode> m_parsed;
    std::vector<std::string> m_stack;  // current gray DFS path (for cycle diagnostics)
    std::vector<std::string> m_order;
};

}  // namespace

std::string canonical_import_path(const std::string& import_path) {
    return std::filesystem::path(import_path).lexically_normal().generic_string();
}

std::string canonical_entry_name(const std::string& entry,
                                 const std::vector<std::string>& include_paths) {
    std::error_code ec;
    const std::filesystem::path abs_entry = std::filesystem::weakly_canonical(entry, ec);
    if (ec) {
        return entry;
    }
    for (const auto& include : include_paths) {
        const std::filesystem::path abs_include = std::filesystem::weakly_canonical(include, ec);
        if (ec) {
            continue;
        }
        const std::filesystem::path relative =
            std::filesystem::relative(abs_entry, abs_include, ec);
        if (ec || relative.empty()) {
            continue;
        }
        const std::string name = relative.generic_string();
        if (name.rfind("..", 0) != 0) {  // entry is under this include path
            return name;
        }
    }
    return entry;
}

Result<ResolvedFileSet> resolve(const std::string& entry_file, const ResolverConfig& config,
                                SourceRegistry& sources) {
    return Resolver(config, sources).run({entry_file});
}

Result<ResolvedFileSet> resolve(const std::string& entry_file, const ResolverConfig& config) {
    SourceRegistry discard;  // callers using this overload do not render with line:col
    return Resolver(config, discard).run({entry_file});
}

Result<ResolvedFileSet> resolve(const std::vector<std::string>& entry_files,
                                const ResolverConfig& config, SourceRegistry& sources) {
    return Resolver(config, sources).run(entry_files);
}

}  // namespace rapidproto
