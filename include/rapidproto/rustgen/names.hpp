// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Rust identifier naming for the Rust emitter: keyword-safe identifiers, case conversion, and the
// per-enum const/variant names. The prefix-detection (enum_value_prefix) is reused from the shared
// codegen layer; everything escaping-related is Rust-specific (Rust's keyword set and `r#` raw
// identifiers differ from C++'s), so it lives here rather than in codegen/naming.hpp.

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"    // enum_value_prefix
#include "rapidproto/codegen/naming.hpp"  // dedup_suffix

namespace rapidproto::rustgen {

// Rust strict + reserved keywords (2015/2018/2021/2024). A proto identifier equal to one of these
// is not a legal bare Rust identifier.
inline bool is_rust_keyword(std::string_view s) {
    static const std::unordered_set<std::string_view> kKeywords = {
        // strict
        "as", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern", "false", "fn",
        "for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref",
        "return", "self", "Self", "static", "struct", "super", "trait", "true", "type", "unsafe",
        "use", "where", "while", "async", "await",
        // reserved (2015+) and 2024's `gen`
        "abstract", "become", "box", "do", "final", "gen", "macro", "override", "priv", "typeof",
        "unsized", "virtual", "yield", "try"};
    return kKeywords.count(s) != 0;
}

// `self`, `Self`, `super`, `crate` cannot be written as raw identifiers (`r#self` is rejected), so
// a collision with one is escaped with a trailing `_` instead of `r#`.
inline bool is_raw_ineligible(std::string_view s) {
    return s == "self" || s == "Self" || s == "super" || s == "crate";
}

// A proto identifier -> a legal Rust identifier: a keyword becomes `r#kw` (or `kw_` for the four
// that cannot be raw); `_` alone becomes `_x` (a bare `_` is a wildcard, not an identifier);
// anything else is unchanged. Case is the caller's responsibility (apply it before escaping).
inline std::string escape_rust_ident(std::string name) {
    if (name == "_") {
        return "_x";
    }
    if (is_raw_ineligible(name)) {
        return name + "_";
    }
    if (is_rust_keyword(name)) {
        return "r#" + name;
    }
    return name;
}

// A MODULE-SCOPE name derived from the schema (a package module, a message struct or submodule, an
// enum newtype, a stream macro) never starts with `rp_` followed by anything but `_`: such a name
// gains a `_` after the prefix (`rp_rt` -> `rp__rt`). That shape belongs to the generator's own
// module-level glue -- the stream modules' `rp_rt` / `rp_mod_*` / `rp_stream_ty_*` re-exports and
// `rp_mac_*` macros -- so no schema name, nor anything derived from one (`<T>Visitor`, a dedup `_`,
// `<snake>_stream`), can equal glue. (A trailing `_` would not do: glue names end in schema names.)
// A name already starting `rp__` keeps its spelling; if it meets a reserved `rp_x` -> `rp__x` in
// one scope, the scope's dedup separates them like any other collision. Member names (fields,
// accessors, consts) share no namespace with module items and keep their spelling.
inline std::string reserve_rp_prefix(std::string name) {
    if (name.rfind("rp_", 0) == 0 && name.rfind("rp__", 0) != 0) {
        name.insert(3, 1, '_');
    }
    return name;
}

// `SCREAMING_SNAKE` or `snake` or anything_ -> `PascalCase`, splitting on `_` and digits-as-words
// boundaries kept simple: each `_`-separated run is lower-cased then title-cased. Used for enum
// classify-variant names (Rust enum variants are UpperCamelCase; a non-camel name warns).
inline std::string to_pascal_case(std::string_view s) {
    std::string out;
    bool start_word = true;
    for (const char c : s) {
        if (c == '_') {
            start_word = true;
            continue;
        }
        if (start_word) {
            out += (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
            start_word = false;
        } else {
            out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }
    }
    return out;
}

// A proto name -> a legal Rust UpperCamelCase identifier, used for every synthesized enum-variant and
// oneof name (NOT yet deduped -- the caller dedups with dedup_suffix). PascalCase, then a `V`
// prepended when that leaves an empty or digit-headed name (an all-underscore `_`/`__` yields "", a
// `_2D`-style name a digit head -- neither a valid identifier head), then keyword-escaped.
inline std::string pascal_ident(std::string_view raw) {
    std::string name = to_pascal_case(raw);
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
        name.insert(name.begin(), 'V');
    }
    return escape_rust_ident(name);
}

// `PascalCase`/`camelCase`/`ALLCAPS` -> `snake_case`, for the per-message module a message's nested
// types live in. A `_` is inserted before an uppercase letter that follows a lowercase/digit, or
// that begins a word after an acronym run (`HTTPServer` -> `http_server`, `MyMsg` -> `my_msg`).
inline std::string to_snake_case(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            const bool prev_lower_or_digit = i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') ||
                                                       (s[i - 1] >= '0' && s[i - 1] <= '9'));
            const bool acronym_boundary = i > 0 && i + 1 < s.size() && s[i - 1] >= 'A' &&
                                          s[i - 1] <= 'Z' && s[i + 1] >= 'a' && s[i + 1] <= 'z';
            if (prev_lower_or_digit || acronym_boundary) {
                out += '_';
            }
            out += static_cast<char>(c - ('A' - 'a'));
        } else {
            out += c;
        }
    }
    return out;
}

