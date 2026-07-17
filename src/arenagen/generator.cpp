// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#include "rapidproto/arenagen/generator.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/codegen/wire.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/version.hpp"

namespace rapidproto::arenagen {
namespace {

using codegen::cpp_type_name;
using codegen::CppNameTable;
using codegen::join_ns;
using codegen::namespace_of;
using codegen::Printer;

// ── small string helpers ─────────────────────────────────────────────────────────────────────────

// A proto numeric/bool scalar keyword -> its C++ storage type, from the shared codegen table. Callers
// reach this only after classifying the field as a numeric scalar (string/bytes become ArenaString;
// enum/message route elsewhere), so the lookup resolves; the keyword echo is a defensive fallback that
// keeps a malformed schema from yielding an empty type rather than relying on a debug-only assert.
std::string cpp_scalar(std::string_view type) {
    const std::string_view cpp = codegen::cpp_numeric_type(type);
    return std::string(cpp.empty() ? type : cpp);
}

std::string capitalize(std::string_view name) {
    std::string out(name);
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

std::string mask_word_type(std::size_t mask_size) {
    switch (mask_size) {
        case 1:
            return "std::uint8_t";
        case 2:
            return "std::uint16_t";
        case 4:
            return "std::uint32_t";
        default:
            return "std::uint64_t";  // 8, or each element of a >64-bit array
    }
}

constexpr std::size_t kWordBytes = 8;  // a uint64 mask word; >8 mask bytes means a uint64[] array
constexpr int kWordBits = 64;

// A `(mask & bit)` test for bit index `bit`, honoring single- vs multi-word masks.
std::string bit_test(const MessageLayout& layout, int bit) {
    if (layout.mask_size > kWordBytes) {
        return "(m_rp_mask[" + std::to_string(bit / kWordBits) + "] & (std::uint64_t{1} << " +
               std::to_string(bit % kWordBits) + "))";
    }
    return "(m_rp_mask & (" + mask_word_type(layout.mask_size) + "{1} << " + std::to_string(bit) +
           "))";
}

// ── type-name helpers ────────────────────────────────────────────────────────────────────────────

// SynthNames (the deduped <Oneof> visit-tag / <Map>Entry / has_unknown_fields identifiers) is now a
// public type in rapidproto/arenagen/generator.hpp -- exposed so the debug dumper can name the SAME
// deduped identifiers this header emitted. build_synth_names() (its definition, below) is likewise
// public; synth_for_message() stays file-local.

// Refs to long-lived inputs; Emit is a short-lived, non-copied bundle threaded through emission.
struct Emit {
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    Printer& printer;
    const CppNameTable& names;
    const LayoutSet& layouts;
    const SynthNames& synth;
    const SymbolTable& types;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

std::string member_id(const Emit& emit, const MemberPlan& m) {
    return m.field != nullptr ? emit.names.local.at(m.field) : emit.names.local.at(m.map_field);
}

std::string repeated_elem_type(const Emit& emit, const FieldNode& field) {
    if (field.is_message_type || field.is_enum_type) {
        return cpp_type_name(emit.names, field.resolved_type_fqn);
    }
    if (field.type_name == "string" || field.type_name == "bytes") {
        return "::rapidproto::ArenaString";
    }
    return cpp_scalar(field.type_name);
}

// The C++ storage type of one byte-occupying member (bit-only bools/wrappers return "").
std::string storage_type(const Emit& emit, const MemberPlan& m) {
    switch (m.kind) {
        case FieldKind::InlineScalar:
            return cpp_scalar(m.field->type_name);
        case FieldKind::InlineEnum:
            return cpp_type_name(emit.names, m.target_fqn);
        case FieldKind::SsoString:
            return "::rapidproto::ArenaString";
        case FieldKind::InlineFixedSubMsg:
            return cpp_type_name(emit.names, m.target_fqn);
        case FieldKind::PointerSubMsg:
            return "const " + cpp_type_name(emit.names, m.target_fqn) + "*";
        case FieldKind::Repeated:
            return "::rapidproto::ArrayView<" + repeated_elem_type(emit, *m.field) + ">";
        case FieldKind::Map:
            return "::rapidproto::MapView<" + emit.synth.entry_type.at(m.map_field) + ">";
        case FieldKind::Raw:
            return m.field->is_repeated ? "::rapidproto::ArrayView<::rapidproto::ByteView>"
                                        : "::rapidproto::ByteView";
    }
    return "";
}

std::string oneof_member_storage(const Emit& emit, const OneofMemberPlan& m) {
    switch (m.kind) {
        case FieldKind::InlineEnum:
            return cpp_type_name(emit.names, m.target_fqn);
        case FieldKind::SsoString:
            return "::rapidproto::ArenaString";
        case FieldKind::InlineFixedSubMsg:
            return cpp_type_name(emit.names, m.target_fqn);
        case FieldKind::PointerSubMsg:
            return "const " + cpp_type_name(emit.names, m.target_fqn) + "*";
        default:
            return cpp_scalar(m.field->type_name);
    }
}

// ── nested map-entry struct ──────────────────────────────────────────────────────────────────────
void emit_map_entry(const Emit& emit, const MemberPlan& m, const std::string& enclosing) {
    Printer& p = emit.printer;
    assert(m.entry.has_value());
    const EntryPlan& e = *m.entry;
    const std::string key_store = e.key_kind == FieldKind::SsoString
                                      ? "::rapidproto::ArenaString"
                                      : cpp_scalar(m.map_field->key_type);
    std::string val_store;
    switch (e.value_kind) {
        case FieldKind::SsoString:
            val_store = "::rapidproto::ArenaString";
            break;
        case FieldKind::InlineEnum:
        case FieldKind::InlineFixedSubMsg:
        case FieldKind::PointerSubMsg:
            val_store = cpp_type_name(emit.names, e.value_fqn);
            break;
        default:
            val_store = cpp_scalar(m.map_field->value_type);
    }
    const std::string key_decl = key_store + " rp_key;";
    const std::string val_decl =
        (e.value_kind == FieldKind::PointerSubMsg ? "const " + val_store + "* rp_value;"
                                                  : val_store + " rp_value;");

    p.print("struct $N$ {\n", {{"N", emit.synth.entry_type.at(m.map_field)}});
    p.indent();
    // Read-only accessors; the key/value storage below is private, so a consumer cannot rewrite a
    // parsed entry. Only the enclosing message's decoder (a friend) populates it.
    if (e.key_kind == FieldKind::SsoString) {
        p.print("std::string_view key() const noexcept { return rp_key.view(); }\n");
    } else {
        p.print("$T$ key() const noexcept { return rp_key; }\n", {{"T", key_store}});
    }
    switch (e.value_kind) {
        case FieldKind::SsoString:
            p.print("std::string_view value() const noexcept { return rp_value.view(); }\n");
            break;
        case FieldKind::PointerSubMsg:
            p.print("const $T$* value() const noexcept { return rp_value; }\n", {{"T", val_store}});
            break;
        case FieldKind::InlineFixedSubMsg:
            p.print("const $T$* value() const noexcept { return &rp_value; }\n",
                    {{"T", val_store}});
            break;
        default:
            p.print("$T$ value() const noexcept { return rp_value; }\n", {{"T", val_store}});
    }
    p.print("friend class $E$;\n", {{"E", enclosing}});
    p.outdent();
    p.print(" private:\n");
    p.indent();
    if (e.key_offset <= e.value_offset) {  // storage in compact (offset) order
        p.print("$d$\n", {{"d", key_decl}});
        p.print("$d$\n", {{"d", val_decl}});
    } else {
        p.print("$d$\n", {{"d", val_decl}});
        p.print("$d$\n", {{"d", key_decl}});
    }
    p.outdent();
    p.print("};\n");
}

// ── enums ────────────────────────────────────────────────────────────────────────────────────────
// ── oneof ────────────────────────────────────────────────────────────────────────────────────────
void emit_oneof_visit_tags(const Emit& emit, const OneofPlan& o) {
    Printer& p = emit.printer;
    // Visit-tag types: one per member (named after the field), each with a Value typedef -- mirroring
    // the streaming decoder's per-field tags, so the same combine/handles_one dispatch machinery
    // drives the oneof's <name>() reader (unhandled members ignored; a single (auto, auto) catch-all
    // allowed). The UNSET state is std::monostate -- kept out of this struct so it can never clash with
    // a member named e.g. `none`.
    p.print("struct $S$ {\n", {{"S", emit.synth.case_tag.at(o.oneof)}});
    p.indent();
    for (const OneofMemberPlan& member : o.members) {
        std::string vt;
        if (member.kind == FieldKind::SsoString) {
            vt = "std::string_view";
        } else if (member.kind == FieldKind::InlineFixedSubMsg ||
                   member.kind == FieldKind::PointerSubMsg) {
            // Bare (decayed) type: handlers take `const <T>&` (the arena object, no copy), but the
            // dispatch traits compare decayed parameter types, so Value must not be a reference.
            vt = cpp_type_name(emit.names, member.target_fqn);
        } else {
            vt = oneof_member_storage(emit, member);  // scalar / enum
        }
        p.print("struct $m$ { using Value = $V$; };\n",
                {{"m", emit.names.local.at(member.field)}, {"V", vt}});
    }
    p.outdent();
    p.print("};\n");
}

// A union sized to the largest member; the no-op default ctor leaves it inactive (the decoder
// sets the active member, the reader dispatches on the discriminant). Trivially destructible/copyable.
// Emitted in the PRIVATE section (unlike the visit-tag struct above): it is pure storage that no
// accessor exposes, so it must not surface in a user's autocomplete as constructible API.
// NOTE: the union makes the enclosing message non-trivially-default-constructible, so the decoder
// must VALUE-initialize it, which Arena::create<T>() does via (::new (mem) T()), to zero the
// discriminant to 0 (the unset state); default-init (::new (mem) T) would leave the case indeterminate.
void emit_oneof_union(const Emit& emit, const OneofPlan& o) {
    Printer& p = emit.printer;
    p.print("union rp_$o$_union {\n", {{"o", o.oneof->name}});
    p.indent();
    for (const OneofMemberPlan& member : o.members) {
        p.print("$T$ $m$;\n", {{"T", oneof_member_storage(emit, member)},
                               {"m", emit.names.local.at(member.field)}});
    }
    p.print("rp_$o$_union() noexcept {}\n", {{"o", o.oneof->name}});
    p.outdent();
    p.print("};\n");
}

void emit_oneof_accessors(const Emit& emit, const OneofPlan& o) {
    Printer& p = emit.printer;
    const std::string tag = emit.synth.case_tag.at(o.oneof);
    // The oneof reader, named after the oneof -- sanitized like any identifier (a keyword/reserved oneof
    // name is escaped), but NOT deduped against fields: a oneof name is already unique among the
    // message's fields. Pass one typed handler per member (and/or a single (auto, auto) catch-all); the
    // active member is dispatched to its handler, an unhandled member is ignored -- the same
    // combine/handles_one dispatch the streaming decoder uses, but invoked via invoke_handler: a
    // handler must return void (the message is already decoded -- nothing to abort), as does the
    // reader itself. A `[](std::monostate)` handler covers the unset state.
    p.print("template <class... RpFs> void $n$(RpFs&&... rp_fs) const {\n",
            {{"n", codegen::sanitize(o.oneof->name)}});
    p.indent();
    for (const OneofMemberPlan& member : o.members) {
        const std::string& id = emit.names.local.at(member.field);
        std::string tagref = tag;
        tagref += "::";
        tagref += id;
        std::string args = tagref;
        args += ", typename ";
        args += tagref;
        args += "::Value";
        std::string what = "oneof member '";
        what += id;
        what += '\'';
        std::string expected = tagref;
        expected += "::Value";
        codegen::emit_dispatch_guards(p, "RpFs", args, what, expected);
    }
    codegen::emit_dispatch_guards(p, "RpFs", "std::monostate",
                                  "a oneof's unset (std::monostate) state", "");
    // Per-handler stray guard: every handler must name one of THIS oneof's member tags (or
    // std::monostate, or be a catch-all). Catches a handler pasted from another oneof's reader,
    // which no per-member guard would ever see.
    std::string tags;
    for (const OneofMemberPlan& member : o.members) {
        tags += tag;
        tags += "::";
        tags += emit.names.local.at(member.field);
        tags += ", ";
    }
    tags += "std::monostate";
    p.print(
        "static_assert((true && ... && !::rapidproto::is_stray_handler<RpFs, $tags$>),"
        " \"a callback matches no member of oneof '$o$' (and is not a catch-all or the"
        " std::monostate unset handler)\");\n",
        {{"tags", tags}, {"o", o.oneof->name}});
    p.print("auto rp_d = ::rapidproto::combine(static_cast<RpFs&&>(rp_fs)...);\n");
    p.print("switch (m_rp_$o$_case) {\n", {{"o", o.oneof->name}});
    p.indent();
    int index = 1;
    for (const OneofMemberPlan& member : o.members) {
        const std::string& id = emit.names.local.at(member.field);
        std::string val;
        if (member.kind == FieldKind::PointerSubMsg) {
            val = "*";  // stored as a pointer -> hand over a const ref
        }
        val += "m_rp_";
        val += o.oneof->name;
        val += '.';
        val += id;
        if (member.kind == FieldKind::SsoString) {
            val += ".view()";
        }
        p.print("case $i$:\n", {{"i", std::to_string(index++)}});
        p.indent();
        p.print(
            "if constexpr ((false || ... ||"
            " ::rapidproto::handles_one<RpFs, $S$::$id$, typename $S$::$id$::Value>)) {\n",
            {{"S", tag}, {"id", id}});
        p.print("::rapidproto::invoke_handler(rp_d, $S$::$id${}, $val$);\n",
                {{"S", tag}, {"id", id}, {"val", val}});
        p.print("}\n");
        p.print("break;\n");
        p.outdent();
    }
    p.print("default:\n");
    p.indent();
    p.print("if constexpr ((false || ... || ::rapidproto::handles_one<RpFs, std::monostate>)) {\n");
    p.print("::rapidproto::invoke_handler(rp_d, std::monostate{});\n");
    p.print("}\n");
    p.print("break;\n");
    p.outdent();
    p.outdent();
    p.print("}\n");
    p.outdent();
    p.print("}\n");
}

// ── field accessors ──────────────────────────────────────────────────────────────────────────────
void emit_field_accessor(const Emit& emit, const MessageLayout& layout, const MemberPlan& m) {
    Printer& p = emit.printer;
    const std::string id = member_id(emit, m);
    // A scalar/enum/string field with explicit presence returns std::optional<T> (std::nullopt when
    // absent); an implicit-presence field returns the bare value. Message fields encode presence in their
    // `const T*` accessor's null return. No has_<f>() is emitted -- the optional carries presence.
    switch (m.kind) {
        case FieldKind::InlineScalar:
            if (m.is_bool && m.presence_bit >= 0) {
                p.print(
                    "std::optional<bool> $id$() const noexcept { return $p$ != 0 ?"
                    " std::optional<bool>($b$ != 0) : std::nullopt; }\n",
                    {{"id", id},
                     {"p", bit_test(layout, m.presence_bit)},
                     {"b", bit_test(layout, m.value_bit)}});
            } else if (m.is_bool) {
                p.print("bool $id$() const noexcept { return $b$ != 0; }\n",
                        {{"id", id}, {"b", bit_test(layout, m.value_bit)}});
            } else if (m.presence_bit >= 0) {
                p.print(
                    "std::optional<$T$> $id$() const noexcept { return $p$ != 0 ?"
                    " std::optional<$T$>(m_$id$) : std::nullopt; }\n",
                    {{"T", cpp_scalar(m.field->type_name)},
                     {"id", id},
                     {"p", bit_test(layout, m.presence_bit)}});
            } else {
                p.print("$T$ $id$() const noexcept { return m_$id$; }\n",
                        {{"T", cpp_scalar(m.field->type_name)}, {"id", id}});
            }
            break;
        case FieldKind::InlineEnum:
            if (m.presence_bit >= 0) {
                p.print(
                    "std::optional<$T$> $id$() const noexcept { return $p$ != 0 ?"
                    " std::optional<$T$>(m_$id$) : std::nullopt; }\n",
                    {{"T", cpp_type_name(emit.names, m.target_fqn)},
                     {"id", id},
                     {"p", bit_test(layout, m.presence_bit)}});
            } else {
                p.print("$T$ $id$() const noexcept { return m_$id$; }\n",
                        {{"T", cpp_type_name(emit.names, m.target_fqn)}, {"id", id}});
            }
            break;
        case FieldKind::SsoString:
            if (m.presence_bit >= 0) {
                p.print(
                    "std::optional<std::string_view> $id$() const noexcept { return $p$ != 0 ?"
                    " std::optional<std::string_view>(m_$id$.view()) : std::nullopt; }\n",
                    {{"id", id}, {"p", bit_test(layout, m.presence_bit)}});
            } else {
                p.print("std::string_view $id$() const noexcept { return m_$id$.view(); }\n",
                        {{"id", id}});
            }
            break;
        case FieldKind::InlineFixedSubMsg:
            if (m.presence_bit >= 0) {
                p.print(
                    "const $T$* $id$() const noexcept { return $b$ != 0 ? &m_$id$ : nullptr; }\n",
                    {{"T", cpp_type_name(emit.names, m.target_fqn)},
                     {"id", id},
                     {"b", bit_test(layout, m.presence_bit)}});
            } else {  // required: always present
                p.print("const $T$* $id$() const noexcept { return &m_$id$; }\n",
                        {{"T", cpp_type_name(emit.names, m.target_fqn)}, {"id", id}});
            }
            break;
        case FieldKind::PointerSubMsg:
            p.print("const $T$* $id$() const noexcept { return m_$id$; }\n",
                    {{"T", cpp_type_name(emit.names, m.target_fqn)}, {"id", id}});
            break;
        case FieldKind::Repeated:
            if (repeated_elem_type(emit, *m.field) == "::rapidproto::ArenaString") {
                // Storage is ArrayView<ArenaString> (SSO); expose std::string_view, not the storage type.
                p.print(
                    "::rapidproto::StringArrayView $id$() const noexcept {"
                    " return ::rapidproto::StringArrayView(m_$id$); }\n",
                    {{"id", id}});
            } else {
                p.print("$T$ $id$() const noexcept { return m_$id$; }\n",
                        {{"T", storage_type(emit, m)}, {"id", id}});
            }
            break;
        case FieldKind::Map:
            p.print("$T$ $id$() const noexcept { return m_$id$; }\n",
                    {{"T", storage_type(emit, m)}, {"id", id}});
            break;
        case FieldKind::Raw:
            // The message field's arena-copied payload(s): hand a view to the field type's own
            // decode() when (and if) the tree is wanted. Presence follows the accessor
            // conventions -- optional for explicit presence (null data = absent; a present empty
            // payload is non-null, so no mask bit is spent), bare for required (always present
            // after a successful decode), element count for repeated.
            if (m.field->presence == FieldPresence::Explicit && !m.field->is_repeated) {
                p.print(
                    "std::optional<::rapidproto::ByteView> $id$() const noexcept {"
                    " return m_$id$.data() != nullptr ?"
                    " std::optional<::rapidproto::ByteView>(m_$id$) : std::nullopt; }\n",
                    {{"id", id}});
            } else {
                p.print("$T$ $id$() const noexcept { return m_$id$; }\n",
                        {{"T", storage_type(emit, m)}, {"id", id}});
            }
            break;
    }
}

// ── storage ──────────────────────────────────────────────────────────────────────────────────────
void emit_storage(const Emit& emit, const MessageLayout& layout) {
    std::vector<std::pair<std::size_t, std::string>> slots;
    for (const MemberPlan& m : layout.members) {
        if (m.size > 0) {
            slots.emplace_back(m.offset, storage_type(emit, m) + " m_" + member_id(emit, m) + ";");
        }
    }
    for (const OneofPlan& o : layout.oneofs) {
        slots.emplace_back(o.disc_offset, "std::uint8_t m_rp_" + o.oneof->name + "_case;");
        slots.emplace_back(o.union_offset,
                           "rp_" + o.oneof->name + "_union m_rp_" + o.oneof->name + ";");
    }
    if (layout.mask_size > 0) {
        const std::string decl =
            layout.mask_size > 8
                ? "std::uint64_t m_rp_mask[" + std::to_string(layout.mask_size / 8) + "];"
                : mask_word_type(layout.mask_size) + " m_rp_mask;";
        slots.emplace_back(layout.mask_offset, decl);
    }
    std::stable_sort(slots.begin(), slots.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [off, decl] : slots) {
        emit.printer.print("$d$\n", {{"d", decl}});
    }
}

// ── message ──────────────────────────────────────────────────────────────────────────────────────

// Types that must be visible before `message`'s subtree is emitted, split by HOW visible they must be:
//  - `complete`: a sub-message inlined by value -- the exact type must already be DEFINED.
//  - `enclosing`: a pointer / repeated / map / oneof sub-message, or any referenced enum -- the type
//    only needs to be NAMEABLE. A direct sibling is named through its forward declaration, but a type
//    NESTED inside a sibling (`Sibling::Inner`) can only be named once that sibling is COMPLETE. So an
//    `enclosing` entry forces ordering only when nested under a sibling, never for a direct-sibling
//    target -- which is what keeps mutually-referential siblings compiling via their forward decls.
void collect_must_precede(const Emit& emit, const MessageNode& message,
                          std::unordered_set<std::string>& complete,
                          std::unordered_set<std::string>& enclosing) {
    const MessageLayout& layout = *emit.layouts.find(message.fqn);
    const auto classify = [&](FieldKind kind, const std::string& fqn) {
        if (fqn.empty()) {
            return;
        }
        switch (kind) {
            case FieldKind::InlineFixedSubMsg:  // inlined by value -> the type must be complete
                complete.insert(fqn);
                break;
            case FieldKind::PointerSubMsg:
            case FieldKind::Repeated:    // ArrayView<T>: T (message or enum) must be nameable
            case FieldKind::InlineEnum:  // a nested enum needs its enclosing message complete
                enclosing.insert(fqn);
                break;
            default:
                break;
        }
    };
    for (const MemberPlan& m : layout.members) {
        classify(m.kind, m.target_fqn);
        if (m.kind == FieldKind::Map && m.entry.has_value()) {
            classify(m.entry->value_kind, m.entry->value_fqn);  // map key is always scalar
        }
    }
    for (const OneofPlan& o : layout.oneofs) {
        for (const OneofMemberPlan& member : o.members) {
            classify(member.kind, member.target_fqn);
        }
    }
    for (const MessageNode& nested : message.nested_messages) {
        collect_must_precede(emit, nested, complete, enclosing);
    }
}

// Order sibling messages so every type that must be visible here is emitted first. Acyclic for valid
// schemas; the active-set guard breaks any cycle, which only an inherently-uncompilable schema can form
// (two siblings each naming a type nested in the other). DFS post-order over the "B must precede A" edges.
std::vector<const MessageNode*> ordered_siblings(const Emit& emit,
                                                 const std::vector<MessageNode>& siblings) {
    std::vector<std::unordered_set<std::string>> complete(siblings.size());
    std::vector<std::unordered_set<std::string>> enclosing(siblings.size());
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        collect_must_precede(emit, siblings[i], complete[i], enclosing[i]);
    }
    const auto depends_on = [&](std::size_t a, std::size_t b) {  // must B precede A?
        const std::string& root = siblings[b].fqn;
        const auto under = [&](const std::string& t) {  // t is strictly nested under B
            return t.size() > root.size() && t.compare(0, root.size(), root) == 0 &&
                   t[root.size()] == '.';
        };
        // A by-value target needs B whether it IS B or is nested under it; a nameable target needs B
        // only when nested under it (a direct-sibling target is covered by B's forward declaration).
        return std::any_of(complete[a].begin(), complete[a].end(),
                           [&](const std::string& t) { return t == root || under(t); }) ||
               std::any_of(enclosing[a].begin(), enclosing[a].end(), under);
    };
    return codegen::topo_order_siblings(siblings, depends_on);
}

void emit_message(const Emit& emit, const MessageNode& message);  // recursion

// Index a layout's members by their field/map node pointer, for accessor/arm lookup by node.
std::unordered_map<const void*, const MemberPlan*> by_node_map(const MessageLayout& layout) {
    std::unordered_map<const void*, const MemberPlan*> by_node;
    for (const MemberPlan& m : layout.members) {
        by_node.emplace(m.field != nullptr ? static_cast<const void*>(m.field)
                                           : static_cast<const void*>(m.map_field),
                        &m);
    }
    return by_node;
}

void emit_message_body(const Emit& emit, const MessageNode& message) {
    Printer& p = emit.printer;
    const MessageLayout& layout = *emit.layouts.find(message.fqn);
    const std::string type = emit.names.local.at(&message);

    p.print("class $T$ {\n", {{"T", type}});
    p.print(" public:\n");
    p.indent();

    for (const EnumNode& nested : message.enums) {
        codegen::emit_enum(emit.printer, emit.names, nested, false);
    }
    // Forward-declare nested messages first, so any sibling cross-reference -- a pointer, repeated, map
    // value, or oneof member, and even a cycle -- names a declared type regardless of definition order.
    // Nested types can only be forward-declared here, inside the enclosing class (top-level messages get
    // file-scope forward declarations instead). Inline-by-value members still need the full definition
    // first, which ordered_siblings provides.
    for (const MessageNode& nested : message.nested_messages) {
        p.print("class $N$;\n", {{"N", emit.names.local.at(&nested)}});
    }
    for (const MessageNode* nested : ordered_siblings(emit, message.nested_messages)) {
        emit_message(emit, *nested);
    }
    for (const OneofPlan& o : layout.oneofs) {
        emit_oneof_visit_tags(emit, o);
    }
    for (const MemberPlan& m : layout.members) {
        if (m.kind == FieldKind::Map) {
            emit_map_entry(emit, m, type);
        }
    }

    // Accessors in declaration order (fields, then maps, then oneofs). A profile-DROPPED field
    // has no member and gets NO accessor: touching it is a compile error, the loud failure mode.
    const std::unordered_map<const void*, const MemberPlan*> by_node = by_node_map(layout);
    for (const FieldNode& field : message.fields) {
        if (const auto it = by_node.find(&field); it != by_node.end()) {
            emit_field_accessor(emit, layout, *it->second);
        }
    }
    for (const MapFieldNode& map : message.map_fields) {
        if (const auto it = by_node.find(&map); it != by_node.end()) {
            emit_field_accessor(emit, layout, *it->second);
        }
    }
    for (const OneofPlan& o : layout.oneofs) {
        emit_oneof_accessors(emit, o);
    }
    if (layout.unknown_bit >= 0) {  // --unknown-present: a per-message "saw an unknown field" flag
        p.print(
            "bool $h$() const noexcept { return $b$ != 0; }\n",
            {{"h", emit.synth.unknown.at(&message)}, {"b", bit_test(layout, layout.unknown_bit)}});
    }

    // decode() materializes the whole tree in `arena`; the private rp_decode_into (the wire loop) fills
    // an already-allocated node. Both are defined out-of-line, after every class shell, so all
    // referenced field types are complete (handles forward + cyclic references). rp_decode_into is
    // reached only through the ::rapidproto::arena_detail::decode_into forwarder, befriended below.
    p.print(
        "[[nodiscard]] static const $T$* decode(::rapidproto::ByteView input, ::rapidproto::Arena&"
        " arena, ::rapidproto::ArenaDecodeError* err = nullptr) noexcept;\n",
        {{"T", type}});

    p.outdent();
    p.print(" private:\n");
    p.indent();
    p.print(
        "template <class RpT> friend bool ::rapidproto::arena_detail::decode_into(RpT&,"
        " ::rapidproto::ByteView, ::rapidproto::Arena&, int,"
        " ::rapidproto::ArenaDecodeError*) noexcept;\n");
    p.print(
        "static bool rp_decode_into($T$& out, ::rapidproto::ByteView body, ::rapidproto::Arena& "
        "arena,"
        " int depth, ::rapidproto::ArenaDecodeError* err) noexcept;\n",
        {{"T", type}});
    for (const OneofPlan& o : layout.oneofs) {
        emit_oneof_union(emit, o);
    }
    emit_storage(emit, layout);
    p.outdent();
    p.print("};\n");
}

void emit_message(const Emit& emit, const MessageNode& message) {
    emit_message_body(emit, message);
}

// Assign every synthesized identifier for `message` a name unique within its class scope (seeded
// with the already-deduped field/map/nested-type names), appending `_` on collision.
void synth_for_message(const CppNameTable& names, const LayoutSet& layouts,
                       const MessageNode& message, SynthNames& out) {
    const MessageLayout& layout = *layouts.find(message.fqn);
    std::unordered_set<std::string> taken;
    for (const EnumNode& e : message.enums) {
        taken.insert(names.local.at(&e));
    }
    for (const MessageNode& n : message.nested_messages) {
        taken.insert(names.local.at(&n));
    }
    for (const FieldNode& f : message.fields) {
        taken.insert(names.local.at(&f));
    }
    for (const MapFieldNode& mp : message.map_fields) {
        taken.insert(names.local.at(&mp));
    }
    for (const OneofNode& o : message.oneofs) {
        for (const FieldNode& f : o.fields) {
            taken.insert(names.local.at(&f));
        }
    }
    taken.insert("decode");
    const auto dedup = [&taken](std::string name) {
        while (!taken.insert(name).second) {
            name += '_';
        }
        return name;
    };
    for (const MemberPlan& m : layout.members) {
        if (m.kind == FieldKind::Map) {
            out.entry_type[m.map_field] = dedup(capitalize(names.local.at(m.map_field)) + "Entry");
        }
    }
    for (const OneofPlan& o : layout.oneofs) {
        out.case_tag[o.oneof] = dedup(capitalize(o.oneof->name));  // visit-tag struct, e.g. "Pick"
    }
    if (layout.unknown_bit >= 0) {
        out.unknown[&message] = dedup("has_unknown_fields");
    }
    for (const MessageNode& n : message.nested_messages) {
        synth_for_message(names, layouts, n, out);
    }
}

// ── parse emission ───────────────────────────────────────────────────────────────────────────────

// A scalar proto type -> the shared wire/conversion facts (wire enumerator, read method, value
// conversion) from the codegen table (rapidproto/codegen/wire.hpp). Returns the table's view directly;
// its string_view fields feed straight into the Printer (which binds string_view). Precondition:
// `type` is a scalar keyword (asserted; resolution/analysis guarantees it before codegen).
const codegen::ScalarWire& scalar_wire(std::string_view type) {
    const codegen::ScalarWire* w = codegen::find_scalar_wire(type);
    assert(w != nullptr && "scalar_wire called on a non-scalar type");
    return *w;
}

// {wire enumerator, read expression} for a message field given its encoding (length-prefixed vs the
// group/delimited wire form).
std::pair<std::string, std::string> message_wire(const FieldNode& field) {
    if (field.message_encoding == MessageEncoding::Delimited) {
        return {"SGroup", "read_group(rp_tag.field_number)"};
    }
    return {"Len", "read_length_delimited()"};
}

std::string mask_word_one(const MessageLayout& layout, int bit) {
    const bool multi = layout.mask_size > kWordBytes;
    const std::string word = multi ? "std::uint64_t" : mask_word_type(layout.mask_size);
    return word + "{1} << " + std::to_string(multi ? bit % kWordBits : bit);
}
std::string mask_word_ref(const MessageLayout& layout, int bit) {
    return layout.mask_size > kWordBytes ? "out.m_rp_mask[" + std::to_string(bit / kWordBits) + "]"
                                         : "out.m_rp_mask";
}
std::string set_bit_stmt(const MessageLayout& layout, int bit) {
    const std::string word =
        layout.mask_size > kWordBytes ? "std::uint64_t" : mask_word_type(layout.mask_size);
    const std::string ref = mask_word_ref(layout, bit);
    return ref + " = static_cast<" + word + ">(" + ref + " | (" + mask_word_one(layout, bit) +
           "));";
}
std::string clear_bit_stmt(const MessageLayout& layout, int bit) {
    const std::string word =
        layout.mask_size > kWordBytes ? "std::uint64_t" : mask_word_type(layout.mask_size);
    const std::string ref = mask_word_ref(layout, bit);
    return ref + " = static_cast<" + word + ">(" + ref + " & static_cast<" + word + ">(~(" +
           mask_word_one(layout, bit) + ")));";
}

// The transient required-presence bitmask `rp_req` is a single uint64 for <=64 required fields, else
// a uint64[] (a message can legally have more than 64 required fields).
std::string req_word_ref(int index, std::size_t total) {
    return total > kWordBits ? "rp_req[" + std::to_string(index / kWordBits) + "]" : "rp_req";
}
int req_bit_no(int index, std::size_t total) {
    return total > kWordBits ? index % kWordBits : index;
}

// Forward declarations: the value-threaded read emitters live with the map-entry emitter below.
void emit_vt_scalar_read(const Emit& emit, FieldKind kind, std::string_view proto_type,
                         const std::string& enum_fqn, const std::string& target,
                         const std::string& cur, const std::string& end, const std::string& beg);
void emit_vt_value_read(const Emit& emit, const FieldNode& field, const std::string& target,
                        const std::string& cur, const std::string& end, const std::string& beg);
void emit_vt_len_read(const Emit& emit, const std::string& view);
void emit_vt_message_read(const Emit& emit, const FieldNode& field, const std::string& view);

std::string elem_wire_enum(const FieldNode& field) {  // the native wire type of a repeated element
    if (field.is_message_type) {
        return message_wire(field).first;
    }
    if (field.is_enum_type) {
        return "Varint";
    }
    if (field.type_name == "string" || field.type_name == "bytes") {
        return "Len";
    }
    return std::string(scalar_wire(field.type_name).wire);
}

// Set the presence bit (Explicit) or the transient required bit; nothing for Implicit/pointer.
void emit_presence_set(const Emit& emit, const MessageLayout& layout, const MemberPlan& m,
                       const std::unordered_map<const FieldNode*, int>& required_bit) {
    if (m.presence_bit >= 0) {
        emit.printer.print("$s$\n", {{"s", set_bit_stmt(layout, m.presence_bit)}});
    } else if (m.field != nullptr) {
        const auto it = required_bit.find(m.field);
        if (it != required_bit.end()) {
            emit.printer.print(
                "$ref$ |= std::uint64_t{1} << $b$;\n",
                {{"ref", req_word_ref(it->second, required_bit.size())},
                 {"b", std::to_string(req_bit_no(it->second, required_bit.size()))}});
        }
    }
}

// The decode body of a singular LEN sub-message (reject-if-seen, read the payload, recursively decode
// into the inline struct or an arena-allocated pointee, set presence) -- TAG-CONSUMED: no wire check,
// no `case`, no `continue`. Shared by the general singular arm and the expected-order threaded label.
void emit_message_decode_body(const Emit& emit, const MessageLayout& layout, const MemberPlan& m,
                              const std::unordered_map<const FieldNode*, int>& required_bit) {
    Printer& p = emit.printer;
    const FieldNode& field = *m.field;
    const std::string id = emit.names.local.at(&field);
    // "Already present" reuses existing state: a null pointer, the presence bit, or the transient
    // required bit. A singular sub-message appearing more than once on the wire would, in protoc,
    // merge; a read-only arena tree does not implement merge, so it rejects the (exotic,
    // concatenation-style) input rather than silently take the last (or a partial merge).
    std::string seen;
    if (m.kind == FieldKind::PointerSubMsg) {
        seen = "out.m_" + id + " != nullptr";
    } else if (m.presence_bit >= 0) {
        seen = "(" + mask_word_ref(layout, m.presence_bit) + " & (" +
               mask_word_one(layout, m.presence_bit) + ")) != 0";
    } else {  // a required inline-fixed sub-message (no resting presence bit)
        const int ri = required_bit.at(m.field);
        seen = "(" + req_word_ref(ri, required_bit.size()) + " & (std::uint64_t{1} << " +
               std::to_string(req_bit_no(ri, required_bit.size())) + ")) != 0";
    }
    const std::string sub = cpp_type_name(emit.names, m.target_fqn);
    p.print("if ($seen$) { ::rapidproto::rp_fail_repeated_singular(err, $n$); return false; }\n",
            {{"seen", seen}, {"n", std::to_string(field.number)}});
    emit_vt_message_read(emit, field, "rp_v");
    if (m.kind == FieldKind::InlineFixedSubMsg) {
        p.print(
            "if (!::rapidproto::arena_detail::decode_into(out.m_$id$, rp_v, arena, depth + 1, "
            "err)) { return false; }\n",
            {{"id", id}});
    } else {
        p.print("$S$* const rp_sub = arena.create<$S$>();\n", {{"S", sub}});
        p.print("if (rp_sub == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
        p.print(
            "if (!::rapidproto::arena_detail::decode_into(*rp_sub, rp_v, arena, depth + 1, "
            "err)) { return false; }\n");
        p.print("out.m_$id$ = rp_sub;\n", {{"id", id}});
    }
    emit_presence_set(emit, layout, m, required_bit);
}

// A singular field's switch case (scalar/enum/string/bool/inline-or-pointer message).
void emit_singular_arm(const Emit& emit, const MessageLayout& layout, const MemberPlan& m,
                       const std::unordered_map<const FieldNode*, int>& required_bit) {
    Printer& p = emit.printer;
    const FieldNode& field = *m.field;
    const std::string id = emit.names.local.at(&field);
    p.print("case $n$: {\n", {{"n", std::to_string(field.number)}});
    p.indent();
    if (m.kind == FieldKind::InlineScalar && m.is_bool) {
        p.print("if (rp_tag.wire_type == ::rapidproto::WireType::Varint) {\n");
        p.indent();
        p.print("std::uint64_t rp_raw = 0;\n");
        p.print(
            "const std::uint8_t* const rp_np = ::rapidproto::wire::read_varint(rp_c, rp_cend,"
            " &rp_raw, &rp_we);\n");
        p.print(
            "if (rp_np == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we,"
            " static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(body))); return false; "
            "} rp_c "
            "= rp_np;\n");
        p.print("if (::rapidproto::varint_to_bool(rp_raw)) { $set$ } else { $clr$ }\n",
                {{"set", set_bit_stmt(layout, m.value_bit)},
                 {"clr", clear_bit_stmt(layout, m.value_bit)}});
        emit_presence_set(emit, layout, m, required_bit);
        p.print("continue;\n");
        p.outdent();
        p.print("}\n");
    } else if (m.kind == FieldKind::InlineScalar || m.kind == FieldKind::InlineEnum ||
               m.kind == FieldKind::SsoString) {
        std::string wire;  // scalar_wire is only valid for an actual scalar keyword
        if (m.kind == FieldKind::SsoString) {
            wire = "Len";
        } else if (m.kind == FieldKind::InlineEnum) {
            wire = "Varint";
        } else {
            wire = scalar_wire(field.type_name).wire;
        }
        p.print("if (rp_tag.wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", wire}});
        p.indent();
        emit_vt_value_read(emit, field, "out.m_" + id, "rp_c", "rp_cend",
                           "::rapidproto::wire::byte_ptr(body)");
        emit_presence_set(emit, layout, m, required_bit);
        p.print("continue;\n");
        p.outdent();
        p.print("}\n");
    } else if (m.kind == FieldKind::InlineFixedSubMsg || m.kind == FieldKind::PointerSubMsg) {
        const std::string wire = message_wire(field).first;
        p.print("if (rp_tag.wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", wire}});
        p.indent();
        emit_message_decode_body(emit, layout, m, required_bit);
        p.print("continue;\n");
        p.outdent();
        p.print("}\n");
    }
    p.print("break;\n");
    p.outdent();
    p.print("}\n");
}

