// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter

#include "rapidproto/rustgen/generator.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"    // holds_enum
#include "rapidproto/codegen/naming.hpp"  // dedup_suffix
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/rustgen/names.hpp"

namespace rapidproto::rustgen {
namespace {

using codegen::Printer;

// Split a dotted proto name ("a.b.c") into its components ("" -> none).
std::vector<std::string> dotted_components(std::string_view name) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t dot = name.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? name.size() : dot;
        if (end > start) {
            out.emplace_back(name.substr(start, end - start));
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return out;
}

// The package components as the escaped module names they are emitted as (matching open_module_tree).
std::vector<std::string> escaped_package(const FileNode& file) {
    std::vector<std::string> out;
    for (const std::string& part : dotted_components(file.package)) {
        out.push_back(escape_rust_ident(part));
    }
    return out;
}

// --- Name table -------------------------------------------------------------------------------
// Every message/enum's placement in its model tree, with all Rust identifier dedup applied, computed
// ONCE per file. Emission and cross-reference path computation both read it, so an emitted `pub mod`
// / `pub struct` / newtype name and the path that refers to it can never disagree -- the reason this
// is a table and not recomputed at each use.
//
// Dedup is per scope (one proto package level or message = one Rust module). Within a scope these
// share the Rust TYPE namespace and so must be mutually unique:
//   - arena tree: each message's struct, and the `pub mod <snake>` holding its nested types;
//   - common tree: each enum's newtype and its `<Name>Kind`, and the `pub mod <snake>` holding a
//     nested message's enums.
// A message's submodule name (snake of the message) is deduped among sibling messages ALONE, so it
// is the SAME string in both trees: the module PATH is shared (an enclosing message nests both its
// messages in arena and its enums in common), so renaming a module in one tree would desync the other
// and the stored paths. A leaf type (struct / newtype / Kind) that collides with a module name is
// bumped instead -- a leaf name is never used as a path component, so bumping it stays local.
struct MsgNames {
    std::vector<std::string> module;  // enclosing module segments, from the model root
    std::string type_name;            // the arena struct identifier
    std::string submodule;  // the `pub mod` that holds nested types (snake); shared by both trees
};
struct EnumNames {
    std::vector<std::string> module;
    std::string type_name;  // the newtype identifier
    std::string kind_name;  // the `<Name>Kind` classify-enum identifier
};
struct NameTable {
    std::unordered_map<std::string, MsgNames> messages;  // message FQN -> names
    std::unordered_map<std::string, EnumNames> enums;    // enum FQN -> names
    // A oneof's enum type name (e.g. `Payload`), keyed by the OneofNode (oneofs carry no FQN). The
    // enum lives in the owning MESSAGE's arena submodule, deduped against that submodule's other
    // arena items (nested structs + their modules); see build_scope. The pointer keys index into the
    // FileNode passed to build_name_table; generate_arena/_common build the table and emit from that
    // one un-mutated FileNode, so the keys stay live -- do not copy or rebuild the AST between them.
    std::unordered_map<const OneofNode*, std::string> oneofs;
};

// Assign deduped names for ONE scope -- the child messages/enums declared directly at `module`, plus
// `scope_oneofs` (the oneofs of the MESSAGE whose submodule this scope is; empty at the package root,
// since oneofs always live in their owning message's submodule) -- then recurse into each message's
// own scope. `module` is the enclosing module path (mutated in place across the recursion).
void build_scope(const std::vector<MessageNode>& msgs, const std::vector<EnumNode>& enums,
                 const std::vector<OneofNode>& scope_oneofs, std::vector<std::string>& module,
                 NameTable& table) {
    // Submodule names first: snake, deduped among sibling messages, so both trees and the paths agree
    // on them. A message claims a name iff it emits a `pub mod` SOMEWHERE -- in the arena tree when it
    // has nested messages OR oneofs (the oneof enums live there), in the common tree when it holds
    // enums; a message that emits none leaves `submodule` empty (never read). The reserved names are
    // ONE shared set that both trees' leaf dedup starts from, so a struct/enum name is bumped off a
    // submodule even in the tree where that submodule does not exist (a common-only `pub mod` still
    // bumps a colliding arena struct). That over-dedup is cosmetic -- the bumped name is stored and
    // used uniformly, never a wrong path -- and it keeps the reservation a single set, not one per tree.
    std::unordered_set<std::string> submods;
    std::vector<std::string> submodule(msgs.size());
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (!msgs[i].nested_messages.empty() || !msgs[i].oneofs.empty() ||
            codegen::holds_enum(msgs[i])) {
            submodule[i] =
                codegen::dedup_suffix(submods, escape_rust_ident(to_snake_case(msgs[i].name)));
        }
    }
    // Arena submodule items: submodule names reserved first (shared type namespace), then each nested
    // struct, then this message's oneof enums -- all deduped against the reserved modules and each
    // other. A module keeps its name (it is a path segment); a colliding struct/oneof-enum is bumped.
    {
        std::unordered_set<std::string> taken = submods;
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            MsgNames names;
            names.module = module;
            names.type_name = codegen::dedup_suffix(taken, escape_rust_ident(msgs[i].name));
            names.submodule = submodule[i];
            table.messages.emplace(msgs[i].fqn, std::move(names));
        }
        for (const OneofNode& oneof : scope_oneofs) {
            table.oneofs.emplace(&oneof, codegen::dedup_suffix(taken, pascal_ident(oneof.name)));
        }
    }
    // Common enum names: the SAME submodule names reserved first (so a nested message's `pub mod` and
    // an enum newtype in this common scope cannot collide), then each newtype and its Kind.
    {
        std::unordered_set<std::string> taken = submods;
        for (const EnumNode& node : enums) {
            EnumNames names;
            names.module = module;
            names.type_name = codegen::dedup_suffix(taken, escape_rust_ident(node.name));
            names.kind_name = codegen::dedup_suffix(taken, names.type_name + "Kind");
            table.enums.emplace(node.fqn, std::move(names));
        }
    }
    // Recurse into each message that has its own scope to record (nested messages, enums and/or
    // oneofs), descending through its (non-empty) submodule name.
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (submodule[i].empty()) {
            continue;  // no nested messages, enums or oneofs: nothing to record
        }
        module.push_back(submodule[i]);
        build_scope(msgs[i].nested_messages, msgs[i].enums, msgs[i].oneofs, module, table);
        module.pop_back();
    }
}

