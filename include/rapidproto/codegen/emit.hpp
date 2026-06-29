// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Shared C++ emission helpers used by both decoder emitters (streamgen + arenagen), so a construct emitted
// the same way by each lives in one place.

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/codegen/wire.hpp"

namespace rapidproto::codegen {

// The conventional SCREAMING_SNAKE_CASE prefix that proto enum values carry by Google/buf style: the
// enum name in SCREAMING_SNAKE plus '_' (e.g. enum `HttpStatus` -> "HTTP_STATUS_"). PascalCase/camelCase
// is split on case boundaries, acronym-aware. A name that doesn't match this convention simply won't
// prefix the values, so the strip below is a no-op (safe).
inline std::string enum_value_prefix(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c >= 'A' && c <= 'Z' && !out.empty()) {
            const char prev = name[i - 1];
            const bool after_lower_or_digit =
                (prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9');
            const bool acronym_boundary = prev >= 'A' && prev <= 'Z' && i + 1 < name.size() &&
                                          name[i + 1] >= 'a' && name[i + 1] <= 'z';
            if (after_lower_or_digit || acronym_boundary) {
                out += '_';
            }
        }
        out += (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    }
    out += '_';
    return out;
}

// True iff EVERY value carries `prefix` and its bare remainder is a *clean* identifier: a valid
// identifier head (non-empty, not a digit) that sanitize() leaves unchanged -- i.e. not a keyword, a
// reserved/generated name, an `rp_` name, or a guarded macro. That last guard matters because stripping
// yields a BARE name: `STATUS_EOF`/`STATUS_OR` compile, but bare `EOF` (a macro) / `or` (a keyword) do
// not. If any value fails, the whole enum keeps its (always-safe) full prefixed names.
inline bool enum_prefix_strippable(const EnumNode& node, std::string_view prefix) {
    if (node.values.empty()) {
        return false;
    }
    return std::all_of(node.values.begin(), node.values.end(), [&](const EnumValueNode& value) {
        const std::string_view name = value.name;
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        const std::string_view rest = name.substr(prefix.size());
        if (rest.front() >= '0' && rest.front() <= '9') {
            return false;  // a leading-digit remainder is not a valid identifier
        }
        return sanitize(rest) ==
               rest;  // clean: sanitize would not escape it (no keyword/reserved/macro)
    });
}

// Emit `enum class <Name> : std::int32_t { <values>; rp_non_exhaustive_{min,max} };` for `node`.
// When every value carries the conventional `<ENUMNAME>_` prefix and every bare remainder is a clean
// identifier (see enum_prefix_strippable -- not a keyword/reserved name/macro), the prefix is stripped
// (all-or-nothing per enum) so a conventionally-named enum reads `Status::OK`, not the redundant
// `Status::STATUS_OK`; a numeric-suffix value (`VERSION_2` -> `2`), a value missing the prefix, or one
// whose remainder would macro-expand (`STATUS_EOF` -> `EOF`) disables stripping for the whole enum. Enumerators are then deduped (two
// proto names that sanitize alike must not redefine one), and the full int32 range is reserved via the
// rp_non_exhaustive_{min,max} sentinels so a switch over the enum must carry a `default:` -- enums are
// OPEN, unknown wire values are representable. Stripping touches only this C++ identifier: the wire is
// by number, and an enum default resolves to its number, so nothing else references the value name.
// `trailing_blank` appends the blank line streamgen places between declarations (arenagen does not),
// keeping each generator's output byte-identical.
inline void emit_enum(Printer& printer, const CppNameTable& names, const EnumNode& node,
                      bool trailing_blank) {
    printer.print("enum class $E$ : std::int32_t {\n", {{"E", names.local.at(&node)}});
    printer.indent();
    const std::string prefix = enum_value_prefix(node.name);
    const bool strip = enum_prefix_strippable(node, prefix);
    std::unordered_set<std::string> taken = {"rp_non_exhaustive_min", "rp_non_exhaustive_max"};
    for (const auto& value : node.values) {
        std::string_view raw = value.name;
        if (strip) {
            raw.remove_prefix(prefix.size());
        }
        std::string name = sanitize(raw);
        while (!taken.insert(name).second) {
            name += '_';
        }
        printer.print("$name$ = $n$,\n", {{"name", name}, {"n", std::to_string(value.number)}});
    }
    printer.print("rp_non_exhaustive_min = INT32_MIN,\n");
    printer.print("rp_non_exhaustive_max = INT32_MAX,\n");
    printer.outdent();
    printer.print(trailing_blank ? "};\n\n" : "};\n");
}

// Emit the shared "common header" for `file`: `#pragma once`, `<cstdint>`, an include of each
// (non-option) import's common header, the package namespace, and one `enum class` per TOP-LEVEL enum
// (via emit_enum). This is the single home for the schema's enums: both the streaming and arena decoder
// headers `#include` it -- so the same proto enum is ONE C++ type shared across the two models, instead
// of each decoder defining the enums and colliding when included together. Nested enums are NOT here;
// they ride with their message in each decoder. The import includes use the fixed ".rp.common.hpp"
// suffix (the common header's own name).
inline std::string emit_common_header(const FileNode& file, const CppNameTable& names) {
    Printer printer;
    printer.print("// Generated by rapidprotoc. DO NOT EDIT.\n");
    printer.print("// Shared schema types (enums) for the generated decoders (Apache-2.0).\n");
    printer.print("#pragma once\n\n");
    printer.print("#include <cstdint>\n");
    for (const auto& import : file.imports) {
        if (import.kind != ImportKind::Option) {
            printer.print("#include \"$h$\"\n",
                          {{"h", import_header(import.path, ".rp.common.hpp")}});
        }
    }
    printer.print("\n");
    const std::string ns = join_ns(names.ns_prefix, namespace_of(file.package));
    if (!ns.empty()) {
        printer.print("namespace $ns$ {\n\n", {{"ns", ns}});
    }
    for (const auto& node :
         file.enums) {  // top-level enums only; nested enums ride with the message
        emit_enum(printer, names, node, /*trailing_blank=*/true);
    }
    if (!ns.empty()) {
        printer.print("}  // namespace $ns$\n", {{"ns", ns}});
    }
    return printer.str();
}

}  // namespace rapidproto::codegen
