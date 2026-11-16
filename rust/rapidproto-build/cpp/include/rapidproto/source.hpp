#pragma once

// Source registry + error renderer. The lexer/parser/resolver record errors as a (SourceId,
// byte_offset, message) -- see Error in result.hpp. To turn that into a human-readable
// `file:line:col: message`, something must remember each SourceId's filename and text: that is the
// SourceRegistry, populated as files are loaded. `render_error` resolves an Error against it.

#include <cstddef>
#include <string>
#include <vector>

#include "rapidproto/result.hpp"
#include "rapidproto/source_id.hpp"

namespace rapidproto {

// Remembers, per SourceId, the source's filename and full text, so a byte offset into that source
// can be rendered as a 1-based line:col. SourceIds are dense and assigned in registration order.
class SourceRegistry {
public:
    // Register a source (its filename and full text); returns the SourceId that indexes it.
    SourceId add(std::string filename, std::string text);

    // The registered filename for `id`, or "" if `id` is invalid / not registered.
    [[nodiscard]] const std::string& filename(SourceId id) const;

    struct LineCol {
        std::size_t line = 1;    // 1-based
        std::size_t column = 1;  // 1-based, counted in bytes
    };

    // The 1-based line:col of `byte_offset` within source `id`. An offset past the end clamps to
    // the end; an unknown `id` yields {1, 1}.
    [[nodiscard]] LineCol line_col(SourceId id, std::size_t byte_offset) const;

private:
    struct Source {
        std::string filename;
        std::string text;
    };
    std::vector<Source> m_sources;  // indexed by SourceId::index()
};

// Render `error` as "file:line:col: message" using `registry` to resolve its SourceId. When the
// error is not tied to a known source, the bare message is returned (offset alone is not useful to
// a human without the text).
std::string render_error(const Error& error, const SourceRegistry& registry);

}  // namespace rapidproto