// Build the whole-file name table. Covers types DECLARED in `file`; when imports land this is built
// over the import closure so a cross-file reference resolves too (see the `.at` note on the path
// helpers).
NameTable build_name_table(const FileNode& file) {
    NameTable table;
    std::vector<std::string> module = escaped_package(file);
    const std::vector<OneofNode> no_oneofs;  // the package root declares no oneofs
    build_scope(file.messages, file.enums, no_oneofs, module, table);
    return table;
}

// Emit the newtype + classify enum for one proto enum.
//
//   #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
//   #[repr(transparent)]
//   pub struct Color(pub i32);
//   impl Color {
//       pub const RED: Color = Color(1);
//       ...
//       #[inline] pub fn kind(self) -> ColorKind { match self.0 { 1 => ColorKind::Red, ..,
//                                                                  other => ColorKind::Unknown(other) } }
//   }
//   #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
//   #[non_exhaustive]
//   pub enum ColorKind { Red, .., Unknown(i32) }
//
// Storage is the raw `i32` (open enum, full range); the classify enum is the ergonomic match
// surface and `kind()` fuses to a branchless switch at the call site. The newtype and Kind names come
// from the name table (deduped against everything else in the enum's common scope).
void emit_enum(Printer& printer, const NameTable& table, const EnumNode& node) {
    const EnumNames& names = table.enums.at(node.fqn);
    const std::vector<std::string> consts = enum_const_names(node);
    const std::vector<std::string> variants = enum_variant_names(node);

    printer.set("T", names.type_name);
    printer.set("K", names.kind_name);

    printer.print("#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]\n");
    // `repr(transparent)`: a zero-cost wrapper, layout-identical to its `i32`. (It does NOT let a
    // wire enum array be aliased -- enum values are varint-encoded, so variable-width and decoded
    // one at a time; the guarantee only matters for reinterpreting an in-memory `i32` buffer.)
    printer.print("#[repr(transparent)]\n");
    printer.print("pub struct $T$(pub i32);\n\n");

    printer.print("impl $T$ {\n");
    printer.indent();
    for (std::size_t i = 0; i < node.values.size(); ++i) {
        printer.print("pub const $name$: $T$ = $T$($n$);\n",
                      {{"name", consts[i]}, {"n", std::to_string(node.values[i].number)}});
    }
    printer.print("\n");
    printer.print("/// The matchable classification of this value.\n");
    printer.print("#[inline]\n");
    printer.print("#[must_use]\n");
    printer.print("pub fn kind(self) -> $K$ {\n");
    printer.indent();
    printer.print("match self.0 {\n");
    printer.indent();
    // One arm per DISTINCT number (aliases share a number; duplicate match arms would not compile).
    std::unordered_set<std::int32_t> seen;
    for (std::size_t i = 0; i < node.values.size(); ++i) {
        if (!seen.insert(node.values[i].number).second) {
            continue;
        }
        printer.print("$n$ => $K$::$v$,\n",
                      {{"n", std::to_string(node.values[i].number)}, {"v", variants[i]}});
    }
    printer.print("other => $K$::Unknown(other),\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n\n");

    printer.print("#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]\n");
    printer.print("#[non_exhaustive]\n");
    printer.print("pub enum $K$ {\n");
    printer.indent();
    for (const std::string& variant : variants) {
        printer.print("$v$,\n", {{"v", variant}});
    }
    printer.print("Unknown(i32),\n");
    printer.outdent();
    printer.print("}\n");
}

