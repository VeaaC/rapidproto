// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#include "rapidproto/arenagen/generator.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/codegen/wire.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

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

// Identifiers arenagen synthesizes from field/oneof/map names -- has_<f>(), <oneof>_case(), the
// <Oneof>Case enum, its k<Member> constants, and the <Map>Entry type. The shared CppNameTable
// dedups the base names (fields/maps/nested types) against each other, but not these derived forms,
// so a user field literally named `has_x` or a nested `FooEntry` could collide. Computed once per
// message (keyed by node pointer) with a `_` suffix on collision, so the output always compiles.
struct SynthNames {
    std::unordered_map<const OneofNode*, std::string> case_accessor;  // <oneof>_case()
    std::unordered_map<const OneofNode*, std::string> case_enum;      // <Oneof>Case
    std::unordered_map<const MapFieldNode*, std::string> entry_type;  // <Map>Entry
    std::unordered_map<const FieldNode*, std::string> case_const;     // oneof member -> k<Member>
    std::unordered_map<const MessageNode*, std::string> unknown;      // has_unknown_fields()
};

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
void emit_oneof_types(const Emit& emit, const OneofPlan& o) {
    Printer& p = emit.printer;
    p.print("enum class $E$ : std::uint8_t {\n", {{"E", emit.synth.case_enum.at(o.oneof)}});
    p.indent();
    p.print("kNotSet = 0,\n");
    int index = 1;
    for (const OneofMemberPlan& member : o.members) {
        p.print("$K$ = $i$,\n",
                {{"K", emit.synth.case_const.at(member.field)}, {"i", std::to_string(index++)}});
    }
    p.outdent();
    p.print("};\n");
    // A union sized to the largest member; the no-op default ctor leaves it inactive (the decoder
    // sets the active member, accessors gate on the discriminant). Trivially destructible/copyable.
    // NOTE: the union makes the enclosing message non-trivially-default-constructible, so the decoder
    // must VALUE-initialize it, which Arena::create<T>() does via (::new (mem) T()), to zero the
    // discriminant to kNotSet; default-init (::new (mem) T) would leave the case indeterminate.
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
    const std::string ce = emit.synth.case_enum.at(o.oneof);
    p.print("$E$ $acc$() const noexcept { return static_cast<$E$>(m_rp_$o$_case); }\n",
            {{"E", ce}, {"acc", emit.synth.case_accessor.at(o.oneof)}, {"o", o.oneof->name}});
    const std::string acc = emit.synth.case_accessor.at(o.oneof);
    for (const OneofMemberPlan& member : o.members) {
        const std::string id = emit.names.local.at(member.field);
        const std::string guard = ce + "::" + emit.synth.case_const.at(member.field);
        if (member.kind == FieldKind::SsoString) {
            p.print(
                "std::string_view $id$() const noexcept { return $acc$() == $g$ ?"
                " m_rp_$o$.$id$.view() : std::string_view{}; }\n",
                {{"id", id}, {"acc", acc}, {"o", o.oneof->name}, {"g", guard}});
        } else if (member.kind == FieldKind::PointerSubMsg) {
            p.print(
                "const $T$* $id$() const noexcept { return $acc$() == $g$ ? m_rp_$o$.$id$"
                " : nullptr; }\n",
                {{"T", cpp_type_name(emit.names, member.target_fqn)},
                 {"id", id},
                 {"acc", acc},
                 {"o", o.oneof->name},
                 {"g", guard}});
        } else if (member.kind == FieldKind::InlineFixedSubMsg) {
            p.print(
                "const $T$* $id$() const noexcept { return $acc$() == $g$ ? &m_rp_$o$.$id$"
                " : nullptr; }\n",
                {{"T", cpp_type_name(emit.names, member.target_fqn)},
                 {"id", id},
                 {"acc", acc},
                 {"o", o.oneof->name},
                 {"g", guard}});
        } else {
            p.print(
                "$T$ $id$() const noexcept { return $acc$() == $g$ ? m_rp_$o$.$id$ : $T${}; }\n",
                {{"T", oneof_member_storage(emit, member)},
                 {"id", id},
                 {"acc", acc},
                 {"o", o.oneof->name},
                 {"g", guard}});
        }
    }
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
    std::vector<const MessageNode*> order;
    std::vector<bool> done(siblings.size(), false);
    std::vector<bool> active(siblings.size(), false);
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (done[i] || active[i]) {
            return;
        }
        active[i] = true;
        for (std::size_t j = 0; j < siblings.size(); ++j) {
            if (j != i && depends_on(i, j)) {
                visit(j);
            }
        }
        active[i] = false;
        done[i] = true;
        order.push_back(&siblings[i]);
    };
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        visit(i);
    }
    return order;
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
        emit_oneof_types(emit, o);
    }
    for (const MemberPlan& m : layout.members) {
        if (m.kind == FieldKind::Map) {
            emit_map_entry(emit, m, type);
        }
    }

    // Accessors in declaration order (fields, then maps, then oneofs).
    const std::unordered_map<const void*, const MemberPlan*> by_node = by_node_map(layout);
    for (const FieldNode& field : message.fields) {
        emit_field_accessor(emit, layout, *by_node.at(&field));
    }
    for (const MapFieldNode& map : message.map_fields) {
        emit_field_accessor(emit, layout, *by_node.at(&map));
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
        out.case_accessor[o.oneof] = dedup(o.oneof->name + "_case");
        out.case_enum[o.oneof] = dedup(capitalize(o.oneof->name) + "Case");
        std::unordered_set<std::string> enumerators = {"kNotSet"};  // separate (enum) scope
        for (const OneofMemberPlan& member : o.members) {
            std::string name = "k" + capitalize(names.local.at(member.field));
            while (!enumerators.insert(name).second) {
                name += '_';
            }
            out.case_const[member.field] = name;
        }
    }
    if (layout.unknown_bit >= 0) {
        out.unknown[&message] = dedup("has_unknown_fields");
    }
    for (const MessageNode& n : message.nested_messages) {
        synth_for_message(names, layouts, n, out);
    }
}