void emit_growable_setup(const Emit& emit, const std::string& id, const std::string& elem);

// A repeated field accumulates into a single-pass arena-growable array (geometric realloc-and-copy).
// Shares the array + grow-and-return-slot emission with maps via emit_growable_setup.
void emit_repeated_setup(const Emit& emit, const FieldNode& field) {
    emit_growable_setup(emit, emit.names.local.at(&field), repeated_elem_type(emit, field));
}

// Read one element (from `reader`) and append it to the field's accumulator.
void emit_repeated_element(const Emit& emit, const FieldNode& field) {
    Printer& p = emit.printer;
    const std::string id = emit.names.local.at(&field);
    p.print("$E$* const rp_slot = rp_slot_$id$();\n",
            {{"E", repeated_elem_type(emit, field)}, {"id", id}});
    p.print("if (rp_slot == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
    if (field.is_message_type) {
        const std::string sub = cpp_type_name(emit.names, field.resolved_type_fqn);
        emit_vt_message_read(emit, field, "rp_v");
        p.print("*rp_slot = $S${};\n", {{"S", sub}});
        p.print(
            "if (!::rapidproto::arena_detail::decode_into(*rp_slot, rp_v, arena, depth + 1, err)) "
            "{ return false; }\n");
    } else {
        emit_vt_value_read(emit, field, "*rp_slot", "rp_c", "rp_cend",
                           "::rapidproto::wire::byte_ptr(body)");
    }
}

// Packed scalar fill: reserve the element count up front from the packed span -- exact for fixed-
// width elements (span.size()/width), an upper bound for varints (>=1 byte each, so span.size()) --
// then fill without a per-element capacity check or geometric grow (which the shared growable
// accumulator does on the expanded path). For the varint upper-bound case the array is trimmed to
// its exact length afterward: the fill allocates nothing, so it is still the arena's last allocation
// (Arena::shrink_last), and rp_cap is reset so a later occurrence of the same field re-grows. `rp_p`
// (the packed span) is already read by the caller.
void emit_packed_fill(const Emit& emit, const FieldNode& field) {
    Printer& p = emit.printer;
    const std::string id = emit.names.local.at(&field);
    const std::string elem = repeated_elem_type(emit, field);
    const std::string wire = elem_wire_enum(field);  // Varint / I32 / I64
    const bool varint = wire == "Varint";
    // Element-count bound from the packed byte length: exact for fixed-width (span/width), an upper
    // bound for varints (>=1 byte each, so span.size()).
    if (varint) {
        // Packed varint fill. The array grow, the SMALL-span byte-loop (the common repeated-field case),
        // and the trim stay INLINE here; only the ~2k-instruction SWAR kernel for a LARGE span is a
        // shared out-of-line call (arena_detail::decode_packed_varints_large). That split keeps decode()
        // from carrying a kernel copy per packed field -- code size and gcc register pressure no longer
        // grow with the field count -- yet tiny arrays pay no call and no kernel-setup overhead. `acc`'s
        // address never escapes (the kernel takes `acc + n` by value), so neither compiler spills it. The
        // per-element conversion is a NAMED functor so the template instantiates once per proto type
        // (shared across fields and TUs), not once per field; enums use conv_enum<TheEnum> (a cast --
        // open-enum semantics, any value stored as-is, identical to the per-element read). 256 is
        // decode_packed_varints's own kernel-vs-byte-loop threshold, so the kernels always engage in the
        // out-of-line arm (a sub-256 span would only run the byte-loop tail, done inline below).
        const std::string conv = field.is_enum_type
                                     ? "::rapidproto::wire::conv_enum<" + elem + ">"
                                     : std::string(scalar_wire(field.type_name).packed_conv);
        // Grow the array to fit the span's element upper bound (>=1 byte/element, so span.size()).
        p.print("const std::size_t rp_ub = rp_p.size();\n");
        p.print("if (rp_ub != 0 && rp_cap_$id$ < rp_n_$id$ + rp_ub) {\n", {{"id", id}});
        p.indent();
        p.print("const std::size_t rp_nc = rp_n_$id$ + rp_ub;\n", {{"id", id}});
        p.print("$E$* const rp_nb = arena.allocate_array<$E$>(rp_nc);\n", {{"E", elem}});
        p.print("if (rp_nb == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
        p.print(
            "for (std::size_t rp_i = 0; rp_i < rp_n_$id$; ++rp_i) { rp_nb[rp_i] = "
            "rp_acc_$id$[rp_i]; "
            "}\n",
            {{"id", id}});
        p.print("rp_acc_$id$ = rp_nb;\n", {{"id", id}});
        p.print("rp_cap_$id$ = rp_nc;\n", {{"id", id}});
        p.outdent();
        p.print("}\n");
        p.print("const std::uint8_t* rp_vp = ::rapidproto::wire::byte_ptr(rp_p);\n");
        p.print("const std::uint8_t* const rp_ve = rp_vp + rp_p.size();\n");
        p.print("if (rp_p.size() >= 256) {\n");
        p.indent();
        p.print(
            "const std::size_t rp_dc = ::rapidproto::arena_detail::decode_packed_varints_large<$E$,"
            " $conv$>(rp_vp, rp_ve, rp_acc_$id$ + rp_n_$id$, err);\n",
            {{"E", elem}, {"id", id}, {"conv", conv}});
        p.print("if (rp_dc == static_cast<std::size_t>(-1)) { return false; }\n");
        p.print("rp_n_$id$ += rp_dc;\n", {{"id", id}});
        p.outdent();
        p.print("} else {\n");
        p.indent();
        // Small span: the tuned byte-loop tail (no kernel), inlined -- the common repeated-field shape.
        p.print(
            "const std::size_t rp_dc = ::rapidproto::arena_detail::decode_packed_varints_small<$E$,"
            " $conv$>(rp_vp, rp_ve, rp_acc_$id$ + rp_n_$id$, err);\n",
            {{"E", elem}, {"id", id}, {"conv", conv}});
        p.print("if (rp_dc == static_cast<std::size_t>(-1)) { return false; }\n");
        p.print("rp_n_$id$ += rp_dc;\n", {{"id", id}});
        p.outdent();
        p.print("}\n");
        p.print(
            "arena.shrink_last(rp_acc_$id$, rp_cap_$id$ * sizeof($E$), rp_n_$id$ * sizeof($E$));\n",
            {{"id", id}, {"E", elem}});
        p.print("rp_cap_$id$ = rp_n_$id$;\n", {{"id", id}});
        return;
    }
    // Element-count bound from the packed byte length: exact for fixed-width (span/width).
    const char* div = "";
    if (wire == "I32") {
        div = " / 4";
    } else if (wire == "I64") {
        div = " / 8";
    }
    p.print("const std::size_t rp_ub = rp_p.size()$d$;\n", {{"d", div}});
    p.print("if (rp_ub != 0 && rp_cap_$id$ < rp_n_$id$ + rp_ub) {\n", {{"id", id}});
    p.indent();
    p.print("const std::size_t rp_nc = rp_n_$id$ + rp_ub;\n", {{"id", id}});
    p.print("$E$* const rp_nb = arena.allocate_array<$E$>(rp_nc);\n", {{"E", elem}});
    p.print("if (rp_nb == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
    p.print(
        "for (std::size_t rp_i = 0; rp_i < rp_n_$id$; ++rp_i) { rp_nb[rp_i] = rp_acc_$id$[rp_i]; "
        "}\n",
        {{"id", id}});
    p.print("rp_acc_$id$ = rp_nb;\n", {{"id", id}});
    p.print("rp_cap_$id$ = rp_nc;\n", {{"id", id}});
    p.outdent();
    p.print("}\n");
    // Fixed-width elements are little-endian on the wire, so on a little-endian host the packed span IS
    // the array's byte image: fill it in one memcpy (protoc does the same) instead of a per-element read.
    // Only for a whole-multiple span; a trailing partial (malformed packed) falls through to the
    // per-element loop, which reports the exact truncation error. A big-endian host also takes the
    // per-element (byte-swapping) path.
    p.print("if constexpr (::rapidproto::arena_detail::kFixedIsNativeLE) {\n");
    p.indent();
    p.print("if (rp_p.size() % sizeof($E$) == 0) {\n", {{"E", elem}});
    p.indent();
    p.print(
        "if (rp_ub != 0) { std::memcpy(rp_acc_$id$ + rp_n_$id$, rp_p.data(), rp_p.size()); "
        "}\n",
        {{"id", id}});
    p.print("rp_n_$id$ += rp_ub;\n", {{"id", id}});
    p.print("continue;\n");
    p.outdent();
    p.print("}\n");
    p.outdent();
    p.print("}\n");
    // Packed fixed (big-endian host / partial-span fallback): per-element read from the span, cursor
    // threaded by value.
    p.print("const std::uint8_t* rp_vp = ::rapidproto::wire::byte_ptr(rp_p);\n");
    p.print("const std::uint8_t* const rp_vbeg = rp_vp;\n");
    p.print("const std::uint8_t* const rp_ve = rp_vp + rp_p.size();\n");
    p.print("while (rp_vp < rp_ve) {\n");
    p.indent();
    emit_vt_value_read(emit, field, "rp_acc_" + id + "[rp_n_" + id + "]", "rp_vp", "rp_ve",
                       "rp_vbeg");
    p.print("++rp_n_$id$;\n", {{"id", id}});
    p.outdent();
    p.print("}\n");
}

void emit_repeated_arm(const Emit& emit, const FieldNode& field) {
    Printer& p = emit.printer;
    const std::string elem_wire = elem_wire_enum(field);
    const bool packable = codegen::is_packable_wire(elem_wire);
    p.print("case $n$: {\n", {{"n", std::to_string(field.number)}});
    p.indent();
    p.print("if (rp_tag.wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", elem_wire}});
    p.indent();
    emit_repeated_element(emit, field);
    p.print("continue;\n");
    p.outdent();
    p.print("}\n");
    if (packable) {
        p.print("if (rp_tag.wire_type == ::rapidproto::WireType::Len) {\n");
        p.indent();
        emit_vt_len_read(emit, "rp_p");
        emit_packed_fill(emit, field);
        p.print("continue;\n");
        p.outdent();
        p.print("}\n");
    }
    p.print("break;\n");
    p.outdent();
    p.print("}\n");
}

void emit_repeated_finalize(const Emit& emit, const FieldNode& field) {
    const std::string id = emit.names.local.at(&field);
    emit.printer.print("out.m_$id$ = ::rapidproto::ArrayView<$E$>(rp_acc_$id$, rp_n_$id$);\n",
                       {{"id", id}, {"E", repeated_elem_type(emit, field)}});
}

// ── field-modes `raw` (see modes.hpp): store the message field's payload, decode later ──────────

// Only a repeated raw member needs setup: the growable array of per-element payload views. A
// singular raw payload writes straight into its member.
void emit_raw_setup(const Emit& emit, const MemberPlan& m) {
    if (m.field->is_repeated) {
        emit_growable_setup(emit, emit.names.local.at(m.field), "::rapidproto::ByteView");
    }
}

// The raw arm mirrors its materialized counterpart exactly -- same wire-type guard (a mismatched
// record falls to the shared skip), same RepeatedSingularMessage rejection, same presence/required
// bits -- but instead of decoding the payload it arena-copies it, deferring the decode to whenever
// the consumer hands the view to the field type's own decode(). read_length_delimited/read_group
// both yield exactly that payload (a group's body carries no SGROUP/EGROUP framing).
void emit_raw_arm(const Emit& emit, const MessageLayout& layout, const MemberPlan& m,
                  const std::unordered_map<const FieldNode*, int>& required_bit) {
    Printer& p = emit.printer;
    const FieldNode& field = *m.field;
    const std::string id = emit.names.local.at(&field);
    const std::string wire = message_wire(field).first;
    p.print("case $n$: {\n", {{"n", std::to_string(field.number)}});
    p.indent();
    p.print("if (rp_tag.wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", wire}});
    p.indent();
    if (!field.is_repeated) {
        // "Already present" is the stored view's non-null data -- the same state the accessor
        // reads, mirroring the materialized pointer arm's null check.
        p.print(
            "if (out.m_$id$.data() != nullptr) {"
            " ::rapidproto::rp_fail_repeated_singular(err, $n$); return false; }\n",
            {{"id", id}, {"n", std::to_string(field.number)}});
    }
    emit_vt_message_read(emit, field, "rp_v");
    if (field.is_repeated) {
        p.print("::rapidproto::ByteView* const rp_slot = rp_slot_$id$();\n", {{"id", id}});
        p.print("if (rp_slot == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
        p.print(
            "if (!::rapidproto::arena_detail::copy_payload(rp_v, arena, *rp_slot)) {"
            " ::rapidproto::rp_fail_oom(err); return false; }\n");
    } else {
        p.print(
            "if (!::rapidproto::arena_detail::copy_payload(rp_v, arena, out.m_$id$)) {"
            " ::rapidproto::rp_fail_oom(err); return false; }\n",
            {{"id", id}});
    }
    emit_presence_set(emit, layout, m, required_bit);
    p.print("continue;\n");
    p.outdent();
    p.print("}\n");
    p.print("break;\n");
    p.outdent();
    p.print("}\n");
}