// Open `pub mod <root> { pub mod <pkg0> { ... }`, indenting; close with close_module_tree.
void open_module_tree(Printer& printer, std::string_view root,
                      const std::vector<std::string>& pkg) {
    printer.print("pub mod $m$ {\n", {{"m", root}});
    printer.indent();
    for (const std::string& component : pkg) {
        printer.print("pub mod $m$ {\n", {{"m", escape_rust_ident(component)}});
        printer.indent();
    }
}

void close_module_tree(Printer& printer, const std::vector<std::string>& pkg) {
    for (std::size_t i = 0; i < pkg.size() + 1; ++i) {
        printer.outdent();
        printer.print("}\n");
    }
}

// The Rust type for a proto scalar keyword. `string`/`bytes` borrow the input, so they carry `'a`.
std::string rust_scalar_type(std::string_view t) {
    if (t == "int32" || t == "sint32" || t == "sfixed32") {
        return "i32";
    }
    if (t == "int64" || t == "sint64" || t == "sfixed64") {
        return "i64";
    }
    if (t == "uint32" || t == "fixed32") {
        return "u32";
    }
    if (t == "uint64" || t == "fixed64") {
        return "u64";
    }
    if (t == "bool") {
        return "bool";
    }
    if (t == "float") {
        return "f32";
    }
    if (t == "double") {
        return "f64";
    }
    if (t == "string") {
        return "::rapidproto::ProtoStr<'a>";
    }
    if (t == "bytes") {
        return "&'a [u8]";
    }
    return "()";  // defensive: not reached for a resolved scalar field
}

// `current_mod` is the module segment list of the message whose `impl` is being emitted, relative to
// its model root (`arena`/`common`): package components + one snake module per enclosing message.
//
// The two path helpers look the target up in the name table. `.at`: every referenced type is in the
// table, so a miss is a generator invariant breach (e.g. a cross-file reference before the table is
// built over the import closure) and aborts rather than silently emitting a wrong path.

// A reference from inside the arena tree (at `current_mod`) to an ENUM in the sibling `common` tree.
// Climb `super` out to the shared include root (past the `arena` root + `current_mod`), then descend
// `common`. Enum newtypes carry no lifetime.
std::string common_type_path(const NameTable& table, const std::vector<std::string>& current_mod,
                             const std::string& fqn) {
    const EnumNames& loc = table.enums.at(fqn);
    std::string out;
    for (std::size_t i = 0; i < 1 + current_mod.size(); ++i) {
        out += "super::";
    }
    out += "common";
    for (const std::string& seg : loc.module) {
        out += "::";
        out += seg;
    }
    out += "::";
    out += loc.type_name;
    return out;
}

