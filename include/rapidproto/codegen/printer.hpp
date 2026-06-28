#pragma once

// A tiny code emitter shared by both C++ code generators: it accumulates output text,
// tracks indentation (re-applied at the start of every line), and substitutes `$name$`
// variables (protoc io::Printer style). `$$` emits a literal `$`. Variables come from
// per-call inline bindings (preferred) or persistent ones set via set(); an unresolved
// `$name$` is left verbatim so a generator bug is visible. Generator-internal — NOT part
// of the runtime that generated code includes.

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rapidproto::codegen {

class Printer {
public:
    using Binding = std::pair<std::string_view, std::string_view>;

    // Persistent variable, visible to every subsequent print().
    void set(std::string_view key, std::string_view value) {
        m_vars[std::string(key)] = std::string(value);
    }

    // Emit `text`, substituting `$name$` (inline bindings first, then persistent vars) and
    // re-applying the current indentation at the start of each line.
    void print(std::string_view text, std::initializer_list<Binding> inline_vars = {}) {
        emit(substitute(text, inline_vars));
    }

    void indent() noexcept { ++m_indent; }
    void outdent() noexcept {
        if (m_indent > 0) {
            --m_indent;
        }
    }

    [[nodiscard]] const std::string& str() const noexcept { return m_out; }

private:
    std::string substitute(std::string_view text,
                           std::initializer_list<Binding> inline_vars) const {
        std::string out;
        std::size_t i = 0;
        while (i < text.size()) {
            if (text[i] != '$') {
                out += text[i++];
                continue;
            }
            const std::size_t close = text.find('$', i + 1);
            if (close == std::string_view::npos) {
                out.append(text.substr(i));  // no closing '$' -> emit the rest literally
                break;
            }
            const std::string_view name = text.substr(i + 1, close - i - 1);
            i = close + 1;
            if (name.empty()) {
                out += '$';  // `$$` -> `$`
            } else if (const std::optional<std::string_view> value = lookup(name, inline_vars)) {
                out.append(*value);
            } else {
                out += '$';  // unresolved -> leave `$name$` visible
                out.append(name);
                out += '$';
            }
        }
        return out;
    }

    // Inline-binding and persistent-var values both outlive the print() call, so a view is safe.
    std::optional<std::string_view> lookup(std::string_view name,
                                           std::initializer_list<Binding> inline_vars) const {
        for (const auto& [key, value] : inline_vars) {
            if (key == name) {
                return value;
            }
        }
        if (const auto it = m_vars.find(name); it != m_vars.end()) {
            return std::string_view(it->second);
        }
        return std::nullopt;
    }

    void emit(const std::string& resolved) {
        for (const char ch : resolved) {
            if (ch == '\n') {
                m_out += '\n';
                m_at_line_start = true;
                continue;
            }
            if (m_at_line_start) {
                for (int level = 0; level < m_indent; ++level) {
                    m_out.append(kIndent);
                }
                m_at_line_start = false;
            }
            m_out += ch;
        }
    }

    static constexpr std::string_view kIndent = "  ";
    std::string m_out;
    std::map<std::string, std::string, std::less<>> m_vars;
    int m_indent = 0;
    bool m_at_line_start = true;
};

}  // namespace rapidproto::codegen
