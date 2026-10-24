#include "rapidproto/source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "rapidproto/result.hpp"
#include "rapidproto/source_id.hpp"

namespace rapidproto {

SourceId SourceRegistry::add(std::string filename, std::string text) {
    const auto index = static_cast<std::uint32_t>(m_sources.size());
    m_sources.push_back({std::move(filename), std::move(text)});
    return SourceId{index};
}

const std::string& SourceRegistry::filename(SourceId id) const {
    static const std::string kEmpty;
    if (!id.valid() || id.index() >= m_sources.size()) {
        return kEmpty;
    }
    return m_sources[id.index()].filename;
}

SourceRegistry::LineCol SourceRegistry::line_col(SourceId id, std::size_t byte_offset) const {
    if (!id.valid() || id.index() >= m_sources.size()) {
        return {};
    }
    const std::string& text = m_sources[id.index()].text;
    const std::size_t end = std::min(byte_offset, text.size());
    LineCol pos;
    for (std::size_t i = 0; i < end; ++i) {
        if (text[i] == '\n') {
            ++pos.line;
            pos.column = 1;
        } else {
            ++pos.column;
        }
    }
    return pos;
}

std::string render_error(const Error& error, const SourceRegistry& registry) {
    const std::string& name = registry.filename(error.source);
    if (name.empty()) {
        return error.message;
    }
    const SourceRegistry::LineCol pos = registry.line_col(error.source, error.byte_offset);
    return name + ":" + std::to_string(pos.line) + ":" + std::to_string(pos.column) + ": " +
           error.message;
}

}  // namespace rapidproto