// A reference WITHIN the arena tree from `current_mod` to the message at `fqn`: climb `super` past the
// non-shared tail of `current_mod`, then descend to the target. Same-module -> just the name.
std::string arena_message_path(const NameTable& table, const std::vector<std::string>& current_mod,
                               const std::string& fqn) {
    const MsgNames& loc = table.messages.at(fqn);
    std::size_t common = 0;
    while (common < current_mod.size() && common < loc.module.size() &&
           current_mod[common] == loc.module[common]) {
        ++common;
    }
    std::string out;
    for (std::size_t i = common; i < current_mod.size(); ++i) {
        out += "super::";
    }
    for (std::size_t i = common; i < loc.module.size(); ++i) {
        out += loc.module[i];
        out += "::";
    }
    out += loc.type_name;
    return out;
}

// The Rust type of a field's singular VALUE, ignoring repeated/presence wrapping: scalar -> a Rust
// primitive, enum -> the common-tree path, string -> ProtoStr<'a>, bytes -> &'a [u8], message -> the
// arena-tree path with its lifetime (`Inner<'a>`).
std::string field_value_type(const NameTable& table, const std::vector<std::string>& current_mod,
                             const FieldNode& field) {
    if (field.is_message_type) {
        return arena_message_path(table, current_mod, field.resolved_type_fqn) + "<'a>";
    }
    if (field.is_enum_type) {
        return common_type_path(table, current_mod, field.resolved_type_fqn);
    }
    return rust_scalar_type(field.type_name);
}

// The type carried by a oneof variant for `field`: a singular message is borrowed (`&'a T<'a>`, like
// a singular message FIELD); everything else is the plain value type (ProtoStr<'a>, &'a [u8], the enum
// newtype, or a scalar primitive). Oneof fields are never repeated or maps.
std::string variant_payload_type(const NameTable& table,
                                 const std::vector<std::string>& current_mod,
                                 const FieldNode& field) {
    if (field.is_message_type) {
        return "&'a " + field_value_type(table, current_mod, field);
    }
    return field_value_type(table, current_mod, field);
}

// Does any variant of `oneof` borrow the input, so the enum needs a `<'a>`? Message, string and bytes
// variants borrow; scalar and enum (Copy newtype) variants do not.
bool oneof_borrows(const OneofNode& oneof) {
    return std::any_of(oneof.fields.begin(), oneof.fields.end(), [](const FieldNode& field) {
        return field.is_message_type || field.type_name == "string" || field.type_name == "bytes";
    });
}

// Emit `pub enum <Name>['a] { Variant(T), .. }` for one oneof, in the owning message's arena submodule
// (`current_mod` is that submodule, so variant type paths resolve from there). The enum is closed (not
// `#[non_exhaustive]`): a oneof has a fixed variant set, and an off-list wire field is just an unknown
// field, not a new case. The `<'a>` is present only when a variant borrows -- an enum cannot carry an
// unused lifetime without a bogus phantom variant, unlike a struct's PhantomData field.
void emit_oneof_enum(Printer& printer, const NameTable& table,
                     const std::vector<std::string>& current_mod, const OneofNode& oneof) {
    const std::vector<std::string> variants = oneof_variant_names(oneof);
    printer.print("pub enum $n$$lt$ {\n",
                  {{"n", table.oneofs.at(&oneof)}, {"lt", oneof_borrows(oneof) ? "<'a>" : ""}});
    printer.indent();
    for (std::size_t i = 0; i < oneof.fields.size(); ++i) {
        printer.print(
            "$v$($t$),\n",
            {{"v", variants[i]}, {"t", variant_payload_type(table, current_mod, oneof.fields[i])}});
    }
    printer.outdent();
    printer.print("}\n");
}

