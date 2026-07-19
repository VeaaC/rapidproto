// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#include "rapidproto/dumpgen/generator.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/codegen/wire.hpp"  // codegen::import_header: the dependency debug-header includes
#include "rapidproto/version.hpp"

namespace rapidproto::dumpgen {
namespace {

using arenagen::SynthNames;
using codegen::cpp_type_name;
using codegen::CppNameTable;
using codegen::Printer;

// A sentinel address distinguishing "field is DROPPED by the profile" (skip it) from "no plan to
// consult, emit as declared" (nullptr) in the member lookup below. Never dereferenced.
const arenagen::MemberPlan kDropped;

// The sub-namespace holding a file's generated INTERNALS, so the message's own namespace exposes only
// the two public entry points (rp_dump_write(ostream, ...) / rp_dump_string). The `_detail` naming
// follows the runtime's own `arena_detail` / `swar_detail` / `dump::detail` convention.
constexpr std::string_view kDetailNs = "rp_dump_detail";

// The namespace a message type's CORE dumper (the Writer-threaded rp_dump_write) lives in: the
// namespace its defining file emits it in, plus kDetailNs. Every recursive call is emitted fully
// qualified against this -- the core no longer sits in the message's own namespace, and ADL never
// looks inside a sub-namespace, so unqualified recursion would not find it.
//
// The namespace is read straight off the name table (`type_ns`, recorded when the type was indexed),
// never derived from the FQN: an FQN does not say where the package stops and message nesting starts,
// so any reconstruction breaks when a package segment collides with a message name -- and it would
// silently drop `model_namespace`. This lookup is exact by construction, so it can never disagree
// with the `message_namespace()` the header actually opens.
std::string dump_detail_ns(const CppNameTable& names, const std::string& fqn) {
    const auto it = names.type_ns.find(fqn);
    return "::" +
           codegen::join_ns(it != names.type_ns.end() ? it->second : names.ns_prefix, kDetailNs);
}

// How the debug writer must treat a field's VALUE, derived straight from the FieldNode (mirrors the
// arena accessor's return-type categories).
enum class ValueKind : std::uint8_t {
    Scalar,   // numeric / bool -> numeric literal (bool -> true/false)
    String,   // string -> JSON-escaped quoted
    Bytes,    // bytes -> lowercase hex quoted
    Enum,     // -> "NAME" (numeric fallback), via rp_dump_enum_name
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
// accessor's element type) into `w`. Enums route through rp_dump_enum_name. `detail_ns` is the
// callee's core-dumper namespace (see dump_detail_ns): required for -- and only used by -- the
// Message arm, whose recursive call is emitted fully qualified. Passed explicitly (never defaulted)
// so a new Message-capable call site can't silently emit a global-scope `::rp_dump_write`.
void emit_write_value(Printer& p, ValueKind kind, const std::string& expr,
                      const std::string& detail_ns) {
    switch (kind) {
        case ValueKind::Scalar:
            // bool prints as true/false via std::boolalpha (set once by the caller's ostream setup);
            // ints/float/double stream as numeric literals. Cast (u)int8 out (none here) not needed.
            p.print("w.os() << $e$;\n", {{"e", expr}});
            break;
        case ValueKind::String:
            p.print(
                "w.os() << '\"'; ::rapidproto::dump::write_json_escaped(w.os(), $e$);"
                " w.os() << '\"';\n",
                {{"e", expr}});
            break;
        case ValueKind::Bytes:
            p.print("w.os() << '\"'; ::rapidproto::dump::write_hex(w.os(), $e$); w.os() << '\"';\n",
                    {{"e", expr}});
            break;
        case ValueKind::Enum:
            // rp_dump_enum_name returns the value's stripped name, or nullptr for an unknown (open-
            // enum) value -- in which case we render UNKNOWN(<n>) straight to the stream, with no
            // shared/thread-local buffer. Bind the value once so the accessor call isn't repeated.
            p.print("{ const auto rp_e = $e$;\n", {{"e", expr}});
            p.print(
                "if (const char* rp_nm = ::rapidproto::dump::detail::rp_dump_enum_name(rp_e)) {"
                " w.os() << '\"' << rp_nm << '\"'; }\n");
            p.print(
                "else { w.os() << \"\\\"UNKNOWN(\""
                " << static_cast<std::int32_t>(rp_e) << \")\\\"\"; } }\n");
            break;
        case ValueKind::Message:
            p.print("$ns$::rp_dump_write($e$, w);\n", {{"ns", detail_ns}, {"e", expr}});
            break;
    }
}

// ── enum name tables ─────────────────────────────────────────────────────────────────────────────

// Collect every enum DEFINED in a message (recursively into nested messages), so rp_dump_enum_name is
// emitted once at the enum's definition site -- exactly like the enum type itself. A message that only
// REFERENCES an imported enum gets the overload through the imported .rp.dump.hpp it already includes;
// re-emitting it there would redefine the inline function when both headers meet in one translation unit.
void collect_defined_enums(const MessageNode& m, std::vector<const EnumNode*>& out) {
    for (const EnumNode& e : m.enums) {
        out.push_back(&e);
    }
    for (const MessageNode& n : m.nested_messages) {
        collect_defined_enums(n, out);
    }
}

// Emit `const char* rp_dump_enum_name(E)` -- a switch over the enum's known values returning the proto
// value name as a string literal; an unknown value returns nullptr so the caller renders UNKNOWN(<n>)
// itself (no shared buffer, no dangling pointer). Each returned pointer is a string literal with static
// lifetime, so it stays valid indefinitely.
void emit_enum_name_fn(Printer& p, const CppNameTable& names, const std::string& fqn,
                       const EnumNode& node) {
    const std::string type = cpp_type_name(names, fqn);
    p.print("inline const char* rp_dump_enum_name($T$ rp_e) {\n", {{"T", type}});
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

// `kind` is normally value_kind(f), but a field-modes `raw` message field overrides it to Bytes: its
// arena accessor stores the payload as a ByteView (optional/bare/ArrayView by presence), so the dumper
// renders it as lowercase hex, never as a nested object.
void emit_singular_field(Printer& p, const CppNameTable& names, const FieldNode& f,
                         ValueKind kind) {
    const std::string call = accessor_call(names, f);
    if (kind == ValueKind::Message) {
        // const T* accessor: skip when null, else (unless its path is skipped) emit the key + recurse.
        p.print("if (const auto* rp_p = $c$) {\n", {{"c", call}});
        p.indent();
        p.print("if (w.begin_field(rp_first, \"$k$\")) {\n", {{"k", f.name}});
        p.indent();
        p.print("w.push_path(\"$k$\");\n", {{"k", f.name}});
        emit_write_value(p, kind, "*rp_p", dump_detail_ns(names, f.resolved_type_fqn));
        p.print("w.pop_path();\n");
        p.outdent();
        p.print("}\n");
        p.outdent();
        p.print("}\n");
        return;
    }
    if (is_optional_accessor(f)) {
        // std::optional<T> accessor: skip when nullopt.
        p.print("if (const auto rp_v = $c$) {\n", {{"c", call}});
        p.indent();
        p.print("if (w.begin_field(rp_first, \"$k$\")) {\n", {{"k", f.name}});
        p.indent();
        emit_write_value(p, kind, "*rp_v", {});  // never Message: handled above
        p.outdent();
        p.print("}\n");
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
        p.print("if (w.begin_field(rp_first, \"$k$\")) {\n", {{"k", f.name}});
        p.indent();
        emit_write_value(p, kind, "rp_v", {});  // never Message: handled above
        p.outdent();
        p.print("}\n");
        p.outdent();
        p.print("}\n");
        return;
    }
    // Required: always present on the wire, so always emitted (even at the default value) unless skipped.
    p.print("if (w.begin_field(rp_first, \"$k$\")) {\n", {{"k", f.name}});
    p.indent();
    emit_write_value(p, kind, call, {});  // never Message: handled above
    p.outdent();
    p.print("}\n");
}

void emit_repeated_field(Printer& p, const CppNameTable& names, const FieldNode& f,
                         ValueKind kind) {
    const std::string call = accessor_call(names, f);
    // Omit an empty repeated field. Element type: message -> const T& ref via ptr-less range
    // (ArrayView<T> of message yields const T&); others yield the value/string_view directly.
    p.print("if (const auto& rp_r = $c$; !rp_r.empty()) {\n", {{"c", call}});
    p.indent();
    p.print("if (w.begin_field(rp_first, \"$k$\")) {\n", {{"k", f.name}});
    p.indent();
    // Message elements share the field's path (no index), so descendants can be skipped by e.g.
    // "people.address.city"; repeated scalar/string elements are not path-addressable (no push).
    if (kind == ValueKind::Message) {
        p.print("w.push_path(\"$k$\");\n", {{"k", f.name}});
    }
    p.print("w.group('[', ']', [&] {\n");
    p.indent();
    p.print("bool rp_efirst = true;\n");
    p.print("for (const auto& rp_el : rp_r) {\n");
    p.indent();
    p.print("w.entry_sep(rp_efirst);\n");
    // ArrayView<T> element is a value or const T& (message)
    emit_write_value(
        p, kind, "rp_el",
        kind == ValueKind::Message ? dump_detail_ns(names, f.resolved_type_fqn) : std::string{});
    p.print("if (w.overflowed()) { break; }\n");
    p.outdent();
    p.print("}\n");
    p.outdent();
    p.print("});\n");
    if (kind == ValueKind::Message) {
        p.print("w.pop_path();\n");
    }
    p.outdent();
    p.print("}\n");
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
    p.print("if (w.begin_field(rp_first, \"$k$\")) {\n", {{"k", mp.name}});
    p.indent();
    if (vkind ==
        ValueKind::Message) {  // message-valued entries' fields sit under the map field's path
        p.print("w.push_path(\"$k$\");\n", {{"k", mp.name}});
    }
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
            "w.os() << '\"'; ::rapidproto::dump::write_json_escaped(w.os(), rp_ent.key());"
            " w.os() << \"\\\": \";\n");
    } else {
        p.print("w.os() << '\"' << rp_ent.key() << \"\\\": \";\n");
    }
    if (vkind == ValueKind::Message) {
        // value() is const V* for a message entry.
        p.print(
            "if (const auto* rp_vp = rp_ent.value()) { $ns$::rp_dump_write(*rp_vp, w); }"
            " else { w.os() << \"null\"; }\n",
            {{"ns", dump_detail_ns(names, mp.resolved_value_type_fqn)}});
    } else {
        emit_write_value(p, vkind, "rp_ent.value()", {});  // never Message: handled above
    }
    p.print("if (w.overflowed()) { break; }\n");
    p.outdent();
    p.print("}\n");
    p.outdent();
    p.print("});\n");
    if (vkind == ValueKind::Message) {
        p.print("w.pop_path();\n");
    }
    p.outdent();
    p.print("}\n");
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
    //
    // No drop/raw handling here: mode resolution (arenagen/modes.cpp field_entry_error) rejects a
    // field-modes entry naming a oneof member, so a oneof member is always materialized as its
    // declared type -- the plan never drops or rawifies it.
    p.print("m.$o$([&](auto rp_tag, const auto& rp_v) {\n", {{"o", codegen::sanitize(o.name)}});
    p.indent();
    p.print("using RpTag = std::decay_t<decltype(rp_tag)>;\n");
    for (const FieldNode& f : o.fields) {
        const ValueKind kind = value_kind(f);
        const std::string member_tag = oneof_tag + "::" + names.local.at(&f);
        p.print("if constexpr (std::is_same_v<RpTag, $tag$>) {\n", {{"tag", member_tag}});
        p.indent();
        p.print("if (w.begin_field(rp_first, \"$k$\")) {\n", {{"k", f.name}});
        p.indent();
        if (kind == ValueKind::Message) {
            p.print("w.push_path(\"$k$\");\n", {{"k", f.name}});
        }
        emit_write_value(p, kind, "rp_v",
                         kind == ValueKind::Message ? dump_detail_ns(names, f.resolved_type_fqn)
                                                    : std::string{});
        if (kind == ValueKind::Message) {
            p.print("w.pop_path();\n");
        }
        p.outdent();
        p.print("}\n");
        p.outdent();
        p.print("}\n");
    }
    p.outdent();
    p.print("});\n");
}

// Forward-declare the core rp_dump_write(const T&, Writer&) for `m` and every nested message, so a
// message can reference a sibling / cousin / nested type whose definition comes later in the file
// (mutually-recursive A<->B, a parent that names a nested Def before it is defined, etc.). Still
// required now that recursion is emitted QUALIFIED: a qualified call needs the name already declared
// in that namespace, so without these it is a hard "no member named rp_dump_write" error.
void emit_message_fwd_decls(Printer& p, const CppNameTable& names, const MessageNode& m) {
    for (const MessageNode& n : m.nested_messages) {
        emit_message_fwd_decls(p, names, n);
    }
    p.print("inline void rp_dump_write(const $T$& m, ::rapidproto::dump::Writer& w);\n",
            {{"T", cpp_type_name(names, m.fqn)}});
}

// Emit the singular/repeated fields and maps of one message, consulting its arena plan so the decode
// profile is honored: a field/map ABSENT from the plan's members was DROPPED (no accessor exists ->
// skip it), and a `raw` message field materializes as a ByteView payload (render as bytes/hex, not as
// a nested object). A null `layout` means no plan to consult (every field emitted as declared).
void emit_fields_and_maps(Printer& p, const CppNameTable& names, const MessageNode& m,
                          const arenagen::MessageLayout* layout) {
    std::unordered_map<const void*, const arenagen::MemberPlan*> by_node;
    if (layout != nullptr) {
        for (const arenagen::MemberPlan& member : layout->members) {
            by_node.emplace(member.field != nullptr ? static_cast<const void*>(member.field)
                                                    : static_cast<const void*>(member.map_field),
                            &member);
        }
    }
    // nullptr = emit as declared (no plan); &kDropped = the profile dropped it (skip); else the plan.
    const auto member_for = [&](const void* node) -> const arenagen::MemberPlan* {
        if (layout == nullptr) {
            return nullptr;
        }
        const auto it = by_node.find(node);
        return it != by_node.end() ? it->second : &kDropped;
    };
    for (const FieldNode& f : m.fields) {
        const arenagen::MemberPlan* member = member_for(&f);
        if (member == &kDropped) {
            continue;  // dropped by the profile: no accessor exists
        }
        const bool raw = member != nullptr && member->kind == arenagen::FieldKind::Raw;
        const ValueKind kind = raw ? ValueKind::Bytes : value_kind(f);
        if (f.is_repeated) {
            emit_repeated_field(p, names, f, kind);
        } else {
            emit_singular_field(p, names, f, kind);
        }
    }
    for (const MapFieldNode& mp : m.map_fields) {
        if (member_for(&mp) == &kDropped) {
            continue;  // dropped by the profile (raw never applies to a map -> it is materialized)
        }
        emit_map_field(p, names, mp);
    }
}

// The CORE dumper for one message (and, recursively, its nested messages): the Writer-threaded
// rp_dump_write every other dumper calls. Emitted inside the file's rp_dump_detail namespace.
void emit_message_core(Printer& p, const CppNameTable& names, const SynthNames& synth,
                       const arenagen::LayoutSet& layouts, const MessageNode& m) {
    // Recurse into nested messages first; forward declarations (emitted for the whole file up front)
    // let a definition reference any other message's dumper regardless of emission order.
    for (const MessageNode& n : m.nested_messages) {
        emit_message_core(p, names, synth, layouts, n);
    }
    const std::string type = cpp_type_name(names, m.fqn);
    p.print("inline void rp_dump_write(const $T$& m, ::rapidproto::dump::Writer& w) {\n",
            {{"T", type}});
    p.indent();
    p.print("(void)m;\n");  // a zero-field message never reads `m`; keeps {} warning-free
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
    emit_fields_and_maps(p, names, m, layout);
    for (const OneofNode& o : m.oneofs) {
        emit_oneof(p, names, o, type + "::" + synth.case_tag.at(&o));
    }
    p.print("(void)rp_first;\n");  // no fields -> unused; keeps an empty message ({}) warning-free
    p.outdent();
    p.print("});\n");
    p.outdent();
    p.print("}\n\n");
}

// The PUBLIC entry points for one message (and, recursively, its nested messages), emitted in the
// message's own namespace: the two convenience overloads a consumer actually calls. They forward to
// the core dumper in rp_dump_detail, which is the only thing that ever touches a Writer.
void emit_message_public(Printer& p, const CppNameTable& names, const MessageNode& m) {
    for (const MessageNode& n : m.nested_messages) {
        emit_message_public(p, names, n);
    }
    const std::string type = cpp_type_name(names, m.fqn);
    const std::string detail = dump_detail_ns(names, m.fqn);

    // A convenience overload that constructs a Writer over an ostream. `opts` carries the line-width
    // budget (compact vs multi-line), the start indent, and the skip-paths; an integer still converts
    // (back-compat: `rp_dump_write(os, m, 120)` sets the width).
    p.print(
        "inline void rp_dump_write(std::ostream& rp_os, const $T$& m,"
        " const ::rapidproto::dump::DumpOptions& rp_opts = {}) {\n",
        {{"T", type}});
    p.indent();
    p.print("rp_os << std::boolalpha;\n");
    p.print("::rapidproto::dump::Writer w(rp_os, rp_opts.width, rp_opts.indent, &rp_opts.skip);\n");
    p.print("$ns$::rp_dump_write(m, w);\n", {{"ns", detail}});
    p.outdent();
    p.print("}\n\n");

    // std::string convenience.
    p.print(
        "inline std::string rp_dump_string(const $T$& m,"
        " const ::rapidproto::dump::DumpOptions& rp_opts = {}) {\n",
        {{"T", type}});
    p.indent();
    p.print("std::ostringstream rp_ss; rp_dump_write(rp_ss, m, rp_opts); return rp_ss.str();\n");
    p.outdent();
    p.print("}\n\n");
}

}  // namespace

std::string generate_header(const FileNode& file, const CppNameTable& names,
                            const arenagen::LayoutSet& layouts) {
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
    // The debug header is the one-stop include for dumping: it pulls the arena header (the decoder +
    // the message/enum types) so a consumer who includes only this header can decode AND dump. The
    // IWYU export makes those arena types count as "directly provided" here.
    p.print("#include \"$s$.rp.hpp\"  // IWYU pragma: export\n", {{"s", stem}});
    p.print("#include \"rapidproto/dump_runtime.hpp\"\n");
    // A field (sub-message / repeated / map-value / oneof member) may reference an imported type, whose
    // dumper lives in the imported file's own debug header. Include each dependency's debug header so
    // its rp_dump_detail namespace is declared before we emit a qualified call into it -- these
    // includes stayed necessary when recursion moved off ADL onto qualified lookup, which likewise
    // needs the callee declared. The parallel of the arena header's cross-file includes, keeping every
    // debug header self-contained.
    for (const auto& import : file.imports) {
        if (import.kind != ImportKind::Option) {
            p.print("#include \"$h$\"\n",
                    {{"h", codegen::import_header(import.path, ".rp.dump.hpp")}});
        }
    }
    p.print("\n");

    // Enum name tables: rp_dump_enum_name(E) for each enum DEFINED in this file, in namespace
    // rapidproto::dump::detail so the dumpers call it by one fully-qualified name (overload resolution
    // on the enum type picks the right one) without landing in the runtime's PUBLIC namespace.
    // Emitted once at the definition site -- a file that references an imported enum gets the overload
    // via that import's included .rp.dump.hpp, never re-emitting it.
    std::vector<const EnumNode*> defined_enums;
    defined_enums.reserve(file.enums.size());
    for (const EnumNode& e : file.enums) {
        defined_enums.push_back(&e);
    }
    for (const MessageNode& m : file.messages) {
        collect_defined_enums(m, defined_enums);
    }
    if (!defined_enums.empty()) {
        p.print("namespace rapidproto::dump::detail {\n\n");
        for (const EnumNode* e : defined_enums) {
            emit_enum_name_fn(p, names, e->fqn, *e);
            p.print("\n");
        }
        p.print("}  // namespace rapidproto::dump::detail\n\n");
    }

    // The public entry points live in the message's own namespace (so a consumer calls
    // `pkg::rp_dump_string(m)`); the Writer-threaded cores they forward to live one level down, in
    // pkg::rp_dump_detail, keeping the package namespace free of generated internals. Cross-file
    // recursion is emitted fully qualified against that sub-namespace -- ADL would not reach into it.
    const std::string ns = codegen::message_namespace(names, file);
    if (!ns.empty()) {
        p.print("namespace $ns$ {\n\n", {{"ns", ns}});
    }
    if (!file.messages.empty()) {
        p.print("namespace $d$ {\n\n", {{"d", kDetailNs}});
        for (const MessageNode& m : file.messages) {
            emit_message_fwd_decls(p, names, m);
        }
        p.print("\n");
        for (const MessageNode& m : file.messages) {
            emit_message_core(p, names, synth, layouts, m);
        }
        p.print("}  // namespace $d$\n\n", {{"d", kDetailNs}});
        for (const MessageNode& m : file.messages) {
            emit_message_public(p, names, m);
        }
    }
    if (!ns.empty()) {
        p.print("}  // namespace $ns$\n", {{"ns", ns}});
    }
    return p.str();
}

}  // namespace rapidproto::dumpgen