SynthNames build_synth_names(const CppNameTable& names, const LayoutSet& layouts,
                             const FileNode& file) {
    SynthNames out;
    for (const MessageNode& message : file.messages) {
        synth_for_message(names, layouts, message, out);
    }
    return out;
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
        return {"SGroup", "read_group(rp_tag->field_number)"};
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

// Read one scalar/enum/string value from `reader` into the lvalue `target` (wire type already
// matched); emits a wire-failure return. Not for bool-as-bit or messages.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): target lvalue vs reader expression
void emit_value_read(const Emit& emit, const FieldNode& field, const std::string& target,
                     const std::string& reader) {
    Printer& p = emit.printer;
    if (!field.is_message_type && !field.is_enum_type &&
        (field.type_name == "string" || field.type_name == "bytes")) {
        p.print("const auto rp_v = $r$.read_length_delimited();\n", {{"r", reader}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, $r$); return false; }\n",
                {{"r", reader}});
        p.print("$t$ = ::rapidproto::ArenaString::make(*rp_v, arena);\n", {{"t", target}});
        p.print(
            "if (!rp_v->empty() && ($t$).empty()) { "
            "::rapidproto::rp_fail_string(err, *rp_v); return false; }\n",
            {{"t", target}});
        return;
    }
    if (field.is_enum_type) {
        p.print("const auto rp_v = $r$.read_varint();\n", {{"r", reader}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, $r$); return false; }\n",
                {{"r", reader}});
        p.print("$t$ = static_cast<$E$>(::rapidproto::varint_to_int32(*rp_v));\n",
                {{"t", target}, {"E", cpp_type_name(emit.names, field.resolved_type_fqn)}});
        return;
    }
    const codegen::ScalarWire& info = scalar_wire(field.type_name);
    p.print("const auto rp_v = $r$.$rd$;\n", {{"r", reader}, {"rd", info.read}});
    p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, $r$); return false; }\n",
            {{"r", reader}});
    p.print("$t$ = $pre$*rp_v$post$;\n", {{"t", target}, {"pre", info.pre}, {"post", info.post}});
}

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

