#pragma once

// Range<T>: a minimal, non-owning, read-only view over a contiguous sequence
// of T (a C++17 stand-in for std::span). Used throughout the parser as the
// input cursor: combinators consume a Range and return a smaller subspan of it.
//
// Lifetime: a Range never owns its storage. The underlying buffer (source
// string, token vector, ...) must outlive every Range that views into it.

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rapidproto {

template <typename T>
class Range {
public:
    using value_type = T;
    using const_iterator = const T*;

    constexpr Range() noexcept = default;

    constexpr Range(const T* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    // Precondition: first <= last (both into the same buffer).
    constexpr Range(const T* first, const T* last) noexcept
        : m_data(first), m_size(static_cast<std::size_t>(last - first)) {}

    // From a C array. Matches std::span semantics: extent is N, so for a string
    // literal this INCLUDES the trailing '\0'. Prefer the string/string_view
    // constructors when you want character-exact bounds.
    // The C-array reference parameter is the point: how a span deduces a literal's extent.
    template <std::size_t N>
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    constexpr Range(const T (&arr)[N]) noexcept : m_data(arr), m_size(N) {}

    Range(const std::vector<T>& vec) noexcept : m_data(vec.data()), m_size(vec.size()) {}

    // Char-only conveniences. The parameter types are concrete (std::string /
    // std::string_view), so U is NOT deduced from the argument — the gate keys on
    // the class parameter T. Range<NonChar> therefore cleanly does not advertise
    // these (is_constructible is correctly false), instead of selecting them and
    // hard-erroring in the body.
    template <typename U = T, std::enable_if_t<std::is_same_v<U, char>, int> = 0>
    Range(const std::string& str) noexcept : m_data(str.data()), m_size(str.size()) {}

    template <typename U = T, std::enable_if_t<std::is_same_v<U, char>, int> = 0>
    constexpr Range(std::string_view str) noexcept : m_data(str.data()), m_size(str.size()) {}

    // Reject obviously-dangling construction from owning temporaries: the viewed
    // buffer would die at the end of the full-expression. (A string_view temporary
    // owns nothing, so that path stays allowed.)
    Range(std::vector<T>&&) = delete;
    template <typename U = T, std::enable_if_t<std::is_same_v<U, char>, int> = 0>
    Range(std::string&&) = delete;

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): Range IS the bounds-checked
    // span abstraction the guideline says to reach for -- pointer math is its implementation.
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return m_data; }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return m_data + m_size; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] constexpr const T* data() const noexcept { return m_data; }

    // Precondition: i < size(). front() requires !empty().
    constexpr const T& operator[](std::size_t i) const noexcept { return m_data[i]; }
    [[nodiscard]] constexpr const T& front() const noexcept { return m_data[0]; }

    // Both subspans clamp to bounds (never read past end), so an over-long
    // offset/count yields a shorter or empty Range rather than UB.
    [[nodiscard]] constexpr Range subspan(std::size_t offset) const noexcept {
        offset = offset < m_size ? offset : m_size;
        return Range(m_data + offset, m_size - offset);
    }

    [[nodiscard]] constexpr Range subspan(std::size_t offset, std::size_t count) const noexcept {
        offset = offset < m_size ? offset : m_size;
        const std::size_t avail = m_size - offset;
        count = count < avail ? count : avail;
        return Range(m_data + offset, count);
    }

    template <typename U = T, std::enable_if_t<std::is_same_v<U, char>, int> = 0>
    [[nodiscard]] constexpr std::string_view to_string_view() const noexcept {
        return std::string_view(m_data, m_size);
    }

    friend constexpr bool operator==(const Range& a, const Range& b) {
        if (a.m_size != b.m_size) {
            return false;
        }
        for (std::size_t i = 0; i < a.m_size; ++i) {
            if (!(a.m_data[i] == b.m_data[i])) {
                return false;
            }
        }
        return true;
    }
    friend constexpr bool operator!=(const Range& a, const Range& b) { return !(a == b); }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

private:
    const T* m_data = nullptr;
    std::size_t m_size = 0;
};

}  // namespace rapidproto