// Deduped accessor method names for a message's regular fields, map fields and oneofs, in that order.
// Each proto name is escaped, then deduped against the others AND the reserved method names the same
// `impl` defines (`decode`), with a trailing `_` on collision. Without this, a field named `decode`,
// or two members escaping to one identifier (`self` -> `self_` beside a field `self_`, or a field and
// a oneof with the same name), would emit duplicate `impl` items -- non-compiling Rust for a
// protoc-valid schema. Mirrors the enum dedup.
struct AccessorNames {
    std::vector<std::string> fields;
    std::vector<std::string> maps;
    std::vector<std::string> oneofs;
};
AccessorNames message_accessor_names(const MessageNode& msg) {
    std::unordered_set<std::string> taken;
    taken.insert("decode");  // the generated decode() method lives in the same impl
    AccessorNames names;
    const auto assign = [&taken](const std::string& raw) {
        return codegen::dedup_suffix(taken, escape_rust_ident(raw));
    };
    names.fields.reserve(msg.fields.size());
    for (const FieldNode& field : msg.fields) {
        names.fields.push_back(assign(field.name));
    }
    names.maps.reserve(msg.map_fields.size());
    for (const MapFieldNode& map : msg.map_fields) {
        names.maps.push_back(assign(map.name));
    }
    names.oneofs.reserve(msg.oneofs.size());
    for (const OneofNode& oneof : msg.oneofs) {
        names.oneofs.push_back(assign(oneof.name));
    }
    return names;
}

// Emit a `pub fn <field>(&self) -> ...` with a stubbed body for one regular (non-oneof, non-map)
// field. A singular message is `Option<&'a T<'a>>` (absence = None); a repeated field is a borrowed
// slice (of inline message values for repeated messages). `decode()` is stubbed, so bodies are
// `todo!()` and the struct carries no field storage yet -- this pins accessor SIGNATURES, not the
// arena layout. `accessor` is the pre-deduped name.
void emit_field_accessor(Printer& printer, const NameTable& table,
                         const std::vector<std::string>& current_mod, const FieldNode& field,
                         const std::string& accessor) {
    const std::string value = field_value_type(table, current_mod, field);
    std::string ret;
    if (field.is_repeated) {
        ret = "&'a [" + value + "]";  // repeated: a borrowed slice, empty when the field is absent
    } else if (field.is_message_type) {
        ret = "Option<&'a " + value + ">";  // singular message: borrowed, None when absent
    } else if (field.presence == FieldPresence::Explicit) {
        ret = "Option<" + value + ">";
    } else {
        ret = value;
    }
    printer.print("pub fn $a$(&self) -> $t$ {\n", {{"a", accessor}, {"t", ret}});
    printer.indent();
    printer.print("todo!()\n");
    printer.outdent();
    printer.print("}\n");
}

// Emit a `pub fn <map>(&self) -> MapView<'a, K, V>` accessor. Keys are always scalar; a message value
// is an inline `T<'a>` (like a repeated-message element). `accessor` is the pre-deduped name.
void emit_map_accessor(Printer& printer, const NameTable& table,
                       const std::vector<std::string>& current_mod, const MapFieldNode& map,
                       const std::string& accessor) {
    const std::string key = rust_scalar_type(map.key_type);
    std::string value;
    if (map.value_is_message) {
        value = arena_message_path(table, current_mod, map.resolved_value_type_fqn) + "<'a>";
    } else if (map.value_is_enum) {
        value = common_type_path(table, current_mod, map.resolved_value_type_fqn);
    } else {
        value = rust_scalar_type(map.value_type);
    }
    printer.print("pub fn $a$(&self) -> ::rapidproto::MapView<'a, $k$, $v$> {\n",
                  {{"a", accessor}, {"k", key}, {"v", value}});
    printer.indent();
    printer.print("todo!()\n");
    printer.outdent();
    printer.print("}\n");
}

