// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Header-only support for the generated `*.rp.dump.hpp` dumpers: a JSON-string escaper, a lowercase
// hex encoder for `bytes`, a small indenting Writer wrapping an std::ostream, and a DumpOptions bag
// (width / start-indent / skip-paths). Dependency-light so a dump header pulls in nothing heavy.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rapidproto::dump {

// Internals of the two encoders below. Not part of the surface a generated dumper (or a consumer)
// calls -- only write_json_escaped / write_hex / Writer / DumpOptions are.
namespace detail {

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

}  // namespace detail

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
                if (uc < detail::kFirstPrintable) {  // other control char -> \u00XX
                    os << "\\u00" << detail::hex_high(uc) << detail::hex_low(uc);
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
        os << detail::hex_high(uc) << detail::hex_low(uc);
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

    // width/indent order is stable and documented; DumpOptions (the recommended entry) names them.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    explicit Writer(std::ostream& os, std::size_t width = kDefaultWidth,
                    std::size_t start_indent = 0,
                    const std::vector<std::string_view>* skip = nullptr)
        // A deeper start indent raises m_column, which shrinks the width budget accordingly. Clamp an
        // absurd caller value to kMaxIndent so the size_t->int cast stays well-defined and newline()
        // can't spin printing billions of spaces (no real dump nests anywhere near this deep).
        : m_out(os),
          m_sink(&os),
          m_width(width),
          m_skip(skip),
          m_column((start_indent < kMaxIndent ? start_indent : kMaxIndent) * kIndentWidth),
          m_indent(static_cast<int>(start_indent < kMaxIndent ? start_indent : kMaxIndent)) {}

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

    // ── skip support (dynamic, by qualified field path) ──────────────────────────────────────────
    // The generated dumper calls begin_field() for every field. A field whose dotted path (e.g.
    // "people.address.city") is in the caller's skip list is omitted entirely -- no separator, no key,
    // no value. push_path()/pop_path() maintain the path prefix as the dumper recurses into
    // message / repeated-message / map fields (leaf and repeated-scalar fields need no push).

    // The field's full path is the current prefix + `leaf`; return true if the caller listed it to skip.
    bool skipped(std::string_view leaf) {
        if (m_skip == nullptr || m_skip->empty()) {
            return false;
        }
        const std::size_t base = m_path.size();
        if (base != 0) {
            m_path.push_back('.');
        }
        m_path.append(leaf.data(), leaf.size());
        bool hit = false;
        for (const std::string_view path : *m_skip) {
            if (path == m_path) {
                hit = true;
                break;
            }
        }
        m_path.resize(base);  // restore the prefix; push_path() is what actually descends
        return hit;
    }

    // Begin one object entry: skip it (return false, emit nothing) if its path is listed, else write the
    // separator and the `"key": ` prefix and return true so the caller emits the value.
    bool begin_field(bool& first, std::string_view name) {
        if (skipped(name)) {
            return false;
        }
        entry_sep(first);
        key(name);
        return true;
    }

    void push_path(std::string_view name) {
        m_path_stack.push_back(m_path.size());
        if (!m_path.empty()) {
            m_path.push_back('.');
        }
        m_path.append(name.data(), name.size());
    }
    void pop_path() {
        m_path.resize(m_path_stack.back());
        m_path_stack.pop_back();
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
    static constexpr std::size_t kIndentWidth = 2;   // columns per nesting level (a 2-space indent)
    static constexpr std::size_t kMaxIndent = 1024;  // clamp for DumpOptions::indent (see the ctor)
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
    const std::vector<std::string_view>*
        m_skip;               // caller's skip paths (DumpOptions::skip), or null
    std::size_t m_avail = 0;  // remaining width for the current compact probe
    std::size_t m_column;  // current column on the real output's line (set from the start indent)
    int m_indent;          // current nesting depth (starts at DumpOptions::indent)
    bool m_compact = false;
    std::string m_path;                     // current dotted path prefix (no trailing dot)
    std::vector<std::size_t> m_path_stack;  // saved m_path lengths, for pop_path()
};

// Options for the generated rp_dump_write / rp_dump_string. Backward-compatible: an old
// `rp_dump_string(m, 120)` still compiles -- an integer converts to a width-only DumpOptions.
struct DumpOptions {
    std::size_t width = Writer::kDefaultWidth;  // line-width budget (compact vs one-entry-per-line)
    std::size_t indent = 0;  // nesting level to start at (each level = 2 columns)
    // Qualified field paths to omit from the dump, e.g. {"people.email", "people.address.city"}. A path
    // names a message/container field to drop its whole subtree. Views must outlive the dump call.
    std::vector<std::string_view> skip;

    DumpOptions() = default;
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions): back-compat with `width`.
    DumpOptions(std::size_t width_) : width(width_) {}
};

}  // namespace rapidproto::dump