void emit_raw_finalize(const Emit& emit, const MemberPlan& m) {
    if (m.field->is_repeated) {
        const std::string id = emit.names.local.at(m.field);
        emit.printer.print(
            "out.m_$id$ = ::rapidproto::ArrayView<::rapidproto::ByteView>(rp_acc_$id$, "
            "rp_n_$id$);\n",
            {{"id", id}});
    }
}

// A growable arena array {acc, n, cap} + a grow-and-return-slot lambda (shared by repeated and map).
void emit_growable_setup(const Emit& emit, const std::string& id, const std::string& elem) {
    Printer& p = emit.printer;
    p.print("$E$* rp_acc_$id$ = nullptr;\n", {{"E", elem}, {"id", id}});
    p.print("std::size_t rp_n_$id$ = 0;\n", {{"id", id}});
    p.print("std::size_t rp_cap_$id$ = 0;\n", {{"id", id}});
    p.print("const auto rp_slot_$id$ = [&]() noexcept -> $E$* {\n", {{"E", elem}, {"id", id}});
    p.indent();
    p.print("if (rp_n_$id$ == rp_cap_$id$) {\n", {{"id", id}});
    p.indent();
    p.print("const std::size_t rp_nc = rp_cap_$id$ == 0 ? std::size_t{4} : rp_cap_$id$ * 2;\n",
            {{"id", id}});
    p.print("$E$* const rp_nb = arena.allocate_array<$E$>(rp_nc);\n", {{"E", elem}});
    p.print("if (rp_nb == nullptr) { return nullptr; }\n");
    p.print(
        "for (std::size_t rp_i = 0; rp_i < rp_n_$id$; ++rp_i) { rp_nb[rp_i] = rp_acc_$id$[rp_i]; "
        "}\n",
        {{"id", id}});
    p.print("rp_acc_$id$ = rp_nb;\n", {{"id", id}});
    p.print("rp_cap_$id$ = rp_nc;\n", {{"id", id}});
    p.outdent();
    p.print("}\n");
    p.print("return &rp_acc_$id$[rp_n_$id$++];\n", {{"id", id}});
    p.outdent();
    p.print("};\n");
}

