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
// An ARRAY gets one tier in between: before falling back to one entry per line it tries a GRID of
// aligned columns (see try_grid), which keeps a long list of scalars readable in a few lines.
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
    // While collecting grid cells the budget is instead the collection cap: one cell is small, but a
    // huge array would otherwise materialize every element before we learn the grid can't work.
    bool overflowed() noexcept {
        if (m_collecting) {
            return grid_capped();
        }
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
            // Collecting grid cells: an entry boundary CLOSES the cell being built instead of writing
            // a separator (the grid renderer supplies its own). The final cell is closed by try_grid
            // once body() returns.
            if (m_collect) {
                if (!first) {
                    push_cell();
                }
                first = false;
                return;
            }
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
            // A nested group's own entries are not cells of the array being collected -- suspend
            // collection so only the collecting body's entry_sep calls close a cell.
            const bool outer_collect = m_collect;
            m_collect = false;
            *m_sink << open_ch;
            body();
            *m_sink << close_ch;
            m_collect = outer_collect;
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
        // An ARRAY gets one more chance before one-entry-per-line: lay the entries out in as many
        // aligned columns as fit. An OBJECT never grids -- packing its OWN `"key": value` entries
        // into columns reads worse than one per line (aligning on the colon would be a different
        // feature). An array whose ELEMENTS are objects does still grid: each element is one
        // complete, self-contained cell, which is exactly what a column should hold.
        if (open_ch == '[' && any && try_grid(body)) {
            return;
        }
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
    // ── grid layout (arrays only) ────────────────────────────────────────────────────────────────
    // When an array is too wide for one line, render it as aligned COLUMNS rather than one entry per
    // line: collect each entry as its own cell (entry_sep marks the boundaries), then pick the widest
    // column count that fits. Falls back to one-per-line whenever a grid can't work -- a multi-line
    // cell, an array past the collection cap, or not even two columns fitting.

    // Has collection hit a cap? Counts the cell still being built, so no single cell can grow
    // unbounded either. Both the generated loops (to bail early) and try_grid's post-check consult
    // this, and it only ever grows -- so a loop that bailed is guaranteed to be rejected afterwards,
    // and a truncated cell set can never reach the renderer.
    bool grid_capped() noexcept {
        return m_cells.size() > kMaxGridCells ||
               m_cell_bytes + static_cast<std::size_t>(m_buf.tellp()) > kMaxGridBytes;
    }

    // Close the cell currently in the scratch buffer and start a new one.
    void push_cell() {
        std::string cell = m_buf.str();
        m_cell_bytes += cell.size();
        m_cells.push_back(std::move(cell));
        m_buf.str(std::string());
        m_buf.clear();
    }

    // Does a cell look like a number? Numeric columns are RIGHT-aligned so digits line up by place
    // value; anything else (strings, bools, nested objects) is left-aligned.
    static bool numeric_cell(std::string_view s) noexcept {
        if (s.empty() || (s.front() != '-' && (s.front() < '0' || s.front() > '9'))) {
            return false;
        }
        // Covers ints, decimals and scientific notation; a bool or a quoted string fails the leading
        // check above. Character-set based so this header stays free of <algorithm>.
        return s.find_first_not_of("0123456789+-.eE") == std::string_view::npos;
    }

    // Collect the array's entries, then render them as a grid. Returns false (having emitted nothing)
    // if a grid isn't possible, leaving the caller to fall back -- safe because `body` is re-runnable.
    template <class Body>
    bool try_grid(const Body& body) {
        m_cells.clear();
        m_cell_bytes = 0;
        m_buf.str(std::string());
        m_buf.clear();
        m_sink = &m_buf;
        m_compact = true;
        m_collect = true;
        m_collecting = true;  // unlike m_collect, stays set inside nested groups: while collecting,
                              // overflowed() means "capped", so no cell is ever truncated on width
        body();
        m_collecting = false;
        m_collect = false;
        m_compact = false;
        m_sink = &m_out;
        push_cell();  // body() returning ends the last entry
        // A cap stopped the collection mid-array, so the cells are incomplete and rendering them
        // would silently drop elements.
        if (grid_capped()) {
            return false;
        }
        bool all_numeric = true;
        for (const std::string& cell : m_cells) {
            // Values are escaped (a newline becomes \n) and nested groups render compact here, so a
            // cell should never span lines -- but a grid of multi-line cells would be unreadable, so
            // verify rather than assume.
            if (cell.find('\n') != std::string::npos) {
                return false;
            }
            all_numeric = all_numeric && numeric_cell(cell);
        }
        const std::size_t n = m_cells.size();
        const std::size_t inner = static_cast<std::size_t>(m_indent + 1) * kIndentWidth;
        // A column costs its widest cell plus that cell's ',', and columns are separated by one
        // further space -- so each costs at least kCellPunctWidth + 1, capping how many could fit.
        std::size_t cols = (m_width > inner ? m_width - inner + 1 : 0) / (kCellPunctWidth + 1);
        cols = cols < n ? cols : n;
        std::vector<std::size_t> widths;
        for (; cols >= kMinGridColumns; --cols) {
            widths.assign(cols, 0);
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t cell_w = m_cells[i].size() + kCellPunctWidth;
                std::size_t& w = widths[i % cols];
                w = cell_w > w ? cell_w : w;
            }
            std::size_t total = inner + (cols - 1);  // the space following each column's comma
            for (const std::size_t w : widths) {
                total += w;
            }
            if (total <= m_width) {
                break;
            }
        }
        if (cols < kMinGridColumns) {
            return false;  // not even two columns fit -> one entry per line reads better
        }
        render_grid(cols, widths, all_numeric);
        return true;
    }

    // Emit the collected cells as `cols` aligned columns, row-major so index order still reads
    // left-to-right. Padding is only ever written BEFORE a cell, so no line gains trailing spaces.
    void render_grid(std::size_t cols, const std::vector<std::size_t>& widths, bool all_numeric) {
        const std::size_t n = m_cells.size();
        m_out << '[';
        ++m_indent;
        const std::size_t inner = static_cast<std::size_t>(m_indent) * kIndentWidth;
        std::size_t pos = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t col = i % cols;
            if (col == 0) {
                newline();
                pos = inner;
            }
            std::size_t start = inner;
            for (std::size_t k = 0; k < col; ++k) {
                start += widths[k] + 1;
            }
            for (; pos < start; ++pos) {
                m_out << ' ';
            }
            const std::string& cell = m_cells[i];
            if (all_numeric) {
                // Right-align on width+comma uniformly, so the final cell (which has no comma) still
                // lines its digits up with the column above it.
                for (std::size_t s = cell.size() + kCellPunctWidth; s < widths[col]; ++s) {
                    m_out << ' ';
                    ++pos;
                }
            }
            m_out << cell;
            pos += cell.size();
            if (i + 1 < n) {
                m_out << ',';
                ++pos;
            }
        }
        --m_indent;
        newline();
        m_out << ']';
        m_column = static_cast<std::size_t>(m_indent) * kIndentWidth + 1;  // +1: the close bracket
        m_cells.clear();
        m_cells.shrink_to_fit();  // don't retain a large array's cells for the Writer's lifetime
    }

    static constexpr std::size_t kIndentWidth = 2;   // columns per nesting level (a 2-space indent)
    static constexpr std::size_t kMaxIndent = 1024;  // clamp for DumpOptions::indent (see the ctor)
    static constexpr std::size_t kCellPunctWidth = 1;  // a grid cell's trailing ','
    static constexpr std::size_t kMinGridColumns =
        2;  // below this, one entry per line reads better
    static constexpr std::size_t kMaxGridCells =
        4096;  // collection caps -- an array past either of
    static constexpr std::size_t kMaxGridBytes =
        std::size_t{64} * 1024;                       // these stays one-per-line
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
    bool m_collect = false;     // at a cell boundary: entry_sep closes a cell instead of separating
    bool m_collecting = false;  // anywhere inside a grid collection, nested groups included
    std::string m_path;         // current dotted path prefix (no trailing dot)
    std::vector<std::size_t> m_path_stack;  // saved m_path lengths, for pop_path()
    std::vector<std::string> m_cells;       // the array entries collected for a grid
    std::size_t m_cell_bytes = 0;           // their total size, against kMaxGridBytes
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