// Emit a `pub fn <oneof>(&self) -> Option<<submodule>::Payload['a]>` accessor (None = unset). The
// oneof enum lives in this message's own submodule, so it is reached by descending `submodule`; the
// `<'a>` matches the enum (present only when a variant borrows). `accessor` is the pre-deduped method
// name; `submodule` is the message's (always non-empty, since a message with a oneof has a submodule).
void emit_oneof_accessor(Printer& printer, const NameTable& table, const OneofNode& oneof,
                         const std::string& accessor, const std::string& submodule) {
    printer.print("pub fn $a$(&self) -> Option<$m$::$e$$lt$> {\n",
                  {{"a", accessor},
                   {"m", submodule},
                   {"e", table.oneofs.at(&oneof)},
                   {"lt", oneof_borrows(oneof) ? "<'a>" : ""}});
    printer.indent();
    printer.print("todo!()\n");
    printer.outdent();
    printer.print("}\n");
}

// Emit `pub struct Msg<'a> { .. } impl<'a> Msg<'a> { accessors; decode }`. `current_mod` is the module
// the struct is emitted in, used to compute cross-references.
void emit_message(Printer& printer, const NameTable& table,
                  const std::vector<std::string>& current_mod, const MessageNode& msg) {
    printer.set("M", table.messages.at(msg.fqn).type_name);
    printer.print("pub struct $M$<'a> {\n");
    printer.indent();
    printer.print("_marker: ::core::marker::PhantomData<&'a ()>,\n");
    printer.outdent();
    printer.print("}\n\n");

    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    const AccessorNames accessors = message_accessor_names(msg);
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        emit_field_accessor(printer, table, current_mod, msg.fields[i], accessors.fields[i]);
    }
    for (std::size_t i = 0; i < msg.map_fields.size(); ++i) {
        emit_map_accessor(printer, table, current_mod, msg.map_fields[i], accessors.maps[i]);
    }
    if (!msg.oneofs.empty()) {
        const std::string& submodule = table.messages.at(msg.fqn).submodule;
        for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
            emit_oneof_accessor(printer, table, msg.oneofs[i], accessors.oneofs[i], submodule);
        }
    }
    printer.print(
        "pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) "
        "-> Result<&'a $M$<'a>, ::rapidproto::DecodeError> {\n");
    printer.indent();
    printer.print("todo!()\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n");
}

// Recursively emit arena message structs: each message as a struct+impl in `current_mod`, then -- in a
// `pub mod <snake>` one level deeper -- its oneof enums followed by its nested messages. The submodule
// name comes from the name table (deduped against sibling messages and struct/oneof-enum names), so it
// matches the paths referring in. The submodule is emitted when the message has nested messages OR
// oneofs (both live there).
void emit_arena_messages(Printer& printer, const NameTable& table,
                         const std::vector<MessageNode>& msgs,
                         std::vector<std::string>& current_mod) {
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_message(printer, table, current_mod, msgs[i]);
        const bool has_submodule = !msgs[i].nested_messages.empty() || !msgs[i].oneofs.empty();
        if (has_submodule) {
            const std::string& sub = table.messages.at(msgs[i].fqn).submodule;
            printer.print("\npub mod $m$ {\n", {{"m", sub}});
            printer.indent();
            current_mod.push_back(sub);
            for (std::size_t k = 0; k < msgs[i].oneofs.size(); ++k) {
                if (k != 0) {
                    printer.print("\n");
                }
                emit_oneof_enum(printer, table, current_mod, msgs[i].oneofs[k]);
            }
            if (!msgs[i].oneofs.empty() && !msgs[i].nested_messages.empty()) {
                printer.print("\n");  // separate the oneof enums from the nested messages
            }
            emit_arena_messages(printer, table, msgs[i].nested_messages, current_mod);
            current_mod.pop_back();
            printer.outdent();
            printer.print("}\n");
        }
    }
}

// Recursively emit nested enums into the common tree: a message holding enums gets a `pub mod <snake>`
// (the same table-assigned submodule name as the arena tree) with its own enums plus the recursion.
void emit_common_message_enums(Printer& printer, const NameTable& table,
                               const std::vector<MessageNode>& msgs) {
    for (const MessageNode& msg : msgs) {
        if (!codegen::holds_enum(msg)) {
            continue;
        }
        printer.print("pub mod $m$ {\n", {{"m", table.messages.at(msg.fqn).submodule}});
        printer.indent();
        for (std::size_t i = 0; i < msg.enums.size(); ++i) {
            if (i != 0) {
                printer.print("\n");
            }
            emit_enum(printer, table, msg.enums[i]);
        }
        emit_common_message_enums(printer, table, msg.nested_messages);
        printer.outdent();
        printer.print("}\n");
    }
}