void emit_map_setup(const Emit& emit, const MemberPlan& m) {
    emit_growable_setup(emit, emit.names.local.at(m.map_field),
                        emit.synth.entry_type.at(m.map_field));
}

// Value-threaded scalar/enum/string read: reads one value from a raw byte cursor threaded by
// value (`cur`/`end` stay in registers) instead of a WireReader member. On failure reports the wire
// error at (cur - beg) -- the same offset the reader path records, since a leaf read fails at its entry
// cursor. `cur` is advanced to the returned cursor only on success. Emitted inside a scope of its own
// (an if/else-if arm), so the rp_raw/rp_v/rp_np temporaries do not collide across reads.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): enum FQN, target lvalue, and the cursor triple
// cur/end/beg are distinct string operands of the emitted read.
void emit_vt_scalar_read(const Emit& emit, FieldKind kind, std::string_view proto_type,
                         const std::string& enum_fqn, const std::string& target,
                         const std::string& cur, const std::string& end, const std::string& beg) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    Printer& p = emit.printer;
    const std::string fail =
        "if (rp_np == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we,"
        " static_cast<std::size_t>(" +
        cur + " - " + beg + ")); return false; }\n";
    if (kind == FieldKind::SsoString) {
        p.print("::rapidproto::ByteView rp_v;\n");
        p.print(
            "const std::uint8_t* const rp_np ="
            " ::rapidproto::wire::read_length_delimited($c$, $e$, &rp_v, &rp_we);\n",
            {{"c", cur}, {"e", end}});
        p.print(fail);
        p.print("$c$ = rp_np;\n", {{"c", cur}});
        p.print("$t$ = ::rapidproto::ArenaString::make(rp_v, arena);\n", {{"t", target}});
        p.print(
            "if (!rp_v.empty() && ($t$).empty()) {"
            " ::rapidproto::rp_fail_string(err, rp_v); return false; }\n",
            {{"t", target}});
    } else if (kind == FieldKind::InlineEnum) {
        p.print("std::uint64_t rp_raw = 0;\n");
        p.print(
            "const std::uint8_t* const rp_np = ::rapidproto::wire::read_varint($c$, $e$, &rp_raw,"
            " &rp_we);\n",
            {{"c", cur}, {"e", end}});
        p.print(fail);
        p.print("$c$ = rp_np;\n", {{"c", cur}});
        p.print("$t$ = static_cast<$E$>(::rapidproto::varint_to_int32(rp_raw));\n",
                {{"t", target}, {"E", cpp_type_name(emit.names, enum_fqn)}});
    } else {
        const codegen::ScalarWire& info = scalar_wire(proto_type);
        std::string vt = "read_varint";
        std::string rawty = "std::uint64_t";
        if (info.wire == "I32") {
            vt = "read_fixed32";
            rawty = "std::uint32_t";
        } else if (info.wire == "I64") {
            vt = "read_fixed64";
            rawty = "std::uint64_t";
        }
        p.print("$R$ rp_raw = 0;\n", {{"R", rawty}});
        p.print(
            "const std::uint8_t* const rp_np = ::rapidproto::wire::$vt$($c$, $e$, &rp_raw, "
            "&rp_we);\n",
            {{"vt", vt}, {"c", cur}, {"e", end}});
        p.print(fail);
        p.print("$c$ = rp_np;\n", {{"c", cur}});
        p.print("$t$ = $pre$rp_raw$post$;\n",
                {{"t", target}, {"pre", info.pre}, {"post", info.post}});
    }
}

