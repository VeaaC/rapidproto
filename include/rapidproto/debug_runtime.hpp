// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Header-only support for the generated `*.rp.debug.hpp` dumpers: a JSON-string escaper, a lowercase
// hex encoder for `bytes`, and a small indenting Writer wrapping an std::ostream. Dependency-light on
// purpose (only <ostream>/<string_view>/<cstdint>) so a debug header pulls in nothing heavy.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace rapidproto::debug {

// The 16 lowercase hex digits. std::string_view (not a C array) so it is a plain object, not a
// decayable array the strict checks flag; indexing a nibble [0,15] stays in bounds.
inline constexpr std::string_view kHexDigits = "0123456789abcdef";
inline constexpr unsigned kNibbleBits = 4;         // bits per hex digit
inline constexpr unsigned kNibbleMask = 0xF;       // low nibble
inline constexpr unsigned kFirstPrintable = 0x20;  // < this is a JSON control char needing \u00XX

// The two lowercase-hex chars of one byte, high nibble first.
inline char hex_high(unsigned char uc) noexcept {
    return kHexDigits[(uc >> kNibbleBits) & kNibbleMask];
}
inline char hex_low(unsigned char uc) noexcept {
    return kHexDigits[uc & kNibbleMask];
}

// Write `s` as a JSON string body (no surrounding quotes): escape the mandatory control/quote/backslash
// characters, pass everything else (incl. UTF-8) through verbatim.
inline void write_json_escaped(std::ostream& os, std::string_view s) {
    for (const char ch : s) {
        const auto uc = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':
                os << "\\\"";
                break;
            case '\\':
                os << "\\\\";
                break;
            case '\n':
                os << "\\n";
                break;
            case '\r':
                os << "\\r";
                break;
            case '\t':
                os << "\\t";
                break;
            default:
                if (uc < kFirstPrintable) {  // other control char -> \u00XX
                    os << "\\u00" << hex_high(uc) << hex_low(uc);
                } else {
                    os << ch;
                }
        }
    }
}

// Write `s` (raw bytes) as lowercase hex, two chars per byte, unbounded.
inline void write_hex(std::ostream& os, std::string_view s) {
    for (const char ch : s) {
        const auto uc = static_cast<unsigned char>(ch);
        os << hex_high(uc) << hex_low(uc);
    }
}

// A width-adaptive JSON-like writer. Each object/array is a `group(...)`: the writer first renders it
// COMPACT (single line) into a scratch buffer and, if that fits the remaining width budget, splices it
// verbatim; otherwise it re-emits the group MULTI-LINE (one entry per line, 2-space indent) and each
// child independently retries compact. So a group is multi-line only if it (or a descendant) can't fit
// -- which forces every ancestor multi-line too, while siblings/children stay free to be compact.
//
// Cost stays ~linear: a group that fits is rendered once (its buffer is reused as output); a group that
// doesn't wastes at most `width` chars on the failed compact probe -- `overflowed()` lets generated
// loops bail the moment the buffer exceeds the budget, so a too-wide array isn't rendered in full.
class Writer {
public:
    static constexpr std::size_t kDefaultWidth = 120;  // the default line-width budget

    explicit Writer(std::ostream& os, std::size_t width = kDefaultWidth) noexcept
        : m_out(os), m_sink(&os), m_width(width) {}

    // The active sink the generated value-writers stream into: the scratch buffer during a compact
    // probe, the real output otherwise.
    std::ostream& os() noexcept { return *m_sink; }

    // True once the in-progress compact probe has exceeded its width budget. Generated loops check it
    // and `break` -- the group is going multi-line anyway, so there is no point rendering the rest.
    bool overflowed() noexcept {
        return m_compact && static_cast<std::size_t>(m_buf.tellp()) > m_avail;
    }

    // The `"key": ` prefix before an object entry's value.
    void key(std::string_view name) {
        *m_sink << '"' << name << "\": ";
        if (!m_compact) {
            m_column += name.size() + kKeyPunctWidth;  // the two quotes, the colon, and the space
        }
    }

    // The separator before an entry: `, ` inline, or `,` + newline + indent when multi-line. `first`
    // is a per-group flag the caller flips (no separator before the first entry).
    void entry_sep(bool& first) {
        if (m_compact) {
            if (!first) {
                *m_sink << ", ";
            }
            first = false;
            return;
        }
        if (!first) {
            *m_sink << ',';
        }
        first = false;
        newline();
        m_column = static_cast<std::size_t>(m_indent) * kIndentWidth;
    }

    // Emit an object/array whose entries are produced by `body` (a re-runnable callable). Compact-first,
    // multi-line on overflow. Nested inside an ongoing compact probe, render straight into the buffer.
    // `body` is taken by const-ref (not a forwarding reference): it is invoked up to twice -- once for
    // the compact probe, once for the multi-line re-emit -- so it can never be forwarded/moved-from.
    template <class Body>
    void group(char open_ch, char close_ch, const Body& body) {
        if (m_compact) {
            *m_sink << open_ch;
            body();
            *m_sink << close_ch;
            return;
        }
        const std::size_t avail = m_width > m_column ? m_width - m_column : 0;
        m_buf.str(std::string());
        m_buf.clear();
        m_sink = &m_buf;
        m_compact = true;
        m_avail = avail;
        *m_sink << open_ch;
        body();
        *m_sink << close_ch;
        m_compact = false;
        m_sink = &m_out;
        const std::string s = m_buf.str();
        if (s.size() <= avail) {  // the compact form fits -> splice it verbatim
            m_out << s;
            m_column += s.size();
            return;
        }
        // Too wide: re-emit multi-line. The compact form's length tells us whether the group is empty
        // (just the two brackets) so an empty group still closes on the same line.
        const bool any = s.size() > kEmptyGroupChars;
        m_out << open_ch;
        ++m_indent;
        body();
        --m_indent;
        if (any) {
            newline();
        }
        m_out << close_ch;
        m_column = static_cast<std::size_t>(m_indent) * kIndentWidth + 1;  // +1: the close bracket
    }

    void newline() {
        m_out << '\n';
        for (int i = 0; i < m_indent; ++i) {
            m_out << "  ";
        }
    }

private:
    static constexpr std::size_t kIndentWidth = 2;  // columns per nesting level (a 2-space indent)
    static constexpr std::size_t kKeyPunctWidth = 4;  // a `"key": `'s two quotes + colon + space
    static constexpr std::size_t kEmptyGroupChars =
        2;  // an empty group's compact form is just `[]`/`{}`

    // A Writer wraps a caller-owned ostream for its lifetime (like a stream manipulator); the
    // reference is the point of the type.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::ostream& m_out;       // the real output
    std::ostream* m_sink;      // where os()/emit go now: &m_out, or &m_buf during a compact probe
    std::ostringstream m_buf;  // scratch for the compact probe
    std::size_t m_width;       // line-width budget
    std::size_t m_avail = 0;   // remaining width for the current compact probe
    std::size_t m_column = 0;  // current column on the real output's line
    int m_indent = 0;
    bool m_compact = false;
};

}  // namespace rapidproto::debug
