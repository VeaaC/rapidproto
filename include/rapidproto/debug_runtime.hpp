// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Header-only support for the generated `*.rp.debug.hpp` dumpers: a JSON-string escaper, a lowercase
// hex encoder for `bytes`, and a small indenting Writer wrapping an std::ostream. Dependency-light on
// purpose (only <ostream>/<string_view>/<cstdint>) so a debug header pulls in nothing heavy.

#include <cstdint>
#include <ostream>
#include <string_view>

namespace rapidproto::debug {

// Write `s` as a JSON string body (no surrounding quotes): escape the mandatory control/quote/backslash
// characters, pass everything else (incl. UTF-8) through verbatim.
inline void write_json_escaped(std::ostream& os, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
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
                if (uc < 0x20) {  // other control char -> \u00XX
                    os << "\\u00" << kHex[(uc >> 4) & 0xF] << kHex[uc & 0xF];
                } else {
                    os << ch;
                }
        }
    }
}

// Write `s` (raw bytes) as lowercase hex, two chars per byte, unbounded.
inline void write_hex(std::ostream& os, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const char ch : s) {
        const auto uc = static_cast<unsigned char>(ch);
        os << kHex[(uc >> 4) & 0xF] << kHex[uc & 0xF];
    }
}

// A minimal pretty-printing writer: 2-space indent, one field/element per line. The generated dumpers
// drive it explicitly (open_object / key / close_object, etc.); it holds only the stream + current
// depth, so it is trivially cheap to pass by reference through the recursion.
class Writer {
public:
    explicit Writer(std::ostream& os) noexcept : m_os(os) {}

    std::ostream& os() noexcept { return m_os; }

    void newline() {
        m_os << '\n';
        for (int i = 0; i < m_indent; ++i) {
            m_os << "  ";
        }
    }

    // Open a `{`/`[`; subsequent entries are emitted one-per-line at the deeper indent. `first` is a
    // per-scope flag the caller flips: it inserts a `,` before every entry except the first.
    void open(char bracket) {
        m_os << bracket;
        ++m_indent;
    }
    void close(char bracket, bool any) {
        --m_indent;
        if (any) {
            newline();
        }
        m_os << bracket;
    }

    // Emit the separator + indent before an entry, then (for object entries) the `"key": ` prefix.
    void entry_sep(bool& first) {
        if (!first) {
            m_os << ',';
        }
        first = false;
        newline();
    }
    void key(std::string_view name) { m_os << '"' << name << "\": "; }

private:
    std::ostream& m_os;
    int m_indent = 0;
};

}  // namespace rapidproto::debug