// Dispatches a value-threaded scalar/enum/string read against the cursor names cur/end/beg -- the main
// loop's rp_c/rp_cend/byte_ptr(body), or a packed sub-span's cursor.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): target lvalue + cursor triple, distinct roles
void emit_vt_value_read(const Emit& emit, const FieldNode& field, const std::string& target,
                        const std::string& cur, const std::string& end, const std::string& beg) {
    if (!field.is_message_type && !field.is_enum_type &&
        (field.type_name == "string" || field.type_name == "bytes")) {
        emit_vt_scalar_read(emit, FieldKind::SsoString, field.type_name, /*enum_fqn=*/"", target,
                            cur, end, beg);
    } else if (field.is_enum_type) {
        emit_vt_scalar_read(emit, FieldKind::InlineEnum, field.type_name, field.resolved_type_fqn,
                            target, cur, end, beg);
    } else {
        emit_vt_scalar_read(emit, FieldKind::InlineScalar, field.type_name, /*enum_fqn=*/"", target,
                            cur, end, beg);
    }
}

// Value-threaded LEN read into a fresh ByteView `view`, advancing rp_c. Mirrors
// reader.read_length_delimited(); used for LEN message payloads, packed spans, and map entries.
void emit_vt_len_read(const Emit& emit, const std::string& view) {
    emit.printer.print(
        "::rapidproto::ByteView $v$;\n"
        "{ const std::uint8_t* const rp_np ="
        " ::rapidproto::wire::read_length_delimited(rp_c, rp_cend, &$v$, &rp_we);"
        " if (rp_np == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we,"
        " static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(body))); return false; } "
        "rp_c = "
        "rp_np; }\n",
        {{"v", view}});
}

