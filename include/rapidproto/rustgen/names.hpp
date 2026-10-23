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
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"  // enum_value_prefix

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

// A schema-derived name that shares a namespace with the generator's own names never starts with `rp_`
// followed by anything but `_`: such a name gains a `_` after the prefix (`rp_rt` -> `rp__rt`). That
// shape belongs to the generator -- at module scope the stream modules' `rp_rt` / `rp_mod_*` /
// `rp_stream_ty_*` re-exports and `rp_mac_*` macros; in a message type its `rp_presence` /
// `rp_unknown` storage and `rp_decode_into` worker; and the decode body's locals -- so no schema name,
// nor anything derived from one (`<T>Visitor`, a dedup `_`, `<snake>_stream`), can equal one of them.
// (A trailing `_` would not do: glue names end in schema names.) A name already starting `rp__` keeps
// its spelling; if it meets a reserved `rp_x` -> `rp__x` in one scope, the scope's dedup separates them
// like any other collision.
inline std::string reserve_rp_prefix(std::string name) {
    if (name.rfind("rp_", 0) == 0 && name.rfind("rp__", 0) != 0) {
        name.insert(3, 1, '_');
    }
    return name;
}

// Rust's primitive type names. Not keywords -- a struct may be called `u8` -- but a schema TYPE of
// that name would shadow the primitive for every generated reference in its module (`&'a [u8]`,
// `u32::MAX`, an `enum i32` newtype wrapping itself), and a consumer's glob of a stream module would
// import it over the primitive a `*_stream!` row names at the call site.
inline bool is_rust_primitive(std::string_view s) {
    static const std::unordered_set<std::string_view> kPrimitives = {
        "bool", "char", "str", "i8",  "i16",  "i32",   "i64", "i128", "isize",
        "u8",   "u16",  "u32", "u64", "u128", "usize", "f32", "f64"};
    return kPrimitives.count(s) != 0;
}

// A schema-derived TYPE name (a message struct / stream handle, an enum newtype): keyword-escaped, a
// primitive name gaining a trailing `_` (no generated item is ever named like a primitive), and
// `rp_`-reserved. Not yet deduped.
inline std::string type_ident(std::string_view raw) {
    std::string name = escape_rust_ident(std::string(raw));
    if (is_rust_primitive(name)) {
        name += '_';
    }
    return reserve_rp_prefix(std::move(name));
}

// The identifier an escaped name denotes: the raw identifier `r#type` IS `type`. Dedup sets and
// derived names (`<T>Visitor`, `<T>Class`, `<snake>_stream`) work on this, never on the spelling.
inline std::string_view ident_identity(std::string_view name) {
    return name.rfind("r#", 0) == 0 ? name.substr(2) : name;
}

// Make the escaped identifier `name` unique within `taken` by IDENTITY -- `taken` holds identities,
// so `r#type` collides with a plain `type` -- appending `_` to the bare name until it is free, and
// record it. A suffixed name needs no re-escaping: no keyword ends in `_`. The Rust counterpart of
// codegen::dedup_suffix, which compares spellings and so would let `r#type_` and `type_` (one
// identifier) through.
inline std::string dedup_ident(std::unordered_set<std::string>& taken, const std::string& name) {
    std::string bare(ident_identity(name));
    if (taken.insert(bare).second) {
        return name;
    }
    bare += '_';
    while (!taken.insert(bare).second) {
        bare += '_';
    }
    return bare;
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
// oneof name (NOT yet deduped -- the caller dedups with dedup_ident). PascalCase, then a `V`
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
// allows, Rust-escaped, deduped within the enum (two names colliding gain trailing `_`) and against the
// `classify` method of the same `impl` (consts and methods share its value namespace). Kept in the
// value's as-written case (proto enum values are SCREAMING_SNAKE by convention, which is also Rust's
// convention for associated consts).
inline std::vector<std::string> enum_const_names(const EnumNode& node) {
    const std::string prefix = codegen::enum_value_prefix(node.name);
    const bool strip = rust_prefix_strippable(node, prefix);
    std::unordered_set<std::string> taken = {"classify"};
    std::vector<std::string> out;
    out.reserve(node.values.size());
    for (const auto& value : node.values) {
        std::string_view raw = value.name;
        if (strip) {
            raw.remove_prefix(prefix.size());
        }
        out.push_back(dedup_ident(taken, escape_rust_ident(std::string(raw))));
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
        out.push_back(dedup_ident(taken, pascal_ident(raw)));
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
        out.push_back(dedup_ident(taken, pascal_ident(field.name)));
    }
    return out;
}

}  // namespace rapidproto::rustgen