// A singular field's switch case (scalar/enum/string/bool/inline-or-pointer message).
void emit_singular_arm(const Emit& emit, const MessageLayout& layout, const MemberPlan& m,
                       const std::unordered_map<const FieldNode*, int>& required_bit) {
    Printer& p = emit.printer;
    const FieldNode& field = *m.field;
    const std::string id = emit.names.local.at(&field);
    p.print("case $n$: {\n", {{"n", std::to_string(field.number)}});
    p.indent();
    if (m.kind == FieldKind::InlineScalar && m.is_bool) {
        p.print("if (rp_tag->wire_type == ::rapidproto::WireType::Varint) {\n");
        p.indent();
        p.print("const auto rp_v = reader.read_varint();\n");
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, reader); return false; }\n");
        p.print("if (::rapidproto::varint_to_bool(*rp_v)) { $set$ } else { $clr$ }\n",
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
        p.print("if (rp_tag->wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", wire}});
        p.indent();
        emit_value_read(emit, field, "out.m_" + id, "reader");
        emit_presence_set(emit, layout, m, required_bit);
        p.print("continue;\n");
        p.outdent();
        p.print("}\n");
    } else if (m.kind == FieldKind::InlineFixedSubMsg || m.kind == FieldKind::PointerSubMsg) {
        // A singular sub-message appearing more than once on the wire would, in protoc, merge. A
        // read-only arena tree does not implement merge, so rather than silently take the last (or a
        // partial merge) we reject the (exotic, concatenation-style) input. "Already present" reuses
        // existing state: a null pointer, the presence bit, or the transient required bit.
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
        const auto [wire, read] = message_wire(field);
        const std::string sub = cpp_type_name(emit.names, m.target_fqn);
        p.print("if (rp_tag->wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", wire}});
        p.indent();
        p.print(
            "if ($seen$) { ::rapidproto::rp_fail_repeated_singular(err, $n$); return false; }\n",
            {{"seen", seen}, {"n", std::to_string(field.number)}});
        p.print("const auto rp_v = reader.$rd$;\n", {{"rd", read}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, reader); return false; }\n");
        if (m.kind == FieldKind::InlineFixedSubMsg) {
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(out.m_$id$, *rp_v, arena, depth + 1, "
                "err)) { return "
                "false; }\n",
                {{"S", sub}, {"id", id}});
        } else {
            p.print("$S$* const rp_sub = arena.create<$S$>();\n", {{"S", sub}});
            p.print("if (rp_sub == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(*rp_sub, *rp_v, arena, depth + 1, "
                "err)) { return false; "
                "}\n",
                {{"S", sub}});
            p.print("out.m_$id$ = rp_sub;\n", {{"id", id}});
        }
        emit_presence_set(emit, layout, m, required_bit);
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
void emit_repeated_element(const Emit& emit, const FieldNode& field, const std::string& reader) {
    Printer& p = emit.printer;
    const std::string id = emit.names.local.at(&field);
    p.print("$E$* const rp_slot = rp_slot_$id$();\n",
            {{"E", repeated_elem_type(emit, field)}, {"id", id}});
    p.print("if (rp_slot == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
    if (field.is_message_type) {
        const std::string sub = cpp_type_name(emit.names, field.resolved_type_fqn);
        const auto [wire, read] = message_wire(field);
        (void)wire;
        p.print("const auto rp_v = $r$.$rd$;\n", {{"r", reader}, {"rd", read}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, $r$); return false; }\n",
                {{"r", reader}});
        p.print("*rp_slot = $S${};\n", {{"S", sub}});
        p.print(
            "if (!::rapidproto::arena_detail::decode_into(*rp_slot, *rp_v, arena, depth + 1, err)) "
            "{ return false; }\n",
            {{"S", sub}});
    } else {
        emit_value_read(emit, field, "*rp_slot", reader);
    }
}

void emit_repeated_arm(const Emit& emit, const FieldNode& field) {
    Printer& p = emit.printer;
    const std::string elem_wire = elem_wire_enum(field);
    const bool packable = codegen::is_packable_wire(elem_wire);
    p.print("case $n$: {\n", {{"n", std::to_string(field.number)}});
    p.indent();
    p.print("if (rp_tag->wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", elem_wire}});
    p.indent();
    emit_repeated_element(emit, field, "reader");
    p.print("continue;\n");
    p.outdent();
    p.print("}\n");
    if (packable) {
        p.print("if (rp_tag->wire_type == ::rapidproto::WireType::Len) {\n");
        p.indent();
        p.print("const auto rp_p = reader.read_length_delimited();\n");
        p.print("if (!rp_p) { ::rapidproto::rp_fail_wire(err, reader); return false; }\n");
        p.print("::rapidproto::WireReader rp_pr{*rp_p};\n");
        p.print("while (!rp_pr.at_end()) {\n");
        p.indent();
        emit_repeated_element(emit, field, "rp_pr");
        p.outdent();
        p.print("}\n");
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

// Read a scalar/enum/string map key or value (field 1 / 2 of the entry) into `target` from `reader`.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): enum FQN, target lvalue, reader -- distinct
void emit_kv_scalar(const Emit& emit, FieldKind kind, std::string_view proto_type,
                    const std::string& enum_fqn, const std::string& target,
                    const std::string& reader) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    Printer& p = emit.printer;
    if (kind == FieldKind::SsoString) {
        p.print("const auto rp_v = $r$.read_length_delimited();\n", {{"r", reader}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, $r$); return false; }\n",
                {{"r", reader}});
        p.print("$t$ = ::rapidproto::ArenaString::make(*rp_v, arena);\n", {{"t", target}});
        p.print(
            "if (!rp_v->empty() && ($t$).empty()) { "
            "::rapidproto::rp_fail_string(err, *rp_v); return false; }\n",
            {{"t", target}});
    } else if (kind == FieldKind::InlineEnum) {
        p.print("const auto rp_v = $r$.read_varint();\n", {{"r", reader}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, $r$); return false; }\n",
                {{"r", reader}});
        p.print("$t$ = static_cast<$E$>(::rapidproto::varint_to_int32(*rp_v));\n",
                {{"t", target}, {"E", cpp_type_name(emit.names, enum_fqn)}});
    } else {
        const codegen::ScalarWire& info = scalar_wire(proto_type);
        p.print("const auto rp_v = $r$.$rd$;\n", {{"r", reader}, {"rd", info.read}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, $r$); return false; }\n",
                {{"r", reader}});
        p.print("$t$ = $pre$*rp_v$post$;\n",
                {{"t", target}, {"pre", info.pre}, {"post", info.post}});
    }
}