// Value-threaded message-payload read into a fresh ByteView `view` (the LEN payload, or a group body up
// to its EGROUP), advancing rp_c. Mirrors reader.read_length_delimited() / reader.read_group().
void emit_vt_message_read(const Emit& emit, const FieldNode& field, const std::string& view) {
    if (message_wire(field).first != "SGroup") {
        emit_vt_len_read(emit, view);
        return;
    }
    emit.printer.print(
        "::rapidproto::ByteView $v$;\n"
        "{ std::size_t rp_fo = 0; const std::uint8_t* const rp_np ="
        " ::rapidproto::wire::read_group(rp_c, rp_cend, ::rapidproto::wire::byte_ptr(body), "
        "rp_tag.field_number, &$v$, &rp_we,"
        " &rp_fo); if (rp_np == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we, rp_fo);"
        " return false; } rp_c = rp_np; }\n",
        {{"v", view}});
}

std::string kv_wire(FieldKind kind, std::string_view proto_type) {
    if (kind == FieldKind::SsoString || kind == FieldKind::InlineFixedSubMsg ||
        kind == FieldKind::PointerSubMsg) {
        return "Len";
    }
    if (kind == FieldKind::InlineEnum) {
        return "Varint";
    }
    // NOLINTNEXTLINE(modernize-return-braced-init-list): std::string(string_view) is explicit
    return std::string(scalar_wire(proto_type).wire);
}

void emit_map_arm(const Emit& emit, const MemberPlan& m) {
    Printer& p = emit.printer;
    const MapFieldNode& map = *m.map_field;
    assert(m.entry.has_value());
    const EntryPlan& e = *m.entry;
    const std::string id = emit.names.local.at(&map);
    const std::string et = emit.synth.entry_type.at(&map);
    p.print("case $n$: {\n", {{"n", std::to_string(map.number)}});
    p.indent();
    p.print("if (rp_tag.wire_type == ::rapidproto::WireType::Len) {\n");
    p.indent();
    emit_vt_len_read(emit, "rp_ent");  // the entry payload, read from the main cursor rp_c
    p.print("$ET$* const rp_slot = rp_slot_$id$();\n", {{"ET", et}, {"id", id}});
    p.print("if (rp_slot == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
    p.print("*rp_slot = $ET${};\n", {{"ET", et}});
    // Value-threaded entry loop: thread a byte cursor over the entry payload (stays in registers).
    // Offsets are entry-payload-relative (rp_we is the main loop's shared error slot).
    p.print("const std::uint8_t* rp_ec = ::rapidproto::wire::byte_ptr(rp_ent);\n");
    p.print("const std::uint8_t* const rp_ee = rp_ec + rp_ent.size();\n");
    // The offset base equals byte_ptr(rp_ent) (rp_ec's initial value); recompute it on the cold fail
    // paths rather than holding it live across the hot loop (a free reinterpret_cast off rp_ent).
    const std::string beg = "::rapidproto::wire::byte_ptr(rp_ent)";
    p.print("::rapidproto::Tag rp_et{};\n");
    p.print("for (;;) {\n");
    p.indent();
    p.print("::rapidproto::wire::TagState rp_st = ::rapidproto::wire::TagState::End;\n");
    p.print(
        "const std::uint8_t* const rp_etp ="  // entry-loop tag ptr (distinct from the outer rp_tp)
        " ::rapidproto::wire::read_tag_or_end(rp_ec, rp_ee, &rp_et, &rp_we, &rp_st);\n");
    p.print("if (rp_st == ::rapidproto::wire::TagState::End) { break; }\n");
    p.print(
        "if (rp_st == ::rapidproto::wire::TagState::Error) { ::rapidproto::rp_fail_wire_at(err, "
        "rp_we,"
        " static_cast<std::size_t>(rp_ec - " +
        beg + ")); return false; }\n");
    p.print("rp_ec = rp_etp;\n");
    p.print("if (rp_et.field_number == 1 && rp_et.wire_type == ::rapidproto::WireType::$kw$) {\n",
            {{"kw", kv_wire(e.key_kind, map.key_type)}});
    p.indent();
    emit_vt_scalar_read(emit, e.key_kind, map.key_type, /*enum_fqn=*/"", "rp_slot->rp_key", "rp_ec",
                        "rp_ee", beg);
    p.outdent();
    p.print(
        "} else if (rp_et.field_number == 2 && rp_et.wire_type == ::rapidproto::WireType::$vw$) "
        "{\n",
        {{"vw", kv_wire(e.value_kind, map.value_type)}});
    p.indent();
    if (e.value_kind == FieldKind::InlineFixedSubMsg || e.value_kind == FieldKind::PointerSubMsg) {
        const std::string sub = cpp_type_name(emit.names, e.value_fqn);
        p.print("::rapidproto::ByteView rp_v;\n");
        p.print(
            "{ const std::uint8_t* const rp_np ="
            " ::rapidproto::wire::read_length_delimited(rp_ec, rp_ee, &rp_v, &rp_we);"
            " if (rp_np == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we,"
            " static_cast<std::size_t>(rp_ec - " +
            beg + ")); return false; } rp_ec = rp_np; }\n");
        if (e.value_kind == FieldKind::InlineFixedSubMsg) {
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(rp_slot->rp_value, rp_v, arena, "
                "depth + 1, err)) { return false; }\n");
        } else {
            p.print("$S$* const rp_mv = arena.create<$S$>();\n", {{"S", sub}});
            p.print("if (rp_mv == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(*rp_mv, rp_v, arena, depth + 1, "
                "err)) { return false; }\n");
            p.print("rp_slot->rp_value = rp_mv;\n");
        }
    } else {
        emit_vt_scalar_read(emit, e.value_kind, map.value_type, e.value_fqn, "rp_slot->rp_value",
                            "rp_ec", "rp_ee", beg);
    }
    p.outdent();
    p.print("} else {\n");
    p.indent();
    p.print("std::size_t rp_fo = 0;\n");
    p.print(
        "const std::uint8_t* const rp_sp ="
        " ::rapidproto::wire::skip_value(rp_ec, rp_ee, " +
        beg + ", rp_et, 0, &rp_we, &rp_fo);\n");
    p.print(
        "if (rp_sp == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we, rp_fo); return false; "
        "}\n");
    p.print("rp_ec = rp_sp;\n");
    p.outdent();
    p.print("}\n");
    p.outdent();
    p.print("}\n");  // for (;;) map entry
    p.print("continue;\n");
    p.outdent();
    p.print("}\n");  // if Len
    p.print("break;\n");
    p.outdent();
    p.print("}\n");  // case
}

void emit_map_finalize(const Emit& emit, const MemberPlan& m) {
    const std::string id = emit.names.local.at(m.map_field);
    const std::string et = emit.synth.entry_type.at(m.map_field);
    emit.printer.print(
        "out.m_$id$ = ::rapidproto::MapView<$ET$>(::rapidproto::ArrayView<$ET$>(rp_acc_$id$,"
        " rp_n_$id$));\n",
        {{"id", id}, {"ET", et}});
}

void emit_oneof_arm(const Emit& emit, const OneofPlan& o, const OneofMemberPlan& member,
                    int index) {
    Printer& p = emit.printer;
    const FieldNode& field = *member.field;
    const std::string id = emit.names.local.at(&field);
    const std::string ofield = "out.m_rp_" + o.oneof->name + "." + id;
    std::string wire;
    if (member.kind == FieldKind::SsoString) {
        wire = "Len";
    } else if (member.kind == FieldKind::InlineEnum) {
        wire = "Varint";
    } else if (member.kind == FieldKind::InlineFixedSubMsg ||
               member.kind == FieldKind::PointerSubMsg) {
        wire = message_wire(field).first;
    } else {
        wire = scalar_wire(field.type_name).wire;
    }
    p.print("case $n$: {\n", {{"n", std::to_string(field.number)}});
    p.indent();
    p.print("if (rp_tag.wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", wire}});
    p.indent();
    if (member.kind == FieldKind::InlineFixedSubMsg || member.kind == FieldKind::PointerSubMsg) {
        const std::string sub = cpp_type_name(emit.names, member.target_fqn);
        emit_vt_message_read(emit, field, "rp_v");
        if (member.kind == FieldKind::InlineFixedSubMsg) {
            p.print("$of$ = $S${};\n", {{"of", ofield}, {"S", sub}});
            p.print(
                "if (!::rapidproto::arena_detail::decode_into($of$, rp_v, arena, depth + 1, err)) "
                "{ return false; }\n",
                {{"of", ofield}});
        } else {
            p.print("$S$* const rp_sub = arena.create<$S$>();\n", {{"S", sub}});
            p.print("if (rp_sub == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(*rp_sub, rp_v, arena, depth + 1, "
                "err)) { return false; }\n");
            p.print("$of$ = rp_sub;\n", {{"of", ofield}});
        }
    } else {
        emit_vt_value_read(emit, field, ofield, "rp_c", "rp_cend",
                           "::rapidproto::wire::byte_ptr(body)");
    }
    p.print("out.m_rp_$o$_case = $i$;\n", {{"o", o.oneof->name}, {"i", std::to_string(index)}});
    p.print("continue;\n");
    p.outdent();
    p.print("}\n");
    p.print("break;\n");
    p.outdent();
    p.print("}\n");
}

