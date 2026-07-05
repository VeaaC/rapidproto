#pragma once

// SourceId: a lightweight handle identifying one source buffer within a file set.
//
// The lexer and parser work on a single buffer, where a byte offset alone locates a position. Once
// imports pull in many files, a position needs to say WHICH file too — that is this handle. The
// import resolver assigns a SourceId per loaded file and registers it (filename + text) in a
// SourceRegistry (see source.hpp); an Error carries that SourceId + a byte offset, which
// render_error turns into `file:line:col`. A default-constructed SourceId is Invalid (an error not
// tied to a source — e.g. "entry file not found" — renders as its bare message).

#include <cstdint>

namespace rapidproto {

class SourceId {
public:
    // Default-constructed SourceId is Invalid (not tied to any source).
    constexpr SourceId() noexcept = default;
    explicit constexpr SourceId(std::uint32_t index) noexcept : m_index(index) {}

    [[nodiscard]] constexpr bool valid() const noexcept { return m_index != kInvalid; }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return m_index; }

    static constexpr SourceId invalid() noexcept { return SourceId{}; }

    friend constexpr bool operator==(SourceId a, SourceId b) noexcept {
        return a.m_index == b.m_index;
    }
    friend constexpr bool operator!=(SourceId a, SourceId b) noexcept { return !(a == b); }

private:
    static constexpr std::uint32_t kInvalid = 0xFFFF'FFFFU;
    std::uint32_t m_index = kInvalid;
};

}  // namespace rapidproto
