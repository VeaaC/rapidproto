// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Shared C++ emission helpers used by both decoder emitters (streamgen + arenagen), so a construct emitted
// the same way by each lives in one place.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/codegen/wire.hpp"
#include "rapidproto/runtime.hpp"  // WireType, for composing 2-byte tag bytes at codegen time
#include "rapidproto/version.hpp"

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

// Emit `enum class <Name> : std::int32_t { <values>; rp_known_{min,max};
// rp_non_exhaustive_{min,max} };` for `node`.
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
    std::unordered_set<std::string> taken = {"rp_known_min", "rp_known_max",
                                             "rp_non_exhaustive_min", "rp_non_exhaustive_max"};
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
    // The DECLARED value range (aliases collapse; unrelated to the INT32 sentinels below): the
    // schema-known bounds a consumer can range-check or size against without hand-tracking the
    // schema. Emitted only when the enum has values (an empty enum is invalid input anyway).
    if (!node.values.empty()) {
        const auto [lo, hi] = std::minmax_element(
            node.values.begin(), node.values.end(),
            [](const EnumValueNode& a, const EnumValueNode& b) { return a.number < b.number; });
        printer.print("rp_known_min = $n$,\n", {{"n", std::to_string(lo->number)}});
        printer.print("rp_known_max = $n$,\n", {{"n", std::to_string(hi->number)}});
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
    printer.print("// Generated by rapidprotoc $v$. DO NOT EDIT.\n", {{"v", kVersion}});
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

// The compile-time dispatch misuse guards both emitters generate, identical up to the name of the
// callback template-parameter pack (`pack`: streamgen's decode() uses "Callbacks", the arena oneof
// reader "RpFs"). `args` is the trait argument list after the pack ("$f$, $f$::Value" for a field,
// with Key inserted for a map); `what` names the case for diagnostics; `expected` describes the
// expected value type(s). Four guards: at most one specific handler (duplicates are an error); at
// most one catch-all; no partially-generic callback; and a callback that NAMES this case must
// handle it exactly (per-callback, so a catch-all sibling cannot mask a mistyped callback).
inline void emit_dispatch_guards(Printer& printer, const std::string& pack, const std::string& args,
                                 const std::string& what, const std::string& expected) {
    printer.print(
        "static_assert((0U + ... + static_cast<unsigned>("
        "::rapidproto::specifically_handles<$P$, $A$>)) <= 1U,"
        " \"$what$ is handled by more than one callback\");\n",
        {{"P", pack}, {"A", args}, {"what", what}});
    printer.print(
        "static_assert((0U + ... + static_cast<unsigned>("
        "::rapidproto::is_catch_all<$P$, $A$>)) <= 1U,"
        " \"$what$ is matched by more than one catch-all callback\");\n",
        {{"P", pack}, {"A", args}, {"what", what}});
    if (expected.empty()) {
        // A VALUELESS case (the oneof unset state, args = just the tag): no partially-generic /
        // wrong-value-type notion, but a callback naming the tag with extra parameters would
        // silently never fire -- the same names-it-must-handle-it rule, phrased for the bare tag.
        printer.print(
            "static_assert((true && ... && !(::rapidproto::targets<$P$, $A$>"
            " && !::rapidproto::specifically_handles<$P$, $A$>)),"
            " \"a callback for $what$ must take exactly ($A$)\");\n",
            {{"P", pack}, {"A", args}, {"what", what}});
        return;
    }
    printer.print(
        "static_assert((true && ... && !::rapidproto::is_partial_generic<$P$, $A$>),"
        " \"a callback for $what$ is partially generic; use a concrete (Tag, Value) callback or a"
        " fully generic (auto, auto) catch-all\");\n",
        {{"P", pack}, {"A", args}, {"what", what}});
    printer.print(
        "static_assert((true && ... && !(::rapidproto::targets<$P$, $A$>"
        " && !::rapidproto::specifically_handles<$P$, $A$>)),"
        " \"a callback for $what$ has the wrong value type (expected $expected$)\");\n",
        {{"P", pack}, {"A", args}, {"what", what}, {"expected", expected}});
}

// Order sibling messages so every type that must be visible before a sibling's definition is
// emitted first: post-order DFS over the "B must precede A" edges `depends_on(a, b)` answers.
// Acyclic for valid schemas; the active-set guard breaks any cycle, which only an
// inherently-uncompilable schema can form (two siblings each naming a type nested in the other).
// Iterative (a frame is {node, next candidate to scan}): the dependency-chain length is unbounded
// in a protoc-valid schema, so recursing per edge could overflow the native stack.
template <class DependsOn>
std::vector<const MessageNode*> topo_order_siblings(const std::vector<MessageNode>& siblings,
                                                    const DependsOn& depends_on) {
    std::vector<const MessageNode*> order;
    std::vector<bool> done(siblings.size(), false);
    std::vector<bool> active(siblings.size(), false);
    struct Frame {
        std::size_t node;
        std::size_t next;
    };
    std::vector<Frame> stack;
    for (std::size_t root = 0; root < siblings.size(); ++root) {
        if (done[root]) {
            continue;
        }
        active[root] = true;
        stack.push_back({root, 0});
        while (!stack.empty()) {
            const std::size_t i = stack.back().node;
            std::size_t child = siblings.size();
            while (stack.back().next < siblings.size()) {
                const std::size_t j = stack.back().next++;
                if (j != i && !done[j] && !active[j] && depends_on(i, j)) {
                    child = j;
                    break;
                }
            }
            if (child != siblings.size()) {
                active[child] = true;
                stack.push_back({child, 0});
            } else {
                active[i] = false;
                done[i] = true;
                order.push_back(&siblings[i]);
                stack.pop_back();
            }
        }
    }
    return order;
}

// ---------------------------------------------------------------------------
// Field-order threading: the shared decode-loop SHAPE.
//
// Both decoder emitters (arenagen + streamgen) build the same threaded wire loop: a hub `switch(*rp_c)`
// that peeks the 1-byte tag and jumps to a tag-consumed label `rp_do_<n>`, one label per threaded
// field (each decoding one value, then a depth-2 constant-tag successor probe / repeated self-loop),
// and a wire-guarded `goto` from the general `switch(field_number)` for non-minimal / 2-byte tags.
// Only the per-field label BODY and the loop scaffolding (return type, maps/oneofs, setup/finalize)
// differ between the two -- the SHAPE (hub, labels, probes, general-case routing) is identical. That
// identical part lives here; the caller supplies the body via hooks and drives its own general switch.
// This layer has NO knowledge of arena vs streaming, MemberPlan vs FieldGen, or return types.
// ---------------------------------------------------------------------------

// A field number whose whole tag fits in a single varint byte -- number 1..15 ((15 << 3) | 7 == 127 <
// 128). These appear in the 1-byte-peek hub.
inline constexpr std::int32_t kMaxOneByteTagField = 15;
// Field numbers 16..2047 carry a 2-byte tag (read_tag_or_end's fused 2-byte path). Above that, 3+
// bytes -- never threaded.
inline constexpr std::int32_t kMaxTwoByteTagField = 2047;

// Wire-tag varint encoding constants (see raw_tag / the base-128 varint format), for composing a
// 2-byte tag's two bytes at codegen time: tag = (field_number << kTagFieldShift) | wire.
inline constexpr int kTagFieldShift = 3;      // low 3 bits of a tag are the wire type
inline constexpr int kVarintShift = 7;        // each varint byte carries 7 payload bits
inline constexpr int kVarintPayload = 0x7F;   // low 7 bits of a varint byte
inline constexpr int kVarintContinue = 0x80;  // high bit set == "more bytes follow"

// The numeric WireType value for a wire enumerator name (to compose a raw 2-byte tag's bytes). Uses
// the runtime enum's own values, so it can never drift from WireType.
inline int wire_enum_num(const std::string& w) {
    if (w == "I64") {
        return static_cast<int>(::rapidproto::WireType::I64);
    }
    if (w == "Len") {
        return static_cast<int>(::rapidproto::WireType::Len);
    }
    if (w == "I32") {
        return static_cast<int>(::rapidproto::WireType::I32);
    }
    // Varint (and any non-2-byte-threaded wire, unused here)
    return static_cast<int>(::rapidproto::WireType::Varint);
}

// A threaded field, generator-agnostic: the shape generator needs only the routing facts (number,
// repeated-ness, whether a packed LEN label is also emitted, and the tag it threads on). All value
// emission is the caller's, via ThreadedLoopHooks.
struct ThreadField {
    int number;
    bool repeated;
    bool packable;            // repeated packable => also a rp_do_<n>_p packed label
    std::string thread_wire;  // WireType enumerator: singular field's canonical wire, or repeated
                              // element wire
};

// Hooks the caller supplies for the per-field label bodies. Each emits at the current indent,
// TAG-CONSUMED (the tag has already been consumed on entry to the label), with NO `case`, NO probe,
// NO `continue` -- the shape generator wraps those around the body.
struct ThreadedLoopHooks {
    std::function<void(const ThreadField&)>
        emit_body;  // rp_do_<n>: singular value / repeated native element
    std::function<void(const ThreadField&)>
        emit_packed_body;  // rp_do_<n>_p: packed fill (only called if packable)
};

namespace detail {

// The depth-2 constant-tag successor probes emitted at the tail of a threaded label: from field i,
// try the next / next-but-one threaded field's THREAD tag, consuming the tag bytes before the goto
// (labels are tag-consumed). 1-byte successor: compare the single tag byte, `++rp_c`. 2-byte
// successor: compare both tag bytes, `rp_c += 2`. Ascending order puts the 1-byte fields first, so
// the cheaper 1-byte probes carry the hot run.
inline void emit_thread_probes(Printer& p, const std::vector<ThreadField>& threaded,
                               std::size_t i) {
    for (std::size_t d = 1; d <= 2 && i + d < threaded.size(); ++d) {
        const ThreadField& s = threaded[i + d];
        const std::string n = std::to_string(s.number);
        if (s.number <= kMaxOneByteTagField) {
            p.print(
                "if (rp_c < rp_cend && *rp_c == ::rapidproto::raw_tag($n$,"
                " ::rapidproto::WireType::$w$)) { ++rp_c; goto rp_do_$n$; }\n",
                {{"n", n}, {"w", s.thread_wire}});
        } else {
            const int v = (s.number << kTagFieldShift) | wire_enum_num(s.thread_wire);
            p.print(
                "if (rp_c + 1 < rp_cend && rp_c[0] == $b0$ && rp_c[1] == $b1$)"
                " { rp_c += 2; goto rp_do_$n$; }\n",
                {{"n", n},
                 {"b0", std::to_string((v & kVarintPayload) | kVarintContinue)},
                 {"b1", std::to_string(v >> kVarintShift)}});
        }
    }
}

}  // namespace detail

// Emit the hub `switch(*rp_c)` and the tag-consumed labels for `threaded` (declaration order,
// ascending by number so probes thread that order). Emits, at the current indent:
//   * `if (rp_c >= rp_cend) { <on_end> }`  -- the caller's end action (arena: "break;");
//   * the hub `switch(*rp_c)`: each 1-byte-tag threaded field -> `case raw_tag(n,W): ++rp_c; goto
//     rp_do_n;` (a repeated packable field also gets a Len case -> rp_do_n_p); `default: break;`;
//   * `goto rp_field_general;`;
//   * one tag-consumed label per threaded field: singular -> body + probes + continue; repeated
//     native -> body + element self-loop + probes + continue; packed (rp_do_n_p) -> packed body +
//     probes + continue;
//   * `rp_field_general:;`.
// When `threaded` is empty, emits nothing (the caller's general path stands alone).
inline void emit_hub_and_labels(Printer& p, const std::vector<ThreadField>& threaded,
                                const ThreadedLoopHooks& hooks, const std::string& on_end) {
    if (threaded.empty()) {
        return;
    }
    // Hub: a 1-byte peek switch. Only 1-byte-tag threaded fields appear (a 2-byte-tag field enters
    // via the general path). Each case consumes the peeked byte, then jumps to the tag-consumed
    // label. A miss (multi-byte tag, unknown field, wrong wire type, or a non-minimal encoding of a
    // threaded field's tag) falls to the general path.
    p.print("if (rp_c >= rp_cend) { $e$ }\n", {{"e", on_end}});
    p.print("switch (*rp_c) {  // peek the 1-byte tag; threaded fields jump to their label\n");
    p.indent();
    for (const ThreadField& tf : threaded) {
        if (tf.number > kMaxOneByteTagField) {
            continue;  // 2-byte tag: entered via the general path / a 2-byte probe, not the hub
        }
        const std::string n = std::to_string(tf.number);
        p.print(
            "case ::rapidproto::raw_tag($n$, ::rapidproto::WireType::$w$): ++rp_c; goto "
            "rp_do_$n$;\n",
            {{"n", n}, {"w", tf.thread_wire}});
        if (tf.repeated && tf.packable) {
            p.print(
                "case ::rapidproto::raw_tag($n$, ::rapidproto::WireType::Len): ++rp_c; "
                "goto "
                "rp_do_$n$_p;\n",
                {{"n", n}});
        }
    }
    p.print("default: break;\n");
    p.outdent();
    p.print("}\n");
    p.print("goto rp_field_general;\n");
    // Tag-consumed labels, one per threaded field (declaration order). Each decodes its value, then
    // runs the successor probe (and, for repeated, a self-loop) before falling to `continue`.
    for (std::size_t i = 0; i < threaded.size(); ++i) {
        const ThreadField& tf = threaded[i];
        const std::string n = std::to_string(tf.number);
        if (!tf.repeated) {
            p.print("rp_do_$n$: {\n", {{"n", n}});
            p.indent();
            hooks.emit_body(tf);
            detail::emit_thread_probes(p, threaded, i);
            p.print("continue;\n");
            p.outdent();
            p.print("}\n");
            continue;
        }
        // Repeated: the native-element label self-loops on its own element tag (consuming it), then
        // probes successors. The packed form gets its own tag-consumed label rp_do_<n>_p.
        p.print("rp_do_$n$: {\n", {{"n", n}});
        p.indent();
        hooks.emit_body(tf);
        p.print(
            "if (rp_c < rp_cend && *rp_c == ::rapidproto::raw_tag($n$, "
            "::rapidproto::WireType::$w$"
            ")) { ++rp_c; goto rp_do_$n$; }  // another element of the same field\n",
            {{"n", n}, {"w", tf.thread_wire}});
        detail::emit_thread_probes(p, threaded, i);
        p.print("continue;\n");
        p.outdent();
        p.print("}\n");
        if (tf.packable) {
            p.print("rp_do_$n$_p: {\n", {{"n", n}});
            p.indent();
            hooks.emit_packed_body(tf);
            detail::emit_thread_probes(p, threaded, i);
            p.print("continue;\n");
            p.outdent();
            p.print("}\n");
        }
    }
    p.print("rp_field_general:;\n");
}

// Emit the general-switch routing case for ONE threaded field: a wire-guarded goto into its (shared)
// tag-consumed label, NOT a full arm and NOT a bare skip. read_tag_or_end has already consumed the
// tag (possibly a multi-byte / non-minimal encoding that missed the 1-byte hub), so on the expected
// wire type jump straight to the label; a wrong wire type `break`s to the shared skip. Carries both
// 2-byte-tag threaded fields (never in the hub) and the rare non-minimally-encoded tag of a 1-byte
// threaded field -- with zero body duplication, and no silent drop.
inline void emit_threaded_general_case(Printer& p, const ThreadField& tf) {
    const std::string n = std::to_string(tf.number);
    if (tf.repeated && tf.packable) {
        p.print(
            "case $n$: { if (rp_tag.wire_type == ::rapidproto::WireType::$w$)"
            " { goto rp_do_$n$; } if (rp_tag.wire_type == ::rapidproto::WireType::Len)"
            " { goto rp_do_$n$_p; } break; }\n",
            {{"n", n}, {"w", tf.thread_wire}});
    } else {
        p.print(
            "case $n$: { if (rp_tag.wire_type == ::rapidproto::WireType::$w$)"
            " { goto rp_do_$n$; } break; }\n",
            {{"n", n}, {"w", tf.thread_wire}});
    }
}

}  // namespace rapidproto::codegen