// A single-byte-tag field -- number 1..15 ((15 << 3) | 7 == 127 < 128) -- that is a singular
// scalar/enum/string (not a sub-message or group) or a non-group repeated field: the 1-byte-tag
// scalar/repeated slice of the threaded set. is_threaded folds this together with the singular
// sub-message and 2-byte-tag slices; messages/groups/raw/maps/oneofs/field 16+ are covered there.
bool is_fast_arena_field(const MemberPlan& m, const FieldNode& field) {
    if (field.number > codegen::kMaxOneByteTagField) {
        return false;
    }
    if (field.is_repeated) {
        return elem_wire_enum(field) != "SGroup";  // repeated non-group
    }
    return m.kind == FieldKind::InlineScalar || m.kind == FieldKind::InlineEnum ||
           m.kind == FieldKind::SsoString;  // singular scalar/enum/string/bool (not message)
}

// A singular non-delimited LEN sub-message with a 1-byte tag: not in the default fast (scalar) set,
// but THREADED -- it gets a tag-consumed rp_do_<n> label. Groups (delimited) are excluded: their
// scan-based decode is not a simple peek-and-consume.
bool is_threadable_message(const MemberPlan& m, const FieldNode& field) {
    return field.number <= codegen::kMaxOneByteTagField && !field.is_repeated &&
           (m.kind == FieldKind::InlineFixedSubMsg || m.kind == FieldKind::PointerSubMsg) &&
           field.message_encoding != MessageEncoding::Delimited;
}

// A SINGULAR scalar/enum/string/LEN-message field with a 2-byte tag: threaded (it gets a rp_do_<n>
// label), but the 1-byte hub can't see it -- it is reached via the general path (a wire-guarded goto)
// or a 2-byte successor probe. Repeated 2-byte fields and groups stay on the general path (not threaded).
bool is_threadable_2byte(const MemberPlan& m, const FieldNode& field) {
    if (field.number <= codegen::kMaxOneByteTagField ||
        field.number > codegen::kMaxTwoByteTagField || field.is_repeated) {
        return false;
    }
    const bool scalar = m.kind == FieldKind::InlineScalar || m.kind == FieldKind::InlineEnum ||
                        m.kind == FieldKind::SsoString;
    const bool msg =
        (m.kind == FieldKind::InlineFixedSubMsg || m.kind == FieldKind::PointerSubMsg) &&
        field.message_encoding != MessageEncoding::Delimited;
    return scalar || msg;
}

// A field is threaded (has a rp_do_<n> label): the fast (1-byte scalar/enum/string + repeated) set,
// singular 1-byte non-delimited sub-messages, and singular 2-byte-tag fields. Threading is always on.
bool is_threaded(const MemberPlan& m, const FieldNode& field) {
    return is_fast_arena_field(m, field) || is_threadable_message(m, field) ||
           is_threadable_2byte(m, field);
}

// The WireType enumerator a singular scalar/enum/string field's tag carries.
std::string fast_singular_wire(const MemberPlan& m) {
    if (m.kind == FieldKind::SsoString) {
        return "Len";
    }
    if (m.kind == FieldKind::InlineEnum) {
        return "Varint";
    }
    return std::string(scalar_wire(m.field->type_name).wire);  // InlineScalar, incl. bool -> Varint
}

// The tag a threaded field is routed on (its label rp_do_<n>): a singular scalar's scalar/len tag, a
// singular message's Len tag, or a repeated field's native-element tag (the packed Len form has its own
// rp_do_<n>_p label).
std::string primary_fast_wire(const MemberPlan& m) {
    if (m.field->is_repeated) {
        return elem_wire_enum(*m.field);
    }
    if (m.kind == FieldKind::InlineFixedSubMsg || m.kind == FieldKind::PointerSubMsg) {
        return "Len";
    }
    return fast_singular_wire(m);
}

// The VALUE decode of a fast singular field (read value, set presence) -- TAG ALREADY CONSUMED, no
// `case`, no `++rp_c` for the tag, no `continue`. Emitted inside a threaded label (rp_do_<n>) whose
// entry has already consumed the tag.
void emit_fast_singular_value(const Emit& emit, const MessageLayout& layout, const MemberPlan& m,
                              const std::unordered_map<const FieldNode*, int>& required_bit) {
    Printer& p = emit.printer;
    const FieldNode& field = *m.field;
    const std::string id = emit.names.local.at(&field);
    if (m.kind == FieldKind::InlineScalar && m.is_bool) {
        p.print("std::uint64_t rp_raw = 0;\n");
        p.print(
            "const std::uint8_t* const rp_np = ::rapidproto::wire::read_varint(rp_c, rp_cend,"
            " &rp_raw, &rp_we);\n");
        p.print(
            "if (rp_np == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we,"
            " static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(body))); return false; "
            "} rp_c "
            "= rp_np;\n");
        p.print("if (::rapidproto::varint_to_bool(rp_raw)) { $set$ } else { $clr$ }\n",
                {{"set", set_bit_stmt(layout, m.value_bit)},
                 {"clr", clear_bit_stmt(layout, m.value_bit)}});
    } else {
        emit_vt_value_read(emit, field, "out.m_" + id, "rp_c", "rp_cend",
                           "::rapidproto::wire::byte_ptr(body)");
    }
    emit_presence_set(emit, layout, m, required_bit);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): assembles the per-field wire dispatch
void emit_decode_into_body(const Emit& emit, const MessageNode& message,
                           const MessageLayout& layout) {
    Printer& p = emit.printer;
    const std::unordered_map<const void*, const MemberPlan*> by_node = by_node_map(layout);
    std::unordered_map<const FieldNode*, int> required_bit;
    std::vector<const FieldNode*> required_fields;
    for (const FieldNode& f : message.fields) {
        if (f.presence == FieldPresence::Required) {
            required_bit[&f] = static_cast<int>(required_fields.size());
            required_fields.push_back(&f);
        }
    }

    p.print(
        "if (depth > ::rapidproto::kMaxDecodeDepth) { ::rapidproto::rp_fail_recursion(err);"
        " return false; }\n");
    // Setup, routed per plan in DECLARATION order (layout.members is memory-order; iterating it
    // here would reorder every existing golden): a repeated raw member gets its growable array of
    // payload views (a singular one needs none); a dropped field (absent from the plan) gets
    // nothing; materialized repeated/maps their growable arrays.
    for (const FieldNode& f : message.fields) {
        const auto it = by_node.find(&f);
        if (it == by_node.end()) {
            continue;  // dropped
        }
        if (it->second->kind == FieldKind::Raw) {
            emit_raw_setup(emit, *it->second);
        } else if (f.is_repeated) {
            emit_repeated_setup(emit, f);
        }
    }
    for (const MapFieldNode& mp : message.map_fields) {
        const auto it = by_node.find(&mp);
        if (it == by_node.end()) {
            continue;  // dropped
        }
        assert(it->second->kind == FieldKind::Map);  // raw maps are rejected at mode resolution
        emit_map_setup(emit, *it->second);
    }
    const auto req_words = (required_fields.size() + kWordBits - 1) / kWordBits;
    if (required_fields.size() > static_cast<std::size_t>(kWordBits)) {
        p.print("std::uint64_t rp_req[$w$] = {};\n", {{"w", std::to_string(req_words)}});
    } else if (!required_fields.empty()) {
        p.print("std::uint64_t rp_req = 0;\n");
    }
    // Value-threaded wire loop: the cursor (rp_c) is threaded by value through the rapidproto::wire:: reader/skip free
    // functions and stays in registers -- no WireReader member whose address escapes to memory. Fail
    // offsets are anchored at byte_ptr(body); rp_we is the shared error slot used by every arm/sub-loop.
    p.print("const std::uint8_t* rp_c = ::rapidproto::wire::byte_ptr(body);\n");
    p.print("const std::uint8_t* const rp_cend = rp_c + body.size();\n");
    p.print("::rapidproto::Tag rp_tag{};\n");
    p.print("::rapidproto::WireError rp_we = ::rapidproto::WireError::None;\n");
    p.print("for (;;) {\n");
    p.indent();
    // Field-order threading (always on): every message threads its fields. Each threaded field
    // (is_threaded) gets ONE tag-consumed label rp_do_<n>: the tag is consumed on entry, the body
    // decodes the value (no `case`, no wire guard, no tag `++rp_c`). Entries reach a label three ways,
    // all of which consume the tag FIRST:
    //   - the hub (a 1-byte peek `switch(*rp_c)`) for 1-byte-tag fields: `++rp_c; goto rp_do_n;`;
    //   - the general switch (a wire-guarded goto) for a non-minimal (over-long) tag or a 2-byte tag,
    //     after read_tag_or_end already consumed the tag;
    //   - a depth-2 constant-tag successor probe / repeated self-loop at the end of each label.
    // The general switch's threaded cases are wire-guarded gotos (NOT full arms, NOT bare skips): this
    // removes body duplication AND correctly decodes a non-minimally-encoded tag (hub miss -> general
    // -> wire guard -> label). A wrong-wire tag `break`s to the shared skip, exactly as an untouched
    // field would. End must break (not return) so the post-loop required-field checks still run.
    // The threaded fields in declaration order (ascending), so probes thread that order. Kept
    // alongside a parallel MemberPlan lookup so the body hooks can recover the arena-specific plan
    // from the generator-agnostic codegen::ThreadField the shape generator hands back.
    std::vector<codegen::ThreadField> threaded;
    std::unordered_map<int, const MemberPlan*> threaded_plan;
    for (const FieldNode& f : message.fields) {
        const auto it = by_node.find(&f);
        if (it == by_node.end() || it->second->kind == FieldKind::Raw ||
            !is_threaded(*it->second, f)) {
            continue;
        }
        const MemberPlan* m = it->second;
        const std::string tw = primary_fast_wire(*m);  // singular canonical / repeated element wire
        const bool packable = f.is_repeated && codegen::is_packable_wire(tw);
        threaded.push_back({f.number, f.is_repeated, packable, tw});
        threaded_plan.emplace(f.number, m);
    }
    const auto is_msg_kind = [](const MemberPlan& m) {
        return m.kind == FieldKind::InlineFixedSubMsg || m.kind == FieldKind::PointerSubMsg;
    };
    // The identical decode-loop SHAPE (hub, tag-consumed labels, depth-2 probes, general-case routing)
    // is shared with streamgen via codegen::emit_hub_and_labels; only the per-field label BODY differs,
    // supplied here as arena body emitters. End must break (not return) so the post-loop required-field
    // checks still run.
    codegen::ThreadedLoopHooks hooks;
    hooks.emit_body = [&](const codegen::ThreadField& tf) {
        const MemberPlan* m = threaded_plan.at(tf.number);
        if (!tf.repeated) {
            if (is_msg_kind(*m)) {
                emit_message_decode_body(emit, layout, *m, required_bit);
            } else {
                emit_fast_singular_value(emit, layout, *m, required_bit);
            }
        } else {
            emit_repeated_element(emit, *m->field);
        }
    };
    hooks.emit_packed_body = [&](const codegen::ThreadField& tf) {
        emit_vt_len_read(emit, "rp_p");
        emit_packed_fill(emit, *threaded_plan.at(tf.number)->field);
    };
    codegen::emit_hub_and_labels(p, threaded, hooks, "break;");
    // General path: multi-byte tags, unknown fields, wrong wire types, groups, messages, raw, maps,
    // oneofs, and the wire-guarded-goto routing for the threaded fields above.
    // Fused end-or-tag read: one bounds check drives the loop (see WireReader::read_tag_or_end).
    // End breaks out so the post-loop required-field checks still run.
    p.print("::rapidproto::wire::TagState rp_state = ::rapidproto::wire::TagState::End;\n");
    p.print(
        "const std::uint8_t* const rp_tp ="
        " ::rapidproto::wire::read_tag_or_end(rp_c, rp_cend, &rp_tag, &rp_we, &rp_state);\n");
    p.print("if (rp_state == ::rapidproto::wire::TagState::End) { break; }\n");
    p.print(
        "if (rp_state == ::rapidproto::wire::TagState::Error) { ::rapidproto::rp_fail_wire_at(err, "
        "rp_we,"
        " static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(body))); return false; }\n");
    p.print("rp_c = rp_tp;\n");
    p.print("switch (rp_tag.field_number) {\n");
    p.indent();
    for (const FieldNode& f : message.fields) {
        const auto it = by_node.find(&f);
        if (it == by_node.end()) {
            // Dropped: an explicit no-op case, NOT the default arm -- the record must fall to the
            // shared skip (validated) without tripping the --unknown-present bit (it IS known).
            p.print("case $n$: break;  // dropped by the field-modes profile\n",
                    {{"n", std::to_string(f.number)}});
            continue;
        }
        // A THREADED field's case here is a wire-guarded goto into its (shared) tag-consumed label, NOT
        // a full arm and NOT a bare skip. read_tag_or_end has already consumed the tag (possibly a
        // multi-byte / non-minimal encoding that missed the 1-byte hub), so on the expected wire type
        // jump straight to the label; a wrong wire type `break`s to the shared skip. This carries both
        // 2-byte-tag threaded fields (never in the hub) and the rare non-minimally-encoded tag of a
        // 1-byte threaded field -- with zero body duplication, and no silent drop.
        if (it->second->kind != FieldKind::Raw && is_threaded(*it->second, f)) {
            const std::string tw = primary_fast_wire(*it->second);
            codegen::emit_threaded_general_case(
                p, {f.number, f.is_repeated, f.is_repeated && codegen::is_packable_wire(tw), tw});
        } else if (it->second->kind == FieldKind::Raw) {
            emit_raw_arm(emit, layout, *it->second, required_bit);
        } else if (f.is_repeated) {
            emit_repeated_arm(emit, f);
        } else {
            emit_singular_arm(emit, layout, *it->second, required_bit);
        }
    }
    for (const MapFieldNode& mp : message.map_fields) {
        const auto it = by_node.find(&mp);
        if (it == by_node.end()) {
            p.print("case $n$: break;  // dropped by the field-modes profile\n",
                    {{"n", std::to_string(mp.number)}});
            continue;
        }
        emit_map_arm(emit, *it->second);
    }
    for (const OneofPlan& o : layout.oneofs) {
        int index = 1;
        for (const OneofMemberPlan& member : o.members) {
            emit_oneof_arm(emit, o, member, index++);
        }
    }
    if (layout.unknown_bit >= 0) {
        p.print("default: { $s$ break; }\n", {{"s", set_bit_stmt(layout, layout.unknown_bit)}});
    } else {
        p.print("default: break;\n");
    }
    p.outdent();
    p.print("}\n");  // switch
    p.print("std::size_t rp_fo = 0;\n");
    p.print(
        "const std::uint8_t* const rp_sp ="
        " ::rapidproto::wire::skip_value(rp_c, rp_cend, ::rapidproto::wire::byte_ptr(body), "
        "rp_tag, 0, "
        "&rp_we, &rp_fo);\n");
    p.print(
        "if (rp_sp == nullptr) { ::rapidproto::rp_fail_wire_at(err, rp_we, rp_fo); return false; "
        "}\n");
    p.print("rp_c = rp_sp;\n");
    p.outdent();
    p.print("}\n");  // for (;;)
    for (const FieldNode& f : message.fields) {
        const auto it = by_node.find(&f);
        if (it == by_node.end()) {
            continue;  // dropped
        }
        if (it->second->kind == FieldKind::Raw) {
            emit_raw_finalize(emit, *it->second);
        } else if (f.is_repeated) {
            emit_repeated_finalize(emit, f);
        }
    }
    for (const MapFieldNode& mp : message.map_fields) {
        const auto it = by_node.find(&mp);
        if (it == by_node.end()) {
            continue;  // dropped
        }
        emit_map_finalize(emit, *it->second);
    }
    for (const FieldNode* f : required_fields) {
        const int i = required_bit.at(f);
        p.print(
            "if (($ref$ & (std::uint64_t{1} << $b$)) == 0) {"
            " ::rapidproto::rp_fail_missing_required(err, $n$); return false; }\n",
            {{"ref", req_word_ref(i, required_fields.size())},
             {"b", std::to_string(req_bit_no(i, required_fields.size()))},
             {"n", std::to_string(f->number)}});
    }
    p.print("return true;\n");
}