std::string kv_wire(const Emit& emit, FieldKind kind, std::string_view proto_type) {
    (void)emit;
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
    p.print("if (rp_tag->wire_type == ::rapidproto::WireType::Len) {\n");
    p.indent();
    p.print("const auto rp_ent = reader.read_length_delimited();\n");
    p.print("if (!rp_ent) { ::rapidproto::rp_fail_wire(err, reader); return false; }\n");
    p.print("$ET$* const rp_slot = rp_slot_$id$();\n", {{"ET", et}, {"id", id}});
    p.print("if (rp_slot == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
    p.print("*rp_slot = $ET${};\n", {{"ET", et}});
    p.print("::rapidproto::WireReader rp_er{*rp_ent};\n");
    p.print("while (!rp_er.at_end()) {\n");
    p.indent();
    p.print("const auto rp_et = rp_er.read_tag();\n");
    p.print("if (!rp_et) { ::rapidproto::rp_fail_wire(err, rp_er); return false; }\n");
    p.print("if (rp_et->field_number == 1 && rp_et->wire_type == ::rapidproto::WireType::$kw$) {\n",
            {{"kw", kv_wire(emit, e.key_kind, map.key_type)}});
    p.indent();
    emit_kv_scalar(emit, e.key_kind, map.key_type, /*enum_fqn=*/"", "rp_slot->rp_key", "rp_er");
    p.outdent();
    p.print(
        "} else if (rp_et->field_number == 2 && rp_et->wire_type == ::rapidproto::WireType::$vw$) "
        "{\n",
        {{"vw", kv_wire(emit, e.value_kind, map.value_type)}});
    p.indent();
    if (e.value_kind == FieldKind::InlineFixedSubMsg || e.value_kind == FieldKind::PointerSubMsg) {
        const std::string sub = cpp_type_name(emit.names, e.value_fqn);
        p.print("const auto rp_v = rp_er.read_length_delimited();\n");
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, rp_er); return false; }\n");
        if (e.value_kind == FieldKind::InlineFixedSubMsg) {
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(rp_slot->rp_value, *rp_v, arena, "
                "depth + 1, err)) {"
                " return false; }\n",
                {{"S", sub}});
        } else {
            p.print("$S$* const rp_mv = arena.create<$S$>();\n", {{"S", sub}});
            p.print("if (rp_mv == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(*rp_mv, *rp_v, arena, depth + 1, "
                "err)) { return false; "
                "}\n",
                {{"S", sub}});
            p.print("rp_slot->rp_value = rp_mv;\n");
        }
    } else {
        emit_kv_scalar(emit, e.value_kind, map.value_type, e.value_fqn, "rp_slot->rp_value",
                       "rp_er");
    }
    p.outdent();
    p.print(
        "} else if (!rp_er.skip(rp_et->wire_type, rp_et->field_number)) {"
        " ::rapidproto::rp_fail_wire(err, rp_er); return false; }\n");
    p.outdent();
    p.print("}\n");  // while entry
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
    p.print("if (rp_tag->wire_type == ::rapidproto::WireType::$w$) {\n", {{"w", wire}});
    p.indent();
    if (member.kind == FieldKind::InlineFixedSubMsg || member.kind == FieldKind::PointerSubMsg) {
        const std::string sub = cpp_type_name(emit.names, member.target_fqn);
        const auto [mw, read] = message_wire(field);
        (void)mw;
        p.print("const auto rp_v = reader.$rd$;\n", {{"rd", read}});
        p.print("if (!rp_v) { ::rapidproto::rp_fail_wire(err, reader); return false; }\n");
        if (member.kind == FieldKind::InlineFixedSubMsg) {
            p.print("$of$ = $S${};\n", {{"of", ofield}, {"S", sub}});
            p.print(
                "if (!::rapidproto::arena_detail::decode_into($of$, *rp_v, arena, depth + 1, err)) "
                "{ return false; }\n",
                {{"S", sub}, {"of", ofield}});
        } else {
            p.print("$S$* const rp_sub = arena.create<$S$>();\n", {{"S", sub}});
            p.print("if (rp_sub == nullptr) { ::rapidproto::rp_fail_oom(err); return false; }\n");
            p.print(
                "if (!::rapidproto::arena_detail::decode_into(*rp_sub, *rp_v, arena, depth + 1, "
                "err)) { return false; "
                "}\n",
                {{"S", sub}});
            p.print("$of$ = rp_sub;\n", {{"of", ofield}});
        }
    } else {
        emit_value_read(emit, field, ofield, "reader");
    }
    p.print("out.m_rp_$o$_case = $i$;\n", {{"o", o.oneof->name}, {"i", std::to_string(index)}});
    p.print("continue;\n");
    p.outdent();
    p.print("}\n");
    p.print("break;\n");
    p.outdent();
    p.print("}\n");
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
    for (const FieldNode& f : message.fields) {
        if (f.is_repeated) {
            emit_repeated_setup(emit, f);
        }
    }
    for (const MapFieldNode& mp : message.map_fields) {
        emit_map_setup(emit, *by_node.at(&mp));
    }
    const auto req_words = (required_fields.size() + kWordBits - 1) / kWordBits;
    if (required_fields.size() > static_cast<std::size_t>(kWordBits)) {
        p.print("std::uint64_t rp_req[$w$] = {};\n", {{"w", std::to_string(req_words)}});
    } else if (!required_fields.empty()) {
        p.print("std::uint64_t rp_req = 0;\n");
    }
    p.print("::rapidproto::WireReader reader{body};\n");
    p.print("while (!reader.at_end()) {\n");
    p.indent();
    p.print("const auto rp_tag = reader.read_tag();\n");
    p.print("if (!rp_tag) { ::rapidproto::rp_fail_wire(err, reader); return false; }\n");
    p.print("switch (rp_tag->field_number) {\n");
    p.indent();
    for (const FieldNode& f : message.fields) {
        if (f.is_repeated) {
            emit_repeated_arm(emit, f);
        } else {
            emit_singular_arm(emit, layout, *by_node.at(&f), required_bit);
        }
    }
    for (const MapFieldNode& mp : message.map_fields) {
        emit_map_arm(emit, *by_node.at(&mp));
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
    p.print(
        "if (!reader.skip(rp_tag->wire_type, rp_tag->field_number)) {"
        " ::rapidproto::rp_fail_wire(err, reader); return false; }\n");
    p.outdent();
    p.print("}\n");  // while
    for (const FieldNode& f : message.fields) {
        if (f.is_repeated) {
            emit_repeated_finalize(emit, f);
        }
    }
    for (const MapFieldNode& mp : message.map_fields) {
        emit_map_finalize(emit, *by_node.at(&mp));
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
    p.print(
        "inline bool $Q$::rp_decode_into([[maybe_unused]] $Q$& out, ::rapidproto::ByteView body,"
        " [[maybe_unused]] ::rapidproto::Arena& arena, int depth,"
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

std::string generate_header(const FileNode& file, const CppNameTable& names,
                            const LayoutSet& layouts, const SymbolTable& symbols) {
    Printer printer;
    const SynthNames synth = build_synth_names(names, layouts, file);
    const Emit emit{printer, names, layouts, synth, symbols};
    printer.print("// Generated by rapidprotoc. DO NOT EDIT.\n");
    printer.print(
        "// Generated from your schema; depends on rapidproto/arena_runtime.hpp (Apache-2.0).\n");
    printer.print("#pragma once\n\n");
    printer.print("#include <cstdint>\n");
    printer.print("#include <optional>\n");  // explicit-presence accessors return std::optional<T>
    printer.print("#include <string_view>\n");
    printer.print("#include <type_traits>\n\n");
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
        printer.print("namespace $ns$ {\n\n", {{"ns", ns}});
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
