// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#include "rapidproto/debuggen/generator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/version.hpp"

namespace rapidproto::debuggen {
namespace {

using arenagen::SynthNames;
using codegen::cpp_type_name;
using codegen::CppNameTable;
using codegen::Printer;

// The default line-width budget the generated rp_debug_write/rp_debug_string overloads take: a group
// renders compact (single line) if it fits this many columns, else multi-line. Emitted as the default
// argument in the generated signatures.
constexpr std::size_t kDefaultWidth = 120;

// How the debug writer must treat a field's VALUE, derived straight from the FieldNode (mirrors the
// arena accessor's return-type categories).
enum class ValueKind : std::uint8_t {
    Scalar,   // numeric / bool -> numeric literal (bool -> true/false)
    String,   // string -> JSON-escaped quoted
    Bytes,    // bytes -> lowercase hex quoted
    Enum,     // -> "NAME" (numeric fallback), via rp_debug_enum_name
    Message,  // -> nested object (const T* accessor)
};

ValueKind value_kind(const FieldNode& f) {
    if (f.is_message_type) {
        // A proto2 group is just a message field with a different wire encoding: arenagen decodes it
        // through the IDENTICAL nested-message accessor (const T*), so it dumps as a normal Message
        // with no special case here.
        return ValueKind::Message;
    }
    if (f.is_enum_type) {
        return ValueKind::Enum;
    }
    if (f.type_name == "string") {
        return ValueKind::String;
    }
    if (f.type_name == "bytes") {
        return ValueKind::Bytes;
    }
    return ValueKind::Scalar;
}

// A scalar/string/enum field carries explicit presence -> its arena accessor returns std::optional<T>
// (proto2 optional / proto3 optional). Implicit (proto3 singular) and Required return the bare value;
// message fields always return const T* (their own null == absent). Repeated/map handled separately.
bool is_optional_accessor(const FieldNode& f) {
    return f.presence == FieldPresence::Explicit && !f.is_repeated;
}

// ── value writers (emit code that writes ONE already-obtained value `expr` to the Writer) ────────────

// Write the statement(s) that render a single scalar/string/bytes/enum value `expr` (of the arena
// accessor's element type) into `w`. Enums route through rp_debug_enum_name.
void emit_write_value(Printer& p, ValueKind kind, const std::string& expr) {
    switch (kind) {
        case ValueKind::Scalar:
            // bool prints as true/false via std::boolalpha (set once by the caller's ostream setup);
            // ints/float/double stream as numeric literals. Cast (u)int8 out (none here) not needed.
            p.print("w.os() << $e$;\n", {{"e", expr}});
            break;
        case ValueKind::String:
            p.print(
                "w.os() << '\"'; ::rapidproto::debug::write_json_escaped(w.os(), $e$);"
                " w.os() << '\"';\n",
                {{"e", expr}});
            break;
        case ValueKind::Bytes:
            p.print(
                "w.os() << '\"'; ::rapidproto::debug::write_hex(w.os(), $e$); w.os() << '\"';\n",
                {{"e", expr}});
            break;
        case ValueKind::Enum:
            // rp_debug_enum_name returns the value's stripped name, or nullptr for an unknown (open-
            // enum) value -- in which case we render UNKNOWN(<n>) straight to the stream, with no
            // shared/thread-local buffer. Bind the value once so the accessor call isn't repeated.
            p.print("{ const auto rp_e = $e$;\n", {{"e", expr}});
            p.print(
                "if (const char* rp_nm = ::rapidproto::debug::rp_debug_enum_name(rp_e)) {"
                " w.os() << '\"' << rp_nm << '\"'; }\n");
            p.print(
                "else { w.os() << \"\\\"UNKNOWN(\""
                " << static_cast<std::int32_t>(rp_e) << \")\\\"\"; } }\n");
            break;
        case ValueKind::Message:
            p.print("rp_debug_write($e$, w);\n", {{"e", expr}});
            break;
    }
}

// ── enum name tables ─────────────────────────────────────────────────────────────────────────────

// Collect every enum TYPE referenced by a field/map anywhere in the file's messages (so we emit one
// rp_debug_enum_name overload per enum). Keyed by resolved FQN.
void collect_enums(const MessageNode& m, std::unordered_set<std::string>& out) {
    for (const FieldNode& f : m.fields) {
        if (f.is_enum_type) {
            out.insert(f.resolved_type_fqn);
        }
    }
    for (const MapFieldNode& mp : m.map_fields) {
        if (mp.value_is_enum) {
            out.insert(mp.resolved_value_type_fqn);
        }
    }
    for (const OneofNode& o : m.oneofs) {
        for (const FieldNode& f : o.fields) {
            if (f.is_enum_type) {
                out.insert(f.resolved_type_fqn);
            }
        }
    }
    for (const MessageNode& n : m.nested_messages) {
        collect_enums(n, out);
    }
}

// Emit `const char* rp_debug_enum_name(E)` -- a switch over the enum's known values returning the proto
// value name as a string literal; an unknown value returns nullptr so the caller renders UNKNOWN(<n>)
// itself (no shared buffer, no dangling pointer). Each returned pointer is a string literal with static
// lifetime, so it stays valid indefinitely.
void emit_enum_name_fn(Printer& p, const CppNameTable& names, const std::string& fqn,
                       const EnumNode& node) {
    const std::string type = cpp_type_name(names, fqn);
    p.print("inline const char* rp_debug_enum_name($T$ rp_e) {\n", {{"T", type}});
    p.indent();
    p.print("switch (static_cast<std::int32_t>(rp_e)) {\n");
    p.indent();
    // Display each value with its prefix stripped the same way the generated `enum class` strips it
    // (STATE_ON -> ON), so the debug name matches the C++ enumerator. If the enum isn't strippable
    // (a value missing the prefix, a keyword/macro remainder, etc.) the full proto names are kept.
    const std::string prefix = codegen::enum_value_prefix(node.name);
    const bool strip = codegen::enum_prefix_strippable(node, prefix);
    std::unordered_set<std::int32_t> seen;  // an alias enum (allow_alias) repeats a number
    for (const EnumValueNode& v : node.values) {
        if (!seen.insert(v.number).second) {
            continue;
        }
        std::string_view raw = v.name;
        if (strip) {
            raw.remove_prefix(prefix.size());
        }
        p.print("case $n$: return \"$name$\";\n",
                {{"n", std::to_string(v.number)}, {"name", codegen::sanitize(raw)}});
    }
    p.outdent();
    p.print("}\n");
    p.print("return nullptr;  // unknown (open enum): the caller renders UNKNOWN(<n>)\n");
    p.outdent();
    p.print("}\n");
}

// ── per-message dumper ───────────────────────────────────────────────────────────────────────────

// The `m.<accessor>()` call for a field (accessor id == the deduped local name in `names`).
std::string accessor_call(const CppNameTable& names, const FieldNode& f) {
    return "m." + names.local.at(&f) + "()";
}

void emit_singular_field(Printer& p, const CppNameTable& names, const FieldNode& f) {
    const ValueKind kind = value_kind(f);
    const std::string call = accessor_call(names, f);
    if (kind == ValueKind::Message) {
        // const T* accessor: skip when null, else emit the key + recurse.
        p.print("if (const auto* rp_p = $c$) {\n", {{"c", call}});
        p.indent();
        p.print("w.entry_sep(rp_first); w.key(\"$k$\");\n", {{"k", f.name}});
        emit_write_value(p, kind, "*rp_p");
        p.outdent();
        p.print("}\n");
        return;
    }
    if (is_optional_accessor(f)) {
        // std::optional<T> accessor: skip when nullopt.
        p.print("if (const auto rp_v = $c$) {\n", {{"c", call}});
        p.indent();
        p.print("w.entry_sep(rp_first); w.key(\"$k$\");\n", {{"k", f.name}});
        emit_write_value(p, kind, "*rp_v");
        p.outdent();
        p.print("}\n");
        return;
    }
    if (f.presence == FieldPresence::Implicit) {
        // Implicit presence (proto3 singular): "unset" is indistinguishable from the default, so a
        // default-valued field (0 / false / "" / the zero enum) is omitted -- as protobuf's own JSON
        // does. decltype(rp_v){} is that zero default for the accessor's value type.
        p.print("if (const auto rp_v = $c$; rp_v != decltype(rp_v){}) {\n", {{"c", call}});
        p.indent();
        p.print("w.entry_sep(rp_first); w.key(\"$k$\");\n", {{"k", f.name}});
        emit_write_value(p, kind, "rp_v");
        p.outdent();
        p.print("}\n");
        return;
    }
    // Required: always present on the wire, so always emitted (even at the default value).
    p.print("{\n");
    p.indent();
    p.print("w.entry_sep(rp_first); w.key(\"$k$\");\n", {{"k", f.name}});
    emit_write_value(p, kind, call);
    p.outdent();
    p.print("}\n");
}

void emit_repeated_field(Printer& p, const CppNameTable& names, const FieldNode& f) {
    const ValueKind kind = value_kind(f);
    const std::string call = accessor_call(names, f);
    // Omit an empty repeated field. Element type: message -> const T& ref via ptr-less range
    // (ArrayView<T> of message yields const T&); others yield the value/string_view directly.
    p.print("if (const auto& rp_r = $c$; !rp_r.empty()) {\n", {{"c", call}});
    p.indent();
    p.print("w.entry_sep(rp_first); w.key(\"$k$\");\n", {{"k", f.name}});
    p.print("w.group('[', ']', [&] {\n");
    p.indent();
    p.print("bool rp_efirst = true;\n");
    p.print("for (const auto& rp_el : rp_r) {\n");
    p.indent();
    p.print("w.entry_sep(rp_efirst);\n");
    emit_write_value(p, kind, "rp_el");  // ArrayView<T> element is a value or const T& (message)
    p.print("if (w.overflowed()) { break; }\n");
    p.outdent();
    p.print("}\n");
    p.outdent();
    p.print("});\n");
    p.outdent();
    p.print("}\n");
}

void emit_map_field(Printer& p, const CppNameTable& names, const MapFieldNode& mp) {
    // The map accessor returns MapView<Entry>; each Entry has .key()/.value(). Render as a nested
    // object keyed by the (stringified) key. Value kind derived from the map's value type.
    ValueKind vkind = ValueKind::Scalar;
    if (mp.value_is_message) {
        vkind = ValueKind::Message;
    } else if (mp.value_is_enum) {
        vkind = ValueKind::Enum;
    } else if (mp.value_type == "string") {
        vkind = ValueKind::String;
    } else if (mp.value_type == "bytes") {
        vkind = ValueKind::Bytes;
    }
    const bool key_is_string = mp.key_type == "string";
    // Omit an empty map.
    p.print("if (const auto& rp_mp = m.$acc$(); !rp_mp.empty()) {\n",
            {{"acc", names.local.at(&mp)}});
    p.indent();
    p.print("w.entry_sep(rp_first); w.key(\"$k$\");\n", {{"k", mp.name}});
    p.print("w.group('{', '}', [&] {\n");
    p.indent();
    p.print("bool rp_efirst = true;\n");
    p.print("for (const auto& rp_ent : rp_mp) {\n");
    p.indent();
    p.print("w.entry_sep(rp_efirst);\n");
    // The object key is always a JSON string. A string key escapes; a numeric key is streamed into
    // the quotes as its decimal form.
    if (key_is_string) {
        p.print(
            "w.os() << '\"'; ::rapidproto::debug::write_json_escaped(w.os(), rp_ent.key());"
            " w.os() << \"\\\": \";\n");
    } else {
        p.print("w.os() << '\"' << rp_ent.key() << \"\\\": \";\n");
    }
    if (vkind == ValueKind::Message) {
        // value() is const V* for a message entry.
        p.print(
            "if (const auto* rp_vp = rp_ent.value()) { rp_debug_write(*rp_vp, w); }"
            " else { w.os() << \"null\"; }\n");
    } else {
        emit_write_value(p, vkind, "rp_ent.value()");
    }
    p.print("if (w.overflowed()) { break; }\n");
    p.outdent();
    p.print("}\n");
    p.outdent();
    p.print("});\n");
    p.outdent();
    p.print("}\n");
}

void emit_oneof(Printer& p, const CppNameTable& names, const OneofNode& o,
                const std::string& oneof_tag) {
    // The arena oneof reader allows at most one (auto, auto) catch-all handler. We pass exactly one:
    // it fires for whichever member is active, receiving that member's visit-tag TYPE (`rp_tag`) and
    // decoded value. We recover the member NAME by comparing the tag type against each synthesized
    // member tag (<Oneof>::<member>), then render that member as a normal field. The unset
    // std::monostate state is simply not handled -> the oneof contributes nothing (skipped).
    p.print("m.$o$([&](auto rp_tag, const auto& rp_v) {\n", {{"o", codegen::sanitize(o.name)}});
    p.indent();
    p.print("using RpTag = std::decay_t<decltype(rp_tag)>;\n");
    for (const FieldNode& f : o.fields) {
        const ValueKind kind = value_kind(f);
        const std::string member_tag = oneof_tag + "::" + names.local.at(&f);
        p.print("if constexpr (std::is_same_v<RpTag, $tag$>) {\n", {{"tag", member_tag}});
        p.indent();
        p.print("w.entry_sep(rp_first); w.key(\"$k$\");\n", {{"k", f.name}});
        emit_write_value(p, kind, "rp_v");
        p.outdent();
        p.print("}\n");
    }
    p.outdent();
    p.print("});\n");
}

void emit_message_dumper(Printer& p, const CppNameTable& names, const SynthNames& synth,
                         const arenagen::LayoutSet& layouts, const MessageNode& m) {
    // Recurse into nested messages first (free functions; order only matters for name visibility,
    // which is fine since all are declared/defined at namespace scope after forward-friendly ADL --
    // we simply emit nested before parent).
    for (const MessageNode& n : m.nested_messages) {
        emit_message_dumper(p, names, synth, layouts, n);
    }
    const std::string type = cpp_type_name(names, m.fqn);
    p.print("inline void rp_debug_write(const $T$& m, ::rapidproto::debug::Writer& w) {\n",
            {{"T", type}});
    p.indent();
    p.print("w.group('{', '}', [&] {\n");
    p.indent();
    p.print("bool rp_first = true;\n");
    // The arena message reserves a "saw an unknown field" bit (--unknown-present / --unknown <msg>)
    // iff its layout carries an unknown_bit -- exactly how arenagen decides to emit has_unknown_fields().
    // It is BIT-ONLY: no unknown field DATA is retained, so we can only report presence, not contents.
    const arenagen::MessageLayout* layout = layouts.find(m.fqn);
    if (layout != nullptr && layout->unknown_bit >= 0) {
        p.print(
            "if (m.$h$()) { w.entry_sep(rp_first);"
            " w.os() << \"\\\"has_unknown_fields\\\": true\"; }\n",
            {{"h", synth.unknown.at(&m)}});
    }
    for (const FieldNode& f : m.fields) {
        if (f.is_repeated) {
            emit_repeated_field(p, names, f);
        } else {
            emit_singular_field(p, names, f);
        }
    }
    for (const MapFieldNode& mp : m.map_fields) {
        emit_map_field(p, names, mp);
    }
    for (const OneofNode& o : m.oneofs) {
        emit_oneof(p, names, o, type + "::" + synth.case_tag.at(&o));
    }
    p.print("(void)rp_first;\n");  // no fields -> unused; keeps an empty message ({}) warning-free
    p.outdent();
    p.print("});\n");
    p.outdent();
    p.print("}\n\n");

    // A convenience overload that constructs a Writer over an ostream. `width` is the line-width budget
    // used to decide compact (single-line) vs multi-line rendering.
    p.print(
        "inline void rp_debug_write(std::ostream& rp_os, const $T$& m,"
        " std::size_t rp_width = $w$) {\n",
        {{"T", type}, {"w", std::to_string(kDefaultWidth)}});
    p.indent();
    p.print("rp_os << std::boolalpha;\n");
    p.print("::rapidproto::debug::Writer w(rp_os, rp_width);\n");
    p.print("rp_debug_write(m, w);\n");
    p.outdent();
    p.print("}\n\n");

    // std::string convenience.
    p.print("inline std::string rp_debug_string(const $T$& m, std::size_t rp_width = $w$) {\n",
            {{"T", type}, {"w", std::to_string(kDefaultWidth)}});
    p.indent();
    p.print("std::ostringstream rp_ss; rp_debug_write(rp_ss, m, rp_width); return rp_ss.str();\n");
    p.outdent();
    p.print("}\n\n");
}

}  // namespace

std::string generate_header(const FileNode& file, const CppNameTable& names,
                            const arenagen::LayoutSet& layouts, const SymbolTable& symbols) {
    Printer p;
    const SynthNames synth = arenagen::build_synth_names(names, layouts, file);
    p.print("// Generated by rapidprotoc $v$ (debug dumper). DO NOT EDIT.\n", {{"v", kVersion}});
    p.print("#pragma once\n\n");
    p.print("#include <cstddef>\n");
    p.print("#include <cstdint>\n");  // std::int32_t: the enum-UNKNOWN(<n>) fallback
    p.print("#include <ostream>\n");
    p.print("#include <sstream>\n");
    p.print("#include <string>\n");
    p.print("#include <type_traits>\n\n");
    const std::string stem = [&] {
        std::string s = file.filename;
        const auto slash = s.find_last_of('/');
        if (slash != std::string::npos) {
            s = s.substr(slash + 1);
        }
        const std::string kProto = ".proto";
        if (s.size() >= kProto.size() &&
            s.compare(s.size() - kProto.size(), kProto.size(), kProto) == 0) {
            s = s.substr(0, s.size() - kProto.size());
        }
        return s;
    }();
    p.print("#include \"$s$.rp.hpp\"\n", {{"s", stem}});
    p.print("#include \"rapidproto/debug_runtime.hpp\"\n\n");

    // Enum name tables: emitted in namespace rapidproto::debug so the dumpers can call them by a
    // single fully-qualified name; overload resolution on the enum type picks the right one.
    std::unordered_set<std::string> enum_fqns;
    for (const MessageNode& m : file.messages) {
        collect_enums(m, enum_fqns);
    }
    // Also top-level enums that appear as field types are covered above; we only emit the ones
    // actually referenced (to keep the header tight and avoid unused warnings).
    p.print("namespace rapidproto::debug {\n\n");
    for (const std::string& fqn : enum_fqns) {
        const auto it = symbols.enums.find(fqn);
        if (it != symbols.enums.end()) {
            emit_enum_name_fn(p, names, fqn, *it->second);
            p.print("\n");
        }
    }
    p.print("}  // namespace rapidproto::debug\n\n");

    // The dumpers live in the message's own namespace so unqualified rp_debug_write recursion and ADL
    // both resolve.
    const std::string ns = codegen::message_namespace(names, file);
    if (!ns.empty()) {
        p.print("namespace $ns$ {\n\n", {{"ns", ns}});
    }
    for (const MessageNode& m : file.messages) {
        emit_message_dumper(p, names, synth, layouts, m);
    }
    if (!ns.empty()) {
        p.print("}  // namespace $ns$\n", {{"ns", ns}});
    }
    return p.str();
}

}  // namespace rapidproto::debuggen