// Out-of-line decode()/rp_decode_into() for `message` (C++-qualified name `qualifier`) + its nested
// messages. Emitted after all class shells so every field type is complete.
void emit_decode_def(const Emit& emit, const MessageNode& message, const std::string& qualifier) {
    Printer& p = emit.printer;
    const MessageLayout& layout = *emit.layouts.find(message.fqn);
    p.print(
        "// NOLINTNEXTLINE(readability-function-cognitive-complexity): generated field dispatch\n");
    // out/arena are [[maybe_unused]]: an empty message writes nothing to `out`, and a scalar-only
    // message allocates nothing from `arena`.
    // RP_FLATTEN: inline the wire primitives / dispatch / sub-decodes into this one function. GCC's
    // large-TU inliner is otherwise far more conservative than Clang's, leaving them out-of-line.
    p.print(
        "RP_FLATTEN inline bool $Q$::rp_decode_into([[maybe_unused]] $Q$& out,"
        " ::rapidproto::ByteView body, [[maybe_unused]] ::rapidproto::Arena& arena, int depth,"
        " ::rapidproto::ArenaDecodeError* err) noexcept {\n",
        {{"Q", qualifier}});
    p.indent();
    emit_decode_into_body(emit, message, layout);
    p.outdent();
    p.print("}\n");
    p.print(
        "inline const $Q$* $Q$::decode(::rapidproto::ByteView input, ::rapidproto::Arena& arena,"
        " ::rapidproto::ArenaDecodeError* err) noexcept {\n",
        {{"Q", qualifier}});
    p.indent();
    p.print("$Q$* const rp_root = arena.create<$Q$>();\n", {{"Q", qualifier}});
    p.print("if (rp_root == nullptr) { ::rapidproto::rp_fail_oom(err); return nullptr; }\n");
    p.print("if (!rp_decode_into(*rp_root, input, arena, 0, err)) { return nullptr; }\n");
    p.print("return rp_root;\n");
    p.outdent();
    p.print("}\n\n");
    for (const MessageNode& nested : message.nested_messages) {
        emit_decode_def(emit, nested, qualifier + "::" + emit.names.local.at(&nested));
    }
}

// An import path -> the arena header it produces: "foo/bar.proto" -> "foo/bar.rp.hpp".
std::string import_header(std::string_view path) {
    return codegen::import_header(path, ".rp.hpp");
}

}  // namespace

SynthNames build_synth_names(const CppNameTable& names, const LayoutSet& layouts,
                             const FileNode& file) {
    SynthNames out;
    for (const MessageNode& message : file.messages) {
        synth_for_message(names, layouts, message, out);
    }
    return out;
}

std::string generate_header(const FileNode& file, const CppNameTable& names,
                            const LayoutSet& layouts, const SymbolTable& symbols,
                            const FieldModes* modes) {
    Printer printer;
    const SynthNames synth = build_synth_names(names, layouts, file);
    const Emit emit{printer, names, layouts, synth, symbols};
    const bool profiled = modes != nullptr && modes->active();
    printer.print("// Generated by rapidprotoc $v$. DO NOT EDIT.\n", {{"v", kVersion}});
    printer.print(
        "// Generated from your schema; depends on rapidproto/arena_runtime.hpp (Apache-2.0).\n");
    if (profiled) {
        // Which profile built this header, human-readably: the first place to look when two TUs
        // disagree about a message's shape.
        std::string lines;
        for (const std::string& line : modes->normalized) {
            lines += lines.empty() ? line : ", " + line;
        }
        printer.print("// field modes ($id$): $lines$\n",
                      {{"id", modes->profile_id}, {"lines", lines}});
    }
    printer.print("#pragma once\n\n");
    printer.print("#include <cstdint>\n");
    printer.print("#include <optional>\n");  // explicit-presence accessors return std::optional<T>
    printer.print("#include <string_view>\n");
    printer.print("#include <type_traits>\n");
    printer.print("#include <variant>\n\n");  // std::monostate = a oneof reader's unset state
    printer.print("#include \"rapidproto/arena_runtime.hpp\"\n");
    // The schema's top-level enums live in the shared common header (one C++ type, shared with the
    // streaming decoder); include this file's own sibling common. The IWYU export makes a TU that
    // includes only this decoder still "directly provide" the shared enums (which used to live here).
    printer.print("#include \"$c$\"  // IWYU pragma: export\n",
                  {{"c", codegen::common_sibling_include(file.filename)}});
    for (const auto& import : file.imports) {
        if (import.kind != ImportKind::Option) {
            printer.print("#include \"$h$\"\n", {{"h", import_header(import.path)}});
        }
    }
    printer.print("\n");

    const std::string ns = join_ns(names.ns_prefix, namespace_of(file.package));
    if (!ns.empty()) {
        printer.print(profiled ? "namespace $ns$ {\n" : "namespace $ns$ {\n\n", {{"ns", ns}});
    }
    if (profiled) {
        // The profile identity as an INLINE namespace: users still write pkg::Msg, but the
        // mangled type identity encodes the profile -- two TUs generated under different
        // profiles hold genuinely distinct types (safe coexistence), and passing one across
        // the boundary is a LINK error instead of a silent ODR violation.
        printer.print("inline namespace rp_modes_$id$ {\n\n", {{"id", modes->profile_id}});
    }
    // Top-level enums are emitted by the common header above; nested enums ride with their message.
    for (const auto& message : file.messages) {  // forward-declare for pointer cross-references
        printer.print("class $T$;\n", {{"T", names.local.at(&message)}});
    }
    if (!file.messages.empty()) {
        printer.print("\n");
    }
    for (const MessageNode* message : ordered_siblings(emit, file.messages)) {
        emit_message(emit, *message);
        printer.print("static_assert(::std::is_trivially_destructible_v<$T$>);\n\n",
                      {{"T", names.local.at(message)}});
    }
    // Out-of-line decode()/rp_decode_into() definitions, after every class shell so all field types are
    // complete (handles forward + cyclic references).
    for (const MessageNode* message : ordered_siblings(emit, file.messages)) {
        emit_decode_def(emit, *message, names.local.at(message));
    }
    if (profiled) {
        printer.print("}  // namespace rp_modes_$id$\n", {{"id", modes->profile_id}});
    }
    if (!ns.empty()) {
        printer.print("}  // namespace $ns$\n", {{"ns", ns}});
    }
    return printer.str();
}

std::string generate_header(const FileNode& file, const ResolvedFileSet& set,
                            const SymbolTable& symbols, const std::string& namespace_prefix) {
    const CppNameTable names =
        codegen::build_cpp_names(file, set.files, namespace_of(namespace_prefix));
    const LayoutSet layouts = plan_layouts(set, symbols);
    return generate_header(file, names, layouts, symbols);
}

}  // namespace rapidproto::arenagen