// Collect the FQNs of every message and enum DECLARED in this subtree into `fqns`.
void collect_declared_fqns(const std::vector<MessageNode>& msgs, const std::vector<EnumNode>& enums,
                           std::unordered_set<std::string>& fqns) {
    for (const EnumNode& node : enums) {
        fqns.insert(node.fqn);
    }
    for (const MessageNode& msg : msgs) {
        fqns.insert(msg.fqn);
        collect_declared_fqns(msg.nested_messages, msg.enums, fqns);
    }
}

// Is `fqn` a user type declared outside this file (so a reference to it is cross-file)?
bool external_ref(const std::string& fqn, const std::unordered_set<std::string>& local) {
    return !fqn.empty() && local.count(fqn) == 0;
}

// The first message/enum-typed field in `fields` that references an external type, or nullptr.
const FieldNode* first_external_field(const std::vector<FieldNode>& fields,
                                      const std::unordered_set<std::string>& local) {
    for (const FieldNode& field : fields) {
        if ((field.is_message_type || field.is_enum_type) &&
            external_ref(field.resolved_type_fqn, local)) {
            return &field;
        }
    }
    return nullptr;
}

// The first type reference in `msgs` (recursively) whose target is declared in another file, as a
// human-readable reason; nullopt if every reference is intra-file.
std::optional<std::string> first_cross_ref(const std::vector<MessageNode>& msgs,
                                           const std::unordered_set<std::string>& local) {
    for (const MessageNode& msg : msgs) {
        if (const FieldNode* field = first_external_field(msg.fields, local)) {
            return "message '" + msg.name + "' field '" + field->name + "' references '" +
                   field->resolved_type_fqn + "' declared in another file";
        }
        for (const MapFieldNode& map : msg.map_fields) {
            if ((map.value_is_message || map.value_is_enum) &&
                external_ref(map.resolved_value_type_fqn, local)) {
                return "message '" + msg.name + "' map '" + map.name + "' references '" +
                       map.resolved_value_type_fqn + "' declared in another file";
            }
        }
        for (const OneofNode& oneof : msg.oneofs) {
            if (const FieldNode* field = first_external_field(oneof.fields, local)) {
                return "message '" + msg.name + "' oneof '" + oneof.name + "' field '" +
                       field->name + "' references '" + field->resolved_type_fqn +
                       "' declared in another file";
            }
        }
        if (std::optional<std::string> nested = first_cross_ref(msg.nested_messages, local)) {
            return nested;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string generate_common(const FileNode& file) {
    const NameTable table = build_name_table(file);
    Printer printer;
    const std::vector<std::string> pkg = dotted_components(file.package);
    open_module_tree(printer, "common", pkg);
    for (std::size_t i = 0; i < file.enums.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_enum(printer, table, file.enums[i]);
    }
    emit_common_message_enums(printer, table, file.messages);
    close_module_tree(printer, pkg);
    return printer.str();
}

std::string generate_arena(const FileNode& file) {
    const NameTable table = build_name_table(file);
    Printer printer;
    const std::vector<std::string> pkg = dotted_components(file.package);
    open_module_tree(printer, "arena", pkg);
    std::vector<std::string> current_mod = escaped_package(file);
    emit_arena_messages(printer, table, file.messages, current_mod);
    close_module_tree(printer, pkg);
    return printer.str();
}

std::string generate_file(const FileNode& file) {
    return "// @generated by rapidprotoc --rust -- do not edit.\n\n" + generate_common(file) +
           "\n" + generate_arena(file);
}

std::optional<std::string> cross_file_dependency(const FileNode& file) {
    std::unordered_set<std::string> local;
    collect_declared_fqns(file.messages, file.enums, local);
    return first_cross_ref(file.messages, local);
}

}  // namespace rapidproto::rustgen