// Whether the enum's conventional `<ENUMNAME>_` prefix can be stripped from every value: all values
// carry it and the remainder is a usable identifier head (non-empty, not a leading digit). Rust can
// escape a bare keyword with `r#`, so -- unlike the C++ rule -- a keyword remainder does NOT disable
// stripping. All-or-nothing per enum, so a conventionally-named enum reads `Color::RED`.
inline bool rust_prefix_strippable(const EnumNode& node, std::string_view prefix) {
    if (node.values.empty()) {
        return false;
    }
    return std::all_of(node.values.begin(), node.values.end(), [&](const EnumValueNode& value) {
        const std::string_view name = value.name;
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        const char head = name[prefix.size()];
        return head < '0' || head > '9';  // a leading-digit remainder is not a valid identifier
    });
}

// The newtype const name for each value, in declaration order: prefix stripped where the enum
// allows, Rust-escaped, deduped within the enum (two names colliding gain trailing `_`). Kept in
// the value's as-written case (proto enum values are SCREAMING_SNAKE by convention, which is also
// Rust's convention for associated consts).
inline std::vector<std::string> enum_const_names(const EnumNode& node) {
    const std::string prefix = codegen::enum_value_prefix(node.name);
    const bool strip = rust_prefix_strippable(node, prefix);
    std::unordered_set<std::string> taken;
    std::vector<std::string> out;
    out.reserve(node.values.size());
    for (const auto& value : node.values) {
        std::string_view raw = value.name;
        if (strip) {
            raw.remove_prefix(prefix.size());
        }
        out.push_back(codegen::dedup_suffix(taken, escape_rust_ident(std::string(raw))));
    }
    return out;
}

// The classify-enum variant name for each value, in declaration order: PascalCase of the (prefix-
// stripped) value, Rust-escaped, deduped within the enum AND against the reserved `Unknown` variant
// the classify enum always carries (a proto `UNKNOWN` value must not shadow it).
inline std::vector<std::string> enum_variant_names(const EnumNode& node) {
    const std::string prefix = codegen::enum_value_prefix(node.name);
    const bool strip = rust_prefix_strippable(node, prefix);
    std::unordered_set<std::string> taken;
    taken.insert("Unknown");  // the reserved `Unknown(i32)` variant
    std::vector<std::string> out;
    out.reserve(node.values.size());
    for (const auto& value : node.values) {
        std::string_view raw = value.name;
        if (strip) {
            raw.remove_prefix(prefix.size());
        }
        out.push_back(codegen::dedup_suffix(taken, pascal_ident(raw)));
    }
    return out;
}

// The variant name for each field of a oneof, in declaration order: PascalCase of the field name,
// escaped and deduped within the oneof (two fields that PascalCase alike -- e.g. `my_x`/`myX` -- get
// a trailing `_`). No prefix stripping (a oneof has no enum-style value prefix) and no reserved seed
// (a oneof enum is closed, with no synthesized `Unknown` variant).
inline std::vector<std::string> oneof_variant_names(const OneofNode& oneof) {
    std::unordered_set<std::string> taken;
    std::vector<std::string> out;
    out.reserve(oneof.fields.size());
    for (const FieldNode& field : oneof.fields) {
        out.push_back(codegen::dedup_suffix(taken, pascal_ident(field.name)));
    }
    return out;
}

}  // namespace rapidproto::rustgen
