// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter

#include "rapidproto/rustgen/generator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"  // LayoutSet / MessageLayout / MemberPlan / FieldKind
#include "rapidproto/arenagen/modes.hpp"  // FieldMode / FieldModes (the shared field-modes resolver)
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

// Pointers to a vector's elements, so one module scope can gather members from several files (same-
// package imports) without copying. Nested recursion addrs() a single node's child vector.
template <class T>
std::vector<const T*> addrs(const std::vector<T>& items) {
    std::vector<const T*> out;
    out.reserve(items.size());
    for (const T& item : items) {
        out.push_back(&item);
    }
    return out;
}

// The package tree for a whole generation (the import closure merged into ONE module tree, Option A).
// Each node is a package-path prefix; `files` are the files whose package is EXACTLY this node's path
// (several when same-package files are imported together), `children` the next path components. The
// node's `emitted` Rust module name is assigned by its PARENT (escaped + deduped among siblings), so
// two sibling packages whose escapes collide (`self`/`self_`) still get distinct `pub mod`s, and the
// name table and emission share the exact same module path.
struct PkgTrie {
    std::vector<const FileNode*> files;
    std::map<std::string, PkgTrie> children;  // RAW component -> child (sorted => deterministic)
    std::string emitted;  // this node's deduped Rust module name (empty at root)
};

PkgTrie build_pkg_trie(const std::vector<const FileNode*>& files) {
    PkgTrie root;
    for (const FileNode* file : files) {
        PkgTrie* node = &root;
        for (const std::string& component : dotted_components(file->package)) {
            node = &node->children[component];
        }
        node->files.push_back(file);
    }
    return root;
}

// Assign each child node its deduped Rust module name (escaped raw component, made unique among its
// siblings). A sub-package module is a path segment, so it keeps its name; a colliding message
// submodule/struct is bumped against it later (see build_scope's reserved set).
void assign_emitted_names(PkgTrie& node) {
    std::unordered_set<std::string> taken;
    for (auto& [component, child] : node.children) {
        child.emitted = codegen::dedup_suffix(taken, escape_rust_ident(component));
        assign_emitted_names(child);
    }
}

// The deduped module names of a node's direct sub-packages -- reserved in that node's scope so a
// message submodule or struct cannot collide with a sub-package `pub mod`.
std::unordered_set<std::string> child_module_names(const PkgTrie& node) {
    std::unordered_set<std::string> names;
    for (const auto& [component, child] : node.children) {
        names.insert(child.emitted);
    }
    return names;
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
    std::string type_name;   // the newtype identifier
    std::string class_name;  // the `<Name>Class` classify-enum identifier
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
void build_scope(const std::vector<const MessageNode*>& msgs,
                 const std::vector<const EnumNode*>& enums,
                 const std::vector<const OneofNode*>& scope_oneofs,
                 const std::unordered_set<std::string>& reserved_modules,
                 std::vector<std::string>& module, NameTable& table) {
    // Submodule names first: `reserved_modules` (sub-package `pub mod`s at this scope, fixed path
    // segments) are claimed up front, then each message's snake name, deduped among siblings. Both
    // trees and the paths agree on them. A message claims a name iff it emits a `pub mod` SOMEWHERE
    // -- arena when it has nested messages OR oneofs, common when it holds enums; a message that emits
    // none leaves `submodule` empty (never read). The reserved names are ONE shared set that both
    // trees' leaf dedup starts from, so a struct/enum name is bumped off a submodule (or a sub-package
    // module) even in the tree where that module does not exist. That over-dedup is cosmetic -- the
    // bumped name is stored and used uniformly, never a wrong path -- and keeps the set single.
    std::unordered_set<std::string> submods = reserved_modules;
    std::vector<std::string> submodule(msgs.size());
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (!msgs[i]->nested_messages.empty() || !msgs[i]->oneofs.empty() ||
            codegen::holds_enum(*msgs[i])) {
            submodule[i] =
                codegen::dedup_suffix(submods, escape_rust_ident(to_snake_case(msgs[i]->name)));
        }
    }
    // Arena submodule items: submodule + sub-package names reserved first (shared type namespace),
    // then each nested struct, then this message's oneof enums -- all deduped against the reserved
    // modules and each other. A module keeps its name (path segment); a colliding leaf is bumped.
    {
        std::unordered_set<std::string> taken = submods;
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            MsgNames names;
            names.module = module;
            names.type_name = codegen::dedup_suffix(taken, escape_rust_ident(msgs[i]->name));
            names.submodule = submodule[i];
            table.messages.emplace(msgs[i]->fqn, std::move(names));
        }
        for (const OneofNode* oneof : scope_oneofs) {
            table.oneofs.emplace(oneof, codegen::dedup_suffix(taken, pascal_ident(oneof->name)));
        }
    }
    // Common enum names: the SAME reserved set first (so a nested message's/sub-package's `pub mod`
    // and an enum newtype in this common scope cannot collide), then each newtype and its Class.
    {
        std::unordered_set<std::string> taken = submods;
        for (const EnumNode* node : enums) {
            EnumNames names;
            names.module = module;
            names.type_name = codegen::dedup_suffix(taken, escape_rust_ident(node->name));
            names.class_name = codegen::dedup_suffix(taken, names.type_name + "Class");
            table.enums.emplace(node->fqn, std::move(names));
        }
    }
    // Recurse into each message that has its own scope to record (nested messages, enums and/or
    // oneofs), descending through its (non-empty) submodule name. Nested scopes have no sub-packages,
    // so they reserve nothing.
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (submodule[i].empty()) {
            continue;  // no nested messages, enums or oneofs: nothing to record
        }
        module.push_back(submodule[i]);
        build_scope(addrs(msgs[i]->nested_messages), addrs(msgs[i]->enums), addrs(msgs[i]->oneofs),
                    {}, module, table);
        module.pop_back();
    }
}

// Record one package node's own types (the top-level messages/enums of every file at this exact
// package, gathered together so same-package imports share one scope and dedup against each other),
// then recurse into its sub-packages. `module` is the node's emitted package path.
void build_names_for_node(const PkgTrie& node, std::vector<std::string>& module, NameTable& table) {
    std::vector<const MessageNode*> msgs;
    std::vector<const EnumNode*> enums;
    for (const FileNode* file : node.files) {
        for (const MessageNode& msg : file->messages) {
            msgs.push_back(&msg);
        }
        for (const EnumNode& node_enum : file->enums) {
            enums.push_back(&node_enum);
        }
    }
    // The package root declares no oneofs; sub-package module names are reserved so a message here
    // cannot collide with a `pub mod <subpackage>`.
    build_scope(msgs, enums, /*scope_oneofs=*/{}, child_module_names(node), module, table);
    for (const auto& [component, child] : node.children) {
        module.push_back(child.emitted);
        build_names_for_node(child, module, table);
        module.pop_back();
    }
}

// Build the name table over the whole generation (the import closure). Every type's module path +
// name is recorded, so a reference across files or packages resolves through the same `super::`
// machinery as an intra-file one.
NameTable build_name_table(const PkgTrie& root) {
    NameTable table;
    std::vector<std::string> module;
    build_names_for_node(root, module, table);
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
//       #[inline] pub fn classify(self) -> ColorClass { match self.0 { 1 => ColorClass::Red, ..,
//                                                                  other => ColorClass::Unknown(other) } }
//   }
//   #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
//   #[non_exhaustive]
//   pub enum ColorClass { Red, .., Unknown(i32) }
//
// Storage is the raw `i32` (open enum, full range); the classify enum is the ergonomic match
// surface and `classify()` fuses to a branchless switch at the call site. The newtype and Class names
// come from the name table (deduped against everything else in the enum's common scope).
void emit_enum(Printer& printer, const NameTable& table, const EnumNode& node) {
    const EnumNames& names = table.enums.at(node.fqn);
    const std::vector<std::string> consts = enum_const_names(node);
    const std::vector<std::string> variants = enum_variant_names(node);

    printer.set("T", names.type_name);
    printer.set("K", names.class_name);

    // `Default` yields the all-zero enum value (`$T$(0)`), so a fixed-size message holding an enum
    // field can itself `derive(Default)` -- the zero used for an absent inlined sub-message.
    printer.print("#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]\n");
    // `repr(transparent)`: a zero-cost wrapper, layout-identical to its `i32`. (It does NOT let a
    // wire enum array be aliased -- enum values are varint-encoded, so variable-width and decoded
    // one at a time; the guarantee only matters for reinterpreting an in-memory `i32` buffer.)
    printer.print("#[repr(transparent)]\n");
    printer.print("pub struct $T$(pub i32);\n\n");

    // serde (behind the `serde` feature): an open enum serializes as its raw `i32`, preserving any
    // value the schema does not name -- the lossless choice for a forward-compatible enum.
    printer.print("#[cfg(feature = \"serde\")]\n");
    printer.print("impl ::rapidproto::serde::Serialize for $T$ {\n");
    printer.indent();
    printer.print(
        "fn serialize<RpS>(&self, rp_ser: RpS) -> ::core::result::Result<RpS::Ok, RpS::Error>\n");
    printer.print("where\n");
    printer.indent();
    printer.print("RpS: ::rapidproto::serde::Serializer,\n");
    printer.outdent();
    printer.print("{\n");
    printer.indent();
    printer.print("rp_ser.serialize_i32(self.0)\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n\n");

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
    printer.print("pub fn classify(self) -> $K$ {\n");
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
// unused lifetime without a bogus phantom variant, unlike a struct's PhantomData field. No `Copy`
// derive: the oneof getter hands back a borrow (`Option<&Payload>`), matching C++ (a reference into the
// union, no copy) and keeping `Copy` off the public API -- deriving it would be a hard-to-revert
// commitment, and `Copy` is for cheap integer-like values, not a sum type.
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
// `extra_reserved` seeds the dedup set beyond `decode` -- for a message whose decode body is emitted
// (slice-1 scalar messages), the struct's storage fields are named by these same accessor names and
// coexist with the decode body's internal locals (`rp_rest`, `rp_tag`, ...) and the `_marker` field,
// so those identifiers are reserved here to guarantee no field accessor collides with one.
AccessorNames message_accessor_names(const MessageNode& msg,
                                     const std::unordered_set<std::string>& extra_reserved) {
    std::unordered_set<std::string> taken = extra_reserved;
    taken.insert("decode");              // the generated decode() method lives in the same impl
    taken.insert("has_unknown_fields");  // the field-modes unknown-fields accessor (when enabled)
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

// The resolved field mode for a field / map field (Materialize when the profile is silent on it).
arenagen::FieldMode field_mode(const FieldModes& modes, const FieldNode& field) {
    const auto it = modes.fields.find(&field);
    return it == modes.fields.end() ? arenagen::FieldMode::Materialize : it->second;
}
arenagen::FieldMode map_mode(const FieldModes& modes, const MapFieldNode& map) {
    const auto it = modes.maps.find(&map);
    return it == modes.maps.end() ? arenagen::FieldMode::Materialize : it->second;
}

// Does the message's struct use the `'a` lifetime through a field (string/bytes borrow the input; a
// message field is a borrowed pointer; any repeated field is a borrowed slice; a `MapView<'a, ..>`
// always carries it; a oneof does when a variant borrows)? If not (only singular numeric / enum / bool
// fields), the struct carries a `PhantomData<&'a ()>` to use `'a`.
bool message_uses_lifetime(const MessageNode& msg, const FieldModes& modes) {
    if (std::any_of(msg.map_fields.begin(), msg.map_fields.end(), [&](const MapFieldNode& map) {
            return map_mode(modes, map) != arenagen::FieldMode::Drop;  // a MapView<'a, ..>
        })) {
        return true;
    }
    if (std::any_of(msg.fields.begin(), msg.fields.end(), [&](const FieldNode& field) {
            // A dropped field has no storage; a raw message field is a borrowed &'a [u8].
            return field_mode(modes, field) != arenagen::FieldMode::Drop &&
                   (field.is_repeated || field.is_message_type || field.type_name == "string" ||
                    field.type_name == "bytes");
        })) {
        return true;
    }
    return std::any_of(msg.oneofs.begin(), msg.oneofs.end(), oneof_borrows);
}

// Does the decode body allocate from the arena (so its `decode_into` uses the `arena` param)? Repeated
// fields and maps accumulate into an arena `DecodeVec`; a MATERIALIZED singular message field and a
// oneof message variant are arena-allocated. A singular `raw` message field stores borrowed payload
// bytes (no alloc); a `drop` field stores nothing.
bool body_uses_arena(const MessageNode& msg, const FieldModes& modes) {
    if (std::any_of(msg.map_fields.begin(), msg.map_fields.end(), [&](const MapFieldNode& map) {
            return map_mode(modes, map) != arenagen::FieldMode::Drop;  // DecodeVec of entries
        })) {
        return true;
    }
    if (std::any_of(msg.fields.begin(), msg.fields.end(), [&](const FieldNode& field) {
            const arenagen::FieldMode mode = field_mode(modes, field);
            if (mode == arenagen::FieldMode::Drop) {
                return false;
            }
            return field.is_repeated ||  // a DecodeVec (even repeated raw payloads)
                   (field.is_message_type && mode == arenagen::FieldMode::Materialize);  // alloc'd
        })) {
        return true;
    }
    return std::any_of(msg.oneofs.begin(), msg.oneofs.end(), [](const OneofNode& oneof) {
        return std::any_of(oneof.fields.begin(), oneof.fields.end(),
                           [](const FieldNode& field) { return field.is_message_type; });
    });
}

// ---------------------------------------------------------------------------------------------------
// Shared storage-layout planner (arenagen::plan_layouts) consumption. The Rust decoder reuses the C++
// planner's DECISIONS -- which singular sub-messages inline by value vs behind a pointer, and the
// bit-packed presence/bool mask -- so both decoders inline and track presence for the same fields. The
// planner's C++ byte offsets/sizes are ignored (Rust lays out its own structs).

// The planner's MemberPlan for `field` in `layout`, or null (a message may lack a layout plan, and a
// map-entry synthetic field is not a member of its parent message). Matched by FieldNode identity.
const arenagen::MemberPlan* member_for(const arenagen::MessageLayout* layout,
                                       const FieldNode& field) {
    if (layout == nullptr) {
        return nullptr;
    }
    for (const arenagen::MemberPlan& m : layout->members) {
        if (m.field == &field) {
            return &m;
        }
    }
    return nullptr;
}

// How a non-repeated, non-dropped field is stored in the Rust struct, derived from its MemberPlan.
enum class RustStore : std::uint8_t {
    Bare,         // bare value, no presence bit (Implicit/Required scalar/enum/string/bytes)
    MaskedValue,  // bare value + a mask presence bit (Explicit scalar/enum/string/bytes)
    MaskedBool,   // NO struct field; a mask value bit (+ a presence bit iff Explicit)
    InlineMsg,    // bare `T<'a>` + a mask presence bit; `T::default()` when absent
    PointerMsg,   // `Option<&'a T<'a>>`, no bit (null = absent)
};

// Classify a singular (non-repeated), materialized field. `member` may be null (no layout plan, or a
// fallback); then the pre-planner behavior (Explicit -> Option, message -> pointer) is reproduced.
RustStore rust_store(const arenagen::MemberPlan* member, const FieldNode& field) {
    if (member == nullptr) {
        if (field.is_message_type) {
            return RustStore::PointerMsg;
        }
        if (field.type_name == "bool" && field.presence != FieldPresence::Required) {
            return RustStore::Bare;  // no planner: keep a plain bool byte
        }
        return field.presence == FieldPresence::Explicit ? RustStore::MaskedValue : RustStore::Bare;
    }
    switch (member->kind) {
        case arenagen::FieldKind::InlineFixedSubMsg:
            return RustStore::InlineMsg;
        case arenagen::FieldKind::PointerSubMsg:
            return RustStore::PointerMsg;
        case arenagen::FieldKind::InlineScalar:
            if (member->is_bool) {
                return RustStore::MaskedBool;
            }
            return member->presence_bit >= 0 ? RustStore::MaskedValue : RustStore::Bare;
        case arenagen::FieldKind::InlineEnum:
        case arenagen::FieldKind::BorrowString:
            return member->presence_bit >= 0 ? RustStore::MaskedValue : RustStore::Bare;
        case arenagen::FieldKind::Repeated:
        case arenagen::FieldKind::Map:
        case arenagen::FieldKind::Raw:
            break;  // handled on their own emission paths, never through rust_store
    }
    return RustStore::Bare;
}

// Bits the Rust mask word carries: the planner's presence + value bits (NOT the unknown bit -- Rust
// keeps `rp_unknown: bool` as a separate field). Presence/value bits are assigned before the unknown
// bit, so they occupy [0, unknown_bit) when an unknown bit exists, else [0, mask_bits). 0 = no mask.
int rust_mask_bits(const arenagen::MessageLayout& layout) {
    return layout.unknown_bit >= 0 ? layout.unknown_bit : layout.mask_bits;
}

// A `[u64; N]` array mask is used past this many bits; at or below it a single uint word holds the mask.
constexpr int kMaskWordBits = 64;

// The Rust mask word type for `bits` (> 0): the smallest uint covering them, or a `[u64; N]` array.
std::string mask_word_type(int bits) {
    constexpr int kU8 = 8;
    constexpr int kU16 = 16;
    constexpr int kU32 = 32;
    if (bits <= kU8) {
        return "u8";
    }
    if (bits <= kU16) {
        return "u16";
    }
    if (bits <= kU32) {
        return "u32";
    }
    if (bits <= kMaskWordBits) {
        return "u64";
    }
    return "[u64; " + std::to_string((bits + kMaskWordBits - 1) / kMaskWordBits) + "]";
}

// The lvalue holding `bit` for a mask of `total` bits: the word itself, or `word[bit/64]` when the mask
// is a `[u64; N]` array (> 64 bits).
std::string mask_word_at(const std::string& base, int bit, int total) {
    return total > kMaskWordBits ? base + "[" + std::to_string(bit / kMaskWordBits) + "]" : base;
}
int mask_bit_pos(int bit, int total) {
    return total > kMaskWordBits ? bit % kMaskWordBits : bit;
}

// `(<base>[word] & (1 << pos)) != 0` -- a bit-set test over the mask word.
std::string mask_test(const std::string& base, int bit, int total) {
    return "(" + mask_word_at(base, bit, total) + " & (1 << " +
           std::to_string(mask_bit_pos(bit, total)) + ")) != 0";
}

// The struct storage type (= the getter return type) for a field: a repeated field is a borrowed
// slice; a singular message is `Option<&'a T<'a>>`; an explicit-presence scalar wraps in `Option`;
// everything else is the bare value type. A proto2 `[default = ...]` is NOT materialized (matching the
// C++ decoder, which reads an absent defaulted field as `None` and leaves the default to the consumer),
// so a defaulted field is stored exactly like any other field of its presence. A `raw` message field
// keeps its undecoded LEN payload instead of the decoded tree: `Option<&'a [u8]>` singular,
// `&'a [&'a [u8]]` repeated (the consumer feeds a payload to the field type's `decode` itself).
std::string field_storage_type(const NameTable& table, const std::vector<std::string>& current_mod,
                               const FieldNode& field, const FieldModes& modes,
                               const arenagen::MemberPlan* member) {
    if (field_mode(modes, field) == arenagen::FieldMode::Raw) {
        return field.is_repeated ? "&'a [&'a [u8]]" : "Option<&'a [u8]>";
    }
    std::string value = field_value_type(table, current_mod, field);
    if (field.is_repeated) {
        return "&'a [" + value + "]";
    }
    if (rust_store(member, field) == RustStore::PointerMsg) {
        return "Option<&'a " + value + ">";
    }
    // Everything else is stored bare: Bare / MaskedValue (presence in the mask) / InlineMsg (by value,
    // presence in the mask). A MaskedBool has no struct field at all, so this is never emitted for it.
    return value;
}

// The getter's return type -- the SEMANTIC presence-wrapped type, which (unlike the storage type) still
// wraps explicit presence in `Option`: masked scalar/enum/string -> `Option<T>`; inline/pointer message
// -> `Option<&T>`; bool -> `bool` / `Option<bool>`; bare -> the plain value; repeated/raw as stored.
std::string field_getter_type(const NameTable& table, const std::vector<std::string>& current_mod,
                              const FieldNode& field, const FieldModes& modes,
                              const arenagen::MemberPlan* member) {
    if (field_mode(modes, field) == arenagen::FieldMode::Raw) {
        return field.is_repeated ? "&'a [&'a [u8]]" : "Option<&'a [u8]>";
    }
    std::string value = field_value_type(table, current_mod, field);
    if (field.is_repeated) {
        return "&'a [" + value + "]";
    }
    switch (rust_store(member, field)) {
        case RustStore::PointerMsg:
            return "Option<&'a " + value + ">";  // a Copy arena borrow, outlives `&self`
        case RustStore::InlineMsg:
            return "Option<&" + value + ">";  // borrows `&self` (the value lives in the struct)
        case RustStore::MaskedValue:
            return "Option<" + value + ">";
        case RustStore::MaskedBool:
            return field.presence == FieldPresence::Explicit ? "Option<bool>" : "bool";
        case RustStore::Bare:
            return value;
    }
    return value;
}

// The default initializer for a NON-repeated field's storage local: `None` for a pointer message;
// `Default::default()` for an inlined-by-value message; else the proto3 zero value (shared by Bare and
// masked-with-a-presence-bit fields -- the presence bit, not the value, records absence). A proto2
// `[default = ...]` is not materialized (matching the C++ decoder). A bool has no local (it lives in
// the mask). Repeated fields use a `DecodeVec`, not this.
std::string field_default_expr(const NameTable& table, const std::vector<std::string>& current_mod,
                               const FieldNode& field, const arenagen::MemberPlan* member) {
    if (member != nullptr && member->kind == arenagen::FieldKind::Raw) {
        return "None";  // a singular `raw` field stores `Option<&'a [u8]>` (None = absent)
    }
    switch (rust_store(member, field)) {
        case RustStore::PointerMsg:
            return "None";
        case RustStore::InlineMsg:
            return "Default::default()";
        case RustStore::MaskedBool:  // no local emitted; defensive
        case RustStore::Bare:
        case RustStore::MaskedValue:
            break;
    }
    if (field.is_enum_type) {
        return field_value_type(table, current_mod, field) + "(0)";
    }
    const std::string& t = field.type_name;
    if (t == "string") {
        return "::rapidproto::ProtoStr::new(&[])";
    }
    if (t == "bytes") {
        return "&[]";
    }
    if (t == "bool") {
        return "false";
    }
    if (t == "float" || t == "double") {
        return "0.0";
    }
    return "0";
}

// The `wire::WireType` variant a scalar field's tag must carry.
std::string scalar_wire_variant(const FieldNode& field) {
    const std::string& t = field.type_name;
    if (t == "fixed32" || t == "sfixed32" || t == "float") {
        return "I32";
    }
    if (t == "fixed64" || t == "sfixed64" || t == "double") {
        return "I64";
    }
    if (t == "string" || t == "bytes") {
        return "Len";
    }
    return "Varint";  // int*/uint*/sint*/bool/enum
}

// The low-3-bit wire code of each wire type (its on-wire value, matching `wire::WireType as u8`).
enum WireCode : std::uint8_t {
    kWireVarint = 0,
    kWireI64 = 1,
    kWireLen = 2,
    kWireSGroup = 3,  // proto2 group start (a materialized group message field dispatches on this)
    kWireI32 = 5,
};

// The raw tag key `field << 3 | wire_code` a decode arm matches on -- the value the loop's one-byte
// peek hub dispatches with (the tag byte itself for a one-byte tag, else `(field << 3) | wire` rebuilt
// after a full read). `wire` is a `scalar_wire_variant` result or the literal "Len". Field numbers go
// up to 2^29 - 1, so the key stays within u32 (the loop's dispatch type).
std::int64_t raw_tag_value(std::int64_t number, const std::string& wire) {
    std::int64_t code = kWireI32;
    if (wire == "Varint") {
        code = kWireVarint;
    } else if (wire == "I64") {
        code = kWireI64;
    } else if (wire == "Len") {
        code = kWireLen;
    } else if (wire == "SGroup") {
        code = kWireSGroup;
    }
    return (number << 3) | code;
}

std::string raw_tag_key(std::int64_t number, const std::string& wire) {
    return std::to_string(raw_tag_value(number, wire)) + "u32";
}

// The expression converting the just-read raw wire value (`rp_v`: u64 for Varint, u32 for I32, u64 for
// I64; `rp_p`: &[u8] for Len) into the field's Rust value type.
std::string scalar_convert_expr(const NameTable& table, const std::vector<std::string>& current_mod,
                                const FieldNode& field) {
    if (field.is_enum_type) {
        return field_value_type(table, current_mod, field) + "(rp_v as i32)";
    }
    const std::string& t = field.type_name;
    // `rp_v` is u64 for a Varint field, u32 for an I32 (fixed32) field, u64 for an I64 (fixed64)
    // field -- so a fixed32/fixed64 value needs no cast, while the varint-coded types do.
    if (t == "int32" || t == "sfixed32") {
        return "rp_v as i32";
    }
    if (t == "int64" || t == "sfixed64") {
        return "rp_v as i64";
    }
    if (t == "uint32") {
        return "rp_v as u32";
    }
    if (t == "uint64" || t == "fixed32" || t == "fixed64") {
        return "rp_v";
    }
    if (t == "sint32") {
        return "::rapidproto::wire::zigzag32(rp_v as u32)";
    }
    if (t == "sint64") {
        return "::rapidproto::wire::zigzag64(rp_v)";
    }
    if (t == "bool") {
        return "rp_v != 0";
    }
    if (t == "float") {
        return "f32::from_bits(rp_v)";
    }
    if (t == "double") {
        return "f64::from_bits(rp_v)";
    }
    if (t == "string") {
        return "::rapidproto::ProtoStr::new(rp_p)";
    }
    if (t == "bytes") {
        return "rp_p";
    }
    return "rp_v";  // defensive
}

// Emit a `pub fn <field>(&self) -> ...` accessor. The return type is the SEMANTIC presence-wrapped
// type; the body reads the bare storage and -- for presence/bool-packed fields -- tests the
// `rp_presence` mask (set-index == test-index, both from the shared planner's bit assignment).
// `mask_bits` is the message's Rust mask width.
void emit_field_accessor(Printer& printer, const NameTable& table,
                         const std::vector<std::string>& current_mod, const FieldNode& field,
                         const std::string& accessor, const FieldModes& modes,
                         const arenagen::MemberPlan* member, int mask_bits) {
    const std::string ret = field_getter_type(table, current_mod, field, modes, member);
    printer.print("pub fn $a$(&self) -> $t$ {\n", {{"a", accessor}, {"t", ret}});
    printer.indent();
    const RustStore store = rust_store(member, field);
    const bool masked = field_mode(modes, field) != arenagen::FieldMode::Raw &&
                        !field.is_repeated && member != nullptr;
    // Only an Explicit field carries a resting presence bit (planner `needs_presence`); an Implicit
    // bool (value bit only) or a Required inlined message has `presence_bit == -1`, so computing the
    // test would emit `1 << -1`. Leave it empty there -- the branches that lack a presence bit never
    // read `pbit`.
    const std::string pbit = (masked && member->presence_bit >= 0)
                                 ? mask_test("self.rp_presence", member->presence_bit, mask_bits)
                                 : "";
    switch (store) {
        case RustStore::MaskedBool: {
            const std::string vtest = mask_test("self.rp_presence", member->value_bit, mask_bits);
            if (field.presence == FieldPresence::Explicit) {
                printer.print("if $p$ { Some($v$) } else { None }\n", {{"p", pbit}, {"v", vtest}});
            } else {
                printer.print("$v$\n", {{"v", vtest}});
            }
            break;
        }
        case RustStore::MaskedValue:
            printer.print("if $p$ { Some(self.$a$) } else { None }\n",
                          {{"p", pbit}, {"a", accessor}});
            break;
        case RustStore::InlineMsg:
            if (member->presence_bit >= 0) {
                printer.print("if $p$ { Some(&self.$a$) } else { None }\n",
                              {{"p", pbit}, {"a", accessor}});
            } else {
                // A Required inlined message has no resting presence bit (validated via
                // MissingRequired), so on a successful decode it is always present.
                printer.print("Some(&self.$a$)\n", {{"a", accessor}});
            }
            break;
        case RustStore::PointerMsg:
        case RustStore::Bare:
            printer.print("self.$a$\n", {{"a", accessor}});
            break;
    }
    printer.outdent();
    printer.print("}\n");
}

// The struct storage (= getter return) type for one map field: a borrowed `MapView<'a, K, V>`. Keys
// are scalar; a message value is stored inline by value (`Inner<'a>`), like a repeated-message element.
std::string map_view_type(const NameTable& table, const std::vector<std::string>& current_mod,
                          const MapFieldNode& map) {
    const std::string key = rust_scalar_type(map.key_type);
    std::string value;
    if (map.value_is_message) {
        value = arena_message_path(table, current_mod, map.resolved_value_type_fqn) + "<'a>";
    } else if (map.value_is_enum) {
        value = common_type_path(table, current_mod, map.resolved_value_type_fqn);
    } else {
        value = rust_scalar_type(map.value_type);
    }
    return "::rapidproto::MapView<'a, " + key + ", " + value + ">";
}

// The struct storage (= getter return) type for one oneof: `Option<<submodule>::<Enum>['a]>` (None =
// unset). The `<'a>` is present only when a variant borrows (matching the enum definition). `submodule`
// is the owning message's (always non-empty, since a message with a oneof has a submodule).
std::string oneof_storage_type(const NameTable& table, const OneofNode& oneof,
                               const std::string& submodule) {
    return "Option<" + submodule + "::" + table.oneofs.at(&oneof) +
           (oneof_borrows(oneof) ? "<'a>" : "") + ">";
}

// Emit a `pub fn <map>(&self) -> MapView<'a, K, V>` accessor reading the stored view. `accessor` is
// the pre-deduped name.
void emit_map_accessor(Printer& printer, const NameTable& table,
                       const std::vector<std::string>& current_mod, const MapFieldNode& map,
                       const std::string& accessor) {
    printer.print("pub fn $a$(&self) -> $t$ {\n",
                  {{"a", accessor}, {"t", map_view_type(table, current_mod, map)}});
    printer.indent();
    printer.print("self.$a$\n", {{"a", accessor}});
    printer.outdent();
    printer.print("}\n");
}

// Emit a `pub fn <oneof>(&self) -> Option<&<submodule>::Payload['a]>` accessor (None = unset),
// borrowing the stored enum (`as_ref`). The getter hands back a reference -- the oneof is not `Copy`
// (see `emit_oneof_enum`) -- so matching binds the variant payloads by reference. The oneof enum lives
// in this message's own submodule. `accessor` is the pre-deduped method name.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): accessor (method name) vs submodule (module path)
void emit_oneof_accessor(Printer& printer, const NameTable& table, const OneofNode& oneof,
                         const std::string& accessor, const std::string& submodule) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const std::string ret = "Option<&" + submodule + "::" + table.oneofs.at(&oneof) +
                            (oneof_borrows(oneof) ? "<'a>" : "") + ">";
    printer.print("pub fn $a$(&self) -> $t$ {\n", {{"a", accessor}, {"t", ret}});
    printer.indent();
    printer.print("self.$a$.as_ref()\n", {{"a", accessor}});
    printer.outdent();
    printer.print("}\n");
}

// A wire read in a decode frame: `src` is the `rest` slice variable to read from and advance, and `off`
// is the body-relative offset expression to report on error -- both resolved to the current frame. The
// readers leave `src` unadvanced on `Err`, so `src`'s position is where the failing item began.
struct ReadAt {
    std::string src;
    std::string off;
};

// The main message loop's body-relative offset: the readers leave `rp_rest` at the failing item.
constexpr const char* kLoopOff = "rp_body.len() - rp_rest.len()";

// The read context for the main message loop: read from `rp_rest`, error at its position in `rp_body`.
ReadAt loop_read() {
    return {"rp_rest", kLoopOff};
}

// The read context for one element inside a packed run: read from the payload suffix `rp_p`. `rp_rest`
// is already fixed past the whole payload and `rp_p` shrinks as elements are read, so this points at
// the element being read -- not at the end of the field, as `loop_read`'s offset would here.
ReadAt packed_read() {
    return {"rp_p", "rp_body.len() - rp_rest.len() - rp_p.len()"};
}

// Emit a scalar (Varint/I32/I64) value read from `at.src` into `rp_v`, erroring at `at.off`. After it
// `scalar_convert_expr` yields the typed value.
void emit_scalar_value_read(Printer& printer, const FieldNode& field, const ReadAt& at) {
    const std::string wire = scalar_wire_variant(field);
    if (wire == "I32") {
        printer.print("let mut rp_v = 0u32;\n");
        printer.print("$s$ = match ::rapidproto::wire::read_fixed32($s$, &mut rp_v) {\n",
                      {{"s", at.src}});
    } else if (wire == "I64") {
        printer.print("let mut rp_v = 0u64;\n");
        printer.print("$s$ = match ::rapidproto::wire::read_fixed64($s$, &mut rp_v) {\n",
                      {{"s", at.src}});
    } else {
        printer.print("let mut rp_v = 0u64;\n");
        printer.print("$s$ = match ::rapidproto::wire::read_varint($s$, &mut rp_v) {\n",
                      {{"s", at.src}});
    }
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", at.off}});
    printer.outdent();
    printer.print("};\n");
}

// Emit a length-delimited payload read from `at.src` into `rp_p`, erroring at `at.off`.
void emit_len_read(Printer& printer, const ReadAt& at) {
    printer.print("let mut rp_p: &[u8] = &[];\n");
    printer.print("$s$ = match ::rapidproto::wire::read_length_delimited($s$, &mut rp_p) {\n",
                  {{"s", at.src}});
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", at.off}});
    printer.outdent();
    printer.print("};\n");
}

// The wire form a message field arrives in: a proto2 group (delimited) is `SGroup` (start-group tag,
// scanned to its matching end), every other message field is length-prefixed (`Len`). Drives both the
// dispatch key and which payload reader runs.
std::string message_field_wire(const FieldNode& field) {
    return field.message_encoding == MessageEncoding::Delimited ? "SGroup" : "Len";
}

// Read a message field's payload from `at.src` into `rp_p`: a length-delimited body for a normal
// message, or a group body (start-tag already consumed, scanned to the matching end-group) via
// `read_group` for a proto2 group field. Either way `rp_p` is the bytes to decode as the message body.
void emit_msg_payload_read(Printer& printer, const FieldNode& field, const ReadAt& at) {
    if (field.message_encoding != MessageEncoding::Delimited) {
        emit_len_read(printer, at);
        return;
    }
    printer.print("let mut rp_p: &[u8] = &[];\n");
    printer.print("$s$ = match ::rapidproto::wire::read_group($s$, $n$, &mut rp_p) {\n",
                  {{"s", at.src}, {"n", std::to_string(field.number)}});
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", at.off}});
    printer.outdent();
    printer.print("};\n");
}

// A `DecodeVec::push`, mapping arena exhaustion to a DecodeError.
void emit_push(Printer& printer, const std::string& acc, const std::string& value) {
    printer.print(
        "$acc$.push($v$).map_err(|_| "
        "::rapidproto::DecodeError::new(::rapidproto::ErrorKind::ArenaExhausted, 0))?;\n",
        {{"acc", acc}, {"v", value}});
}

// A `DecodeVec::reserve`, mapping arena exhaustion to a DecodeError. Packed decode calls it once
// before the fill loop to pre-size the array (mirrors the C++ decoder's up-front `allocate_array`).
void emit_reserve(Printer& printer, const std::string& acc, const std::string& count) {
    printer.print(
        "$acc$.reserve($c$).map_err(|_| "
        "::rapidproto::DecodeError::new(::rapidproto::ErrorKind::ArenaExhausted, 0))?;\n",
        {{"acc", acc}, {"c", count}});
}

// Advance the slice `at.src` past the current unmatched field via `skip_field` (given its raw tag key
// `rp_tagkey`), erroring at `at.off`. The loop's `_` catch-all uses this: the peek hub dispatches on
// the raw key, so an unknown or wrong-wire field reaches here with only the key (no rebuilt Tag), and
// `skip_field` reconstructs the wire type -- rejecting a malformed one-byte tag (field 0 / reserved
// wire code) as InvalidTag, the kind a full tag read would have raised.
void emit_skip_field_body(Printer& printer, const ReadAt& at) {
    printer.print("$s$ = match ::rapidproto::wire::skip_field($s$, rp_tagkey) {\n",
                  {{"s", at.src}});
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", at.off}});
    printer.outdent();
    printer.print("};\n");
}

// Emit the field loop's one-byte peek hub: read the next tag's raw key into `rp_tagkey` (u32), or
// `break` at end of the slice `at.src`. A one-byte tag (high bit clear) becomes the key directly after
// bumping the slice past it -- no Tag built and no field/wire validation (a malformed one-byte tag
// flows to the `_` arm's `skip_field`, which rejects it); a multi-byte tag goes through the fused
// `read_tag` and is rebuilt as `(field << 3) | wire`. The caller then `match`es `rp_tagkey` against
// each field's raw key (see `raw_tag_key`). `tag` is the scratch Tag local (`rp_tag` / `rp_etag`);
// `at.off` is the body-relative error offset for a malformed multi-byte tag.
void emit_peek_hub(Printer& printer, const ReadAt& at, const std::string& tag) {
    printer.print("let rp_b = match $s$.first() {\n", {{"s", at.src}});
    printer.indent();
    printer.print("Some(&rp_b) => rp_b,\n");
    printer.print("None => break,\n");
    printer.outdent();
    printer.print("};\n");
    printer.print("let rp_tagkey: u32 = if rp_b < 0x80 {\n");
    printer.indent();
    printer.print("$s$ = &$s$[1..];\n", {{"s", at.src}});
    printer.print("u32::from(rp_b)\n");
    printer.outdent();
    printer.print("} else {\n");
    printer.indent();
    printer.print(
        "let mut $t$ = ::rapidproto::wire::Tag { field: 0, wire: "
        "::rapidproto::wire::WireType::Varint };\n",
        {{"t", tag}});
    printer.print("$s$ = match ::rapidproto::wire::read_tag($s$, &mut $t$) {\n",
                  {{"s", at.src}, {"t", tag}});
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", at.off}});
    printer.outdent();
    printer.print("};\n");
    printer.print("($t$.field << 3) | $t$.wire as u32\n", {{"t", tag}});
    printer.outdent();
    printer.print("};\n");
}

// Emit the packed (length-delimited run) arm for a repeated numeric field, pre-sizing the
// accumulator from the span before the fill -- the Rust mirror of the C++ decoder's standalone
// `emit_packed_fill` (src/arenagen/generator.cpp). Varint elements reserve the span's byte length as
// an element upper bound (each varint is >= 1 byte), fill, then trim the over-estimate back to the
// exact count (`shrink_to_fit`). Fixed-width elements reserve the exact count (span / width); on a
// little-endian host a whole-multiple span fills in one bulk copy (`extend_from_le_bytes`, the span
// IS the native byte image), else a per-element read reports an exact truncation error.
// Body of the packed (length-delimited run) decode for a repeated numeric field (no arm wrapper),
// shared by the hub `(n,Len)` arm and the threaded prologue's packed probe.
void emit_packed_body(Printer& printer, const NameTable& table,
                      const std::vector<std::string>& current_mod, const FieldNode& field) {
    const std::string acc = "rp_acc_" + std::to_string(field.number);
    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    emit_len_read(printer, loop_read());
    if (wire == "Varint") {
        // Packed varint: the width-classified SWAR kernel (large spans) / scalar loop (small) reserves
        // the element upper bound, fills, and trims -- the Rust mirror of the C++ decoder's
        // `decode_packed_varints`. `$off$` is the span's byte offset in the body; the kernel adds the
        // offset of a malformed varint within the span. `conv` reads the raw value named `rp_v`.
        printer.print("$acc$.fill_packed_varints(rp_p, $off$, |rp_v| $conv$)?;\n",
                      {{"acc", acc}, {"off", packed_read().off}, {"conv", conv}});
    } else {
        const std::string width = wire == "I32" ? "4" : "8";
        printer.print("if cfg!(target_endian = \"little\") && rp_p.len() % $w$ == 0 {\n",
                      {{"w", width}});
        printer.indent();
        printer.print(
            "$acc$.extend_from_le_bytes(rp_p).map_err(|_| "
            "::rapidproto::DecodeError::new(::rapidproto::ErrorKind::ArenaExhausted, 0))?;\n",
            {{"acc", acc}});
        printer.outdent();
        printer.print("} else {\n");
        printer.indent();
        emit_reserve(printer, acc, "rp_p.len() / " + width);
        printer.print("while !rp_p.is_empty() {\n");
        printer.indent();
        emit_scalar_value_read(printer, field, packed_read());
        emit_push(printer, acc, conv);
        printer.outdent();
        printer.print("}\n");
        printer.outdent();
        printer.print("}\n");
    }
}

// Emit the packed arm for a repeated numeric field (body shared with the threaded prologue). Pre-sizes
// the accumulator from the span before the fill -- the Rust mirror of the C++ decoder's standalone
// `emit_packed_fill`.
void emit_packed_arm(Printer& printer, const NameTable& table,
                     const std::vector<std::string>& current_mod, const FieldNode& field) {
    printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, "Len")}});
    printer.indent();
    emit_packed_body(printer, table, current_mod, field);
    printer.outdent();
    printer.print("}\n");
}

// Body of one repeated-field element decode (no arm wrapper): read ONE native element and push it into
// the accumulator -- a numeric value, a string/bytes payload, or a sub-message decoded into the growable
// slot. Shared by the hub arm and the threaded prologue's element self-loop (both read `rp_rest` via
// `loop_read()`), so the element decode cannot drift.
void emit_repeated_native_body(Printer& printer, const NameTable& table,
                               const std::vector<std::string>& current_mod,
                               const FieldNode& field) {
    const std::string acc = "rp_acc_" + std::to_string(field.number);
    if (field.is_message_type) {
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        emit_msg_payload_read(printer, field, loop_read());
        printer.print(
            "$acc$.emplace(|rp_out| $p$::decode_into(rp_out, rp_p, rp_arena, rp_depth + 1))?;\n",
            {{"acc", acc}, {"p", path}});
        return;
    }
    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    if (wire == "Len") {
        emit_len_read(printer, loop_read());
    } else {
        emit_scalar_value_read(printer, field, loop_read());
    }
    emit_push(printer, acc, conv);
}

// Body of a singular (non-repeated) MATERIALIZED message decode (no arm wrapper): reject a second
// occurrence, read the LEN payload, decode it in place -- by value into inline storage
// (`decode_value`, no alloc, presence = a mask bit) if the planner inlined it, else into an arena slot
// behind a pointer (`emplace`, presence = `Option` is `Some`) -- and record presence/required. Shared
// by the hub `(n,Len)` arm and the threaded prologue; both operate on `rp_rest` (`loop_read()`), so the
// body is identical either way.
void emit_singular_message_body(Printer& printer, const NameTable& table,
                                const std::vector<std::string>& current_mod, const FieldNode& field,
                                const std::string& accessor, bool required,
                                const arenagen::MemberPlan* member, int mask_bits) {
    const std::string nstr = std::to_string(field.number);
    const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
    const bool inlined = rust_store(member, field) == RustStore::InlineMsg;
    const bool inlined_masked =
        inlined && member->presence_bit >= 0;  // Explicit: mask-bit presence
    // Duplicate-singular-message rejection tests "already seen": an Explicit inlined message by its
    // mask presence bit, a Required inlined message by its transient `rp_req_<n>` flag (it has no
    // resting bit), and a pointer message by its `Option` being `Some`.
    std::string seen;
    if (inlined_masked) {
        seen = mask_test("rp_presence", member->presence_bit, mask_bits);
    } else if (inlined) {
        seen = "rp_req_" + nstr;
    } else {
        seen = accessor + ".is_some()";
    }
    printer.print("if $s$ {\n", {{"s", seen}});
    printer.indent();
    printer.print(
        "return Err(::rapidproto::DecodeError::new("
        "::rapidproto::ErrorKind::RepeatedSingularMessage, $off$).with_field($n$));\n",
        {{"off", kLoopOff}, {"n", nstr}});
    printer.outdent();
    printer.print("}\n");
    emit_msg_payload_read(printer, field, loop_read());
    if (inlined) {
        // Decode by value directly into the inline storage slot (no arena alloc, no pointer).
        printer.print(
            "$a$ = ::rapidproto::decode_value(|rp_out| $p$::decode_into(rp_out, rp_p, rp_arena, "
            "rp_depth + 1))?;\n",
            {{"a", accessor}, {"p", path}});
        if (inlined_masked) {
            // Explicit: record presence in the mask. (Required has no resting bit -- `rp_req_<n>`
            // below both rejects a duplicate and drives the post-loop MissingRequired check.)
            printer.print("$w$ |= 1 << $b$;\n",
                          {{"w", mask_word_at("rp_presence", member->presence_bit, mask_bits)},
                           {"b", std::to_string(mask_bit_pos(member->presence_bit, mask_bits))}});
        }
    } else {
        // Decode directly into its arena slot (no by-value copy).
        printer.print(
            "$a$ = Some(rp_arena.emplace(|rp_out| $p$::decode_into(rp_out, rp_p, rp_arena, "
            "rp_depth + 1))?);\n",
            {{"a", accessor}, {"p", path}});
    }
    if (required) {
        printer.print("rp_req_$n$ = true;\n", {{"n", nstr}});
    }
}

// Emit the `(n, Len) => { .. }` arm for a singular materialized message field (body shared with the
// threaded prologue). A second occurrence is rejected (matching C++).
void emit_singular_message_arm(Printer& printer, const NameTable& table,
                               const std::vector<std::string>& current_mod, const FieldNode& field,
                               const std::string& accessor, bool required,
                               const arenagen::MemberPlan* member, int mask_bits) {
    printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, message_field_wire(field))}});
    printer.indent();
    emit_singular_message_body(printer, table, current_mod, field, accessor, required, member,
                               mask_bits);
    printer.outdent();
    printer.print("}\n");
}

// Read a singular scalar/enum/string/bytes/bool VALUE at `at` and store it into `accessor` per the
// field's layout: a bare value, a masked value + presence bit, or a bool packed into value (and, if
// explicit, presence) mask bits. Then set the proto2 `required` seen-flag. Shared by the general match
// arm and the threaded prologue so the two cannot drift on storage semantics; `member`/`mask_bits`
// drive the presence/bool mask.
void emit_singular_scalar_store(Printer& printer, const NameTable& table,
                                const std::vector<std::string>& current_mod, const FieldNode& field,
                                const std::string& accessor, bool required,
                                const arenagen::MemberPlan* member, int mask_bits,
                                const ReadAt& at) {
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    const auto set_bit = [&](int bit) {
        return mask_word_at("rp_presence", bit, mask_bits) + " |= 1 << " +
               std::to_string(mask_bit_pos(bit, mask_bits)) + ";\n";
    };
    const auto clear_bit = [&](int bit) {
        return mask_word_at("rp_presence", bit, mask_bits) + " &= !(1 << " +
               std::to_string(mask_bit_pos(bit, mask_bits)) + ");\n";
    };
    if (scalar_wire_variant(field) == "Len") {
        emit_len_read(printer, at);
    } else {
        emit_scalar_value_read(printer, field, at);
    }
    switch (rust_store(member, field)) {
        case RustStore::MaskedBool:
            // The bool value is a mask bit (no byte field): set it from the varint (clear on false, so
            // a later `false` overrides an earlier `true`); Explicit also records the presence bit.
            printer.print("if rp_v != 0 {\n");
            printer.indent();
            printer.print(set_bit(member->value_bit));
            printer.outdent();
            printer.print("} else {\n");
            printer.indent();
            printer.print(clear_bit(member->value_bit));
            printer.outdent();
            printer.print("}\n");
            if (member->presence_bit >= 0) {
                printer.print(set_bit(member->presence_bit));
            }
            break;
        case RustStore::MaskedValue:
            printer.print("$a$ = $c$;\n", {{"a", accessor}, {"c", conv}});
            printer.print(set_bit(member->presence_bit));
            break;
        case RustStore::Bare:
        case RustStore::InlineMsg:
        case RustStore::PointerMsg:
            printer.print("$a$ = $c$;\n", {{"a", accessor}, {"c", conv}});
            break;
    }
    if (required) {
        printer.print("rp_req_$n$ = true;\n", {{"n", std::to_string(field.number)}});
    }
}

// Emit the `match rp_tagkey` arm(s) for one field, keyed by its raw tag `field << 3 | wire`: one arm
// for a singular value, two for a repeated numeric (unpacked + packed), reading via the value-threaded
// wire primitives and storing
// into the field's local (`accessor`) or pushing into its accumulator (`rp_acc_<n>`). A singular
// message is decoded in place (see emit_singular_message_arm); a repeated message's element is decoded
// into the growable array slot. A singular message seen twice is rejected (matching C++).
void emit_field_arms(Printer& printer, const NameTable& table,
                     const std::vector<std::string>& current_mod, const FieldNode& field,
                     const std::string& accessor, bool required, const FieldModes& modes,
                     const arenagen::MemberPlan* member, int mask_bits) {
    const std::string nstr = std::to_string(field.number);
    const std::string acc = "rp_acc_" + nstr;

    const arenagen::FieldMode mode = field_mode(modes, field);
    if (mode == arenagen::FieldMode::Drop) {
        // A dropped field gets no arm: it falls to the loop's `_` catch-all, which skips it. Its
        // number is still in the catch-all's known-number set, so (in an unknown-tracked message) it
        // never trips the unknown-fields bit -- matching the old explicit `(n, _)` known-skip arm.
        return;
    }
    if (mode == arenagen::FieldMode::Raw) {
        // A message field kept as its undecoded payload bytes (no recursion): the LEN payload, or -- for
        // a proto2 group field -- the scanned group body via `read_group` (dispatched on the SGroup
        // key). Singular rejects a second occurrence like a materialized singular message; repeated
        // pushes each payload.
        printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, message_field_wire(field))}});
        printer.indent();
        if (field.is_repeated) {
            emit_msg_payload_read(printer, field, loop_read());
            emit_push(printer, acc, "rp_p");
        } else {
            printer.print("if $a$.is_some() {\n", {{"a", accessor}});
            printer.indent();
            printer.print(
                "return Err(::rapidproto::DecodeError::new("
                "::rapidproto::ErrorKind::RepeatedSingularMessage, $off$).with_field($n$));\n",
                {{"off", kLoopOff}, {"n", nstr}});
            printer.outdent();
            printer.print("}\n");
            emit_msg_payload_read(printer, field, loop_read());
            printer.print("$a$ = Some(rp_p);\n", {{"a", accessor}});
            if (required) {
                printer.print("rp_req_$n$ = true;\n", {{"n", nstr}});
            }
        }
        printer.outdent();
        printer.print("}\n");
        return;
    }

    if (field.is_message_type && !field.is_repeated) {
        emit_singular_message_arm(printer, table, current_mod, field, accessor, required, member,
                                  mask_bits);
        return;
    }
    if (field.is_message_type && field.is_repeated) {
        printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, message_field_wire(field))}});
        printer.indent();
        emit_repeated_native_body(printer, table, current_mod, field);  // one element -> array slot
        printer.outdent();
        printer.print("}\n");
        return;
    }

    const std::string wire = scalar_wire_variant(field);
    if (field.is_repeated && wire != "Len") {
        // Repeated numeric: an unpacked arm (one value per occurrence) and a packed arm (a
        // length-delimited run of values). proto permits mixing both on the wire.
        printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, wire)}});
        printer.indent();
        emit_repeated_native_body(printer, table, current_mod, field);
        printer.outdent();
        printer.print("}\n");
        emit_packed_arm(printer, table, current_mod, field);
        return;
    }
    if (field.is_repeated) {  // repeated string/bytes (wire == Len, not packable)
        printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, "Len")}});
        printer.indent();
        emit_repeated_native_body(printer, table, current_mod, field);
        printer.outdent();
        printer.print("}\n");
        return;
    }
    // Singular scalar / enum / string / bytes.
    printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, wire)}});
    printer.indent();
    emit_singular_scalar_store(printer, table, current_mod, field, accessor, required, member,
                               mask_bits, loop_read());
    printer.outdent();
    printer.print("}\n");
}

// A map entry is a sub-message with field 1 = key, field 2 = value; synthesize the two `FieldNode`s so
// the scalar/message read + convert + storage-type helpers apply unchanged. Keys are always scalar.
FieldNode map_key_field(const MapFieldNode& map) {
    FieldNode f;
    f.name = "key";
    f.type_name = map.key_type;
    f.number = 1;
    f.presence = FieldPresence::Implicit;
    return f;
}
FieldNode map_value_field(const MapFieldNode& map) {
    FieldNode f;
    f.name = "value";
    f.type_name = map.value_type;
    f.number = 2;
    f.presence = FieldPresence::Implicit;
    f.resolved_type_fqn = map.resolved_value_type_fqn;
    f.is_message_type = map.value_is_message;
    f.is_enum_type = map.value_is_enum;
    return f;
}

// Emit the match arm for one oneof member field: read its value and set the oneof's local to
// `Some(<Enum>::<Variant>(value))`. The last tag wins -- the slot simply overwrites -- EXCEPT a message
// member seen while the oneof already holds THAT member is rejected (`RepeatedSingularMessage`): two
// occurrences of the same sub-message would be merged by protobuf, which a read-only tree cannot do, so
// last-wins there would hand back a value differing from protobuf (matching the C++ decoder and the
// singular-message-field / map-value rules). An intervening different member clears the slot, so that
// later occurrence starts fresh and keeps the plain last-wins. `enum_path` is the `<submodule>::<Enum>`
// prefix; a message variant carries a borrowed `&'a T` (arena-allocated).
// Body of one oneof member decode (no arm wrapper), shared by the hub arm and the threaded prologue
// (both read from `rp_rest` via `loop_read()`).
void emit_oneof_body(Printer& printer, const NameTable& table,
                     const std::vector<std::string>& current_mod, const FieldNode& field,
                     const std::string& oneof_local, const std::string& enum_path,
                     const std::string& variant) {
    const std::string nstr = std::to_string(field.number);
    if (field.is_message_type) {
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("if matches!($l$, Some($e$::$v$(_))) {\n",
                      {{"l", oneof_local}, {"e", enum_path}, {"v", variant}});
        printer.indent();
        printer.print(
            "return Err(::rapidproto::DecodeError::new("
            "::rapidproto::ErrorKind::RepeatedSingularMessage, $off$).with_field($n$));\n",
            {{"off", kLoopOff}, {"n", nstr}});
        printer.outdent();
        printer.print("}\n");
        emit_msg_payload_read(printer, field, loop_read());
        // Decode the oneof's message member directly into its arena slot (no by-value copy).
        printer.print(
            "let rp_ptr = rp_arena.emplace(|rp_out| $p$::decode_into(rp_out, rp_p, rp_arena, "
            "rp_depth "
            "+ 1))?;\n",
            {{"p", path}});
        printer.print("$l$ = Some($e$::$v$(rp_ptr));\n",
                      {{"l", oneof_local}, {"e", enum_path}, {"v", variant}});
        return;
    }
    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    if (wire == "Len") {
        emit_len_read(printer, loop_read());
    } else {
        emit_scalar_value_read(printer, field, loop_read());
    }
    printer.print("$l$ = Some($e$::$v$($c$));\n",
                  {{"l", oneof_local}, {"e", enum_path}, {"v", variant}, {"c", conv}});
}

// Emit the match arm for one oneof member field: read its value and set the oneof's local to
// `Some(<Enum>::<Variant>(value))`. The last tag wins -- the slot simply overwrites -- EXCEPT a message
// member seen while the oneof already holds THAT member is rejected (`RepeatedSingularMessage`): two
// occurrences of the same sub-message would be merged by protobuf, which a read-only tree cannot do, so
// last-wins there would hand back a value differing from protobuf (matching the C++ decoder and the
// singular-message-field / map-value rules). An intervening different member clears the slot, so that
// later occurrence starts fresh and keeps the plain last-wins. `enum_path` is the `<submodule>::<Enum>`
// prefix; a message variant carries a borrowed `&'a T` (arena-allocated).
void emit_oneof_arm(Printer& printer, const NameTable& table,
                    const std::vector<std::string>& current_mod, const FieldNode& field,
                    const std::string& oneof_local, const std::string& enum_path,
                    const std::string& variant) {
    const std::string wire =
        field.is_message_type ? message_field_wire(field) : scalar_wire_variant(field);
    printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, wire)}});
    printer.indent();
    emit_oneof_body(printer, table, current_mod, field, oneof_local, enum_path, variant);
    printer.outdent();
    printer.print("}\n");
}

// Emit the key (field 1) or value (field 2) arm inside a map entry loop, reading into `local` (`rp_key`
// / `rp_val`) at `at` (entry-relative offsets). A message value is decoded by value and, seen twice in
// one entry, rejected (`RepeatedSingularMessage`, keyed to the map field `map_number`) -- matching C++;
// a scalar key/value silently last-wins.
void emit_map_component(Printer& printer, const NameTable& table,
                        const std::vector<std::string>& current_mod, const FieldNode& field,
                        const std::string& local, const ReadAt& at, std::int32_t map_number) {
    if (field.is_message_type) {  // the value field; held in an `Option` (None = not yet seen)
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, "Len")}});
        printer.indent();
        printer.print("if $l$.is_some() {\n", {{"l", local}});
        printer.indent();
        printer.print(
            "return Err(::rapidproto::DecodeError::new("
            "::rapidproto::ErrorKind::RepeatedSingularMessage, $o$).with_field($m$));\n",
            {{"o", at.off}, {"m", std::to_string(map_number)}});
        printer.outdent();
        printer.print("}\n");
        emit_len_read(printer, at);
        // Map value message is stored by value in the entry's `Option`; `decode_value` runs the
        // destination-slot decode and moves the result out (its `unsafe` stays in the runtime).
        printer.print(
            "$l$ = Some(::rapidproto::decode_value(|rp_out| $p$::decode_into(rp_out, rp_p, "
            "rp_arena, "
            "rp_depth + 1))?);\n",
            {{"l", local}, {"p", path}});
        printer.outdent();
        printer.print("}\n");
        return;
    }
    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    printer.print("$k$ => {\n", {{"k", raw_tag_key(field.number, wire)}});
    printer.indent();
    if (wire == "Len") {
        emit_len_read(printer, at);
    } else {
        emit_scalar_value_read(printer, field, at);
    }
    printer.print("$l$ = $c$;\n", {{"l", local}, {"c", conv}});
    printer.outdent();
    printer.print("}\n");
}

// Body of one map ENTRY decode (no arm wrapper): read the length-delimited entry, decode its key/value
// in an inner loop (unknown entry fields skipped), and push `(key, value)` into the map's `DecodeVec`.
// The entry is its own decode frame -- offsets inside it are entry-relative (matching C++'s map
// re-anchor). Shared by the hub `(n,Len)` arm and the threaded prologue's map self-loop; both read the
// entry from `rp_rest` (`loop_read()`), so one call decodes exactly one entry.
void emit_map_body(Printer& printer, const NameTable& table,
                   const std::vector<std::string>& current_mod, const MapFieldNode& map,
                   const std::string& map_local) {
    const FieldNode key = map_key_field(map);
    const FieldNode val = map_value_field(map);
    const ReadAt entry_at{"rp_ent", "rp_ent_body.len() - rp_ent.len()"};
    emit_len_read(printer, loop_read());  // the entry payload -> rp_p
    printer.print("let rp_ent_body: &'a [u8] = rp_p;\n");
    printer.print("let mut rp_key: $t$ = $d$;\n",
                  {{"t", field_value_type(table, current_mod, key)},
                   {"d", field_default_expr(table, current_mod, key, nullptr)}});
    // A message value is stored by value (no `Option`), so it is accumulated in an `Option` here --
    // `None` until seen -- and filled with the default (empty) message if the entry omits field 2.
    if (val.is_message_type) {
        printer.print("let mut rp_val: Option<$t$> = None;\n",
                      {{"t", field_value_type(table, current_mod, val)}});
    } else {
        printer.print("let mut rp_val: $t$ = $d$;\n",
                      {{"t", field_value_type(table, current_mod, val)},
                       {"d", field_default_expr(table, current_mod, val, nullptr)}});
    }
    printer.print("let mut rp_ent: &'a [u8] = rp_ent_body;\n");
    printer.print("loop {\n");
    printer.indent();
    emit_peek_hub(printer, entry_at, "rp_etag");
    printer.print("match rp_tagkey {\n");
    printer.indent();
    emit_map_component(printer, table, current_mod, key, "rp_key", entry_at, map.number);
    emit_map_component(printer, table, current_mod, val, "rp_val", entry_at, map.number);
    printer.print("_ => {\n");
    printer.indent();
    emit_skip_field_body(printer, entry_at);
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n");  // match
    printer.outdent();
    printer.print("}\n");  // loop
    if (val.is_message_type) {
        // An entry that omitted field 2 gets the default (empty) message. (On non-protoc input where
        // the value message has `required` fields, this empty decode surfaces MissingRequired rather
        // than C++'s zero-init; protoc always serializes field 2, so protoc-valid input is unaffected.)
        const std::string path = arena_message_path(table, current_mod, val.resolved_type_fqn);
        printer.print("let rp_val = match rp_val {\n");
        printer.indent();
        printer.print("Some(rp_sub) => rp_sub,\n");
        printer.print(
            "None => ::rapidproto::decode_value(|rp_out| $p$::decode_into(rp_out, &[], rp_arena, "
            "rp_depth + 1))?,\n",
            {{"p", path}});
        printer.outdent();
        printer.print("};\n");
    }
    emit_push(printer, map_local, "(rp_key, rp_val)");
}

// Emit the match arm for one map field (body shared with the threaded prologue's map self-loop).
void emit_map_arm(Printer& printer, const NameTable& table,
                  const std::vector<std::string>& current_mod, const MapFieldNode& map,
                  const std::string& map_local) {
    printer.print("$k$ => {\n", {{"k", raw_tag_key(map.number, "Len")}});
    printer.indent();
    emit_map_body(printer, table, current_mod, map, map_local);
    printer.outdent();
    printer.print("}\n");
}

// Emit `decode()` + the `decode_into()` worker for a decodable message: a field loop over `rp_body`
// driven by a one-byte peek hub (`emit_peek_hub`) that reads each tag's raw key into `rp_tagkey`, then
// `match rp_tagkey` to read each known regular / oneof-member / map field by its `field << 3 | wire`
// key (a wrong wire type for a known field falls to `skip_field`, like an unknown field), accumulating
// repeated fields and maps in `DecodeVec`s and recursing into sub-messages, then constructing the
// message (checking proto2 `required` fields were all seen). `decode_into` is `pub(crate)` so
// sub-messages in other modules can recurse into it.
// The decode locals: a repeated field / map accumulates into a `DecodeVec`; a oneof holds its enum
// (None until a member is seen); a proto2 `required` field a transient seen-flag; `unknown-fields` a
// bool; the rest plain values at their proto3 zero / None default. A dropped field/map has no local
// (its arm skip-validates and discards). `submodule` names the message's own oneof-enum submodule.
void emit_decode_locals(Printer& printer, const NameTable& table,
                        const std::vector<std::string>& current_mod, const MessageNode& msg,
                        const FieldModes& modes, const AccessorNames& accessors,
                        const std::string& submodule, const arenagen::MessageLayout* layout) {
    const int mask_bits = layout != nullptr ? rust_mask_bits(*layout) : 0;
    if (mask_bits > 0) {
        // The presence/bool mask, built up as fields decode and stored in the construct. A `[u64; N]`
        // zero is `[0; N]`; a single word is `0`.
        printer.print(
            "let mut rp_presence: $t$ = $z$;\n",
            {{"t", mask_word_type(mask_bits)},
             {"z",
              mask_bits > kMaskWordBits
                  ? "[0; " + std::to_string((mask_bits + kMaskWordBits - 1) / kMaskWordBits) + "]"
                  : std::string("0")}});
    }
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        const FieldNode& field = msg.fields[i];
        if (field_mode(modes, field) == arenagen::FieldMode::Drop) {
            continue;
        }
        const arenagen::MemberPlan* member = member_for(layout, field);
        if (field.is_repeated) {
            printer.print("let mut rp_acc_$n$ = ::rapidproto::DecodeVec::new(rp_arena);\n",
                          {{"n", std::to_string(field.number)}});
        } else if (rust_store(member, field) != RustStore::MaskedBool) {
            // A packed bool has no local -- its value and presence live entirely in the mask.
            printer.print("let mut $l$: $t$ = $d$;\n",
                          {{"l", accessors.fields[i]},
                           {"t", field_storage_type(table, current_mod, field, modes, member)},
                           {"d", field_default_expr(table, current_mod, field, member)}});
        }
        if (field.presence == FieldPresence::Required) {
            printer.print("let mut rp_req_$n$ = false;\n", {{"n", std::to_string(field.number)}});
        }
    }
    if (modes.wants_unknown(&msg)) {
        printer.print("let mut rp_unknown = false;\n");  // set by the unknown-field catch-all
    }
    for (const MapFieldNode& map : msg.map_fields) {
        if (map_mode(modes, map) != arenagen::FieldMode::Drop) {
            printer.print("let mut rp_map_$n$ = ::rapidproto::DecodeVec::new(rp_arena);\n",
                          {{"n", std::to_string(map.number)}});
        }
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        printer.print(
            "let mut rp_oneof_$i$: $t$ = None;\n",
            {{"i", std::to_string(i)}, {"t", oneof_storage_type(table, msg.oneofs[i], submodule)}});
    }
}

// The `match rp_tagkey` arm for every regular / map / oneof-member field (keyed by its raw tag
// `field << 3 | wire`), then a `_` catch-all that skips anything else. A dropped field/map gets no
// arm -- it falls to the catch-all and is skipped like an unknown field. The catch-all reconstructs
// and skips via `skip_field` (the peek hub handed it only the raw key). In an unknown-tracked message
// the catch-all first routes on the field NUMBER (`rp_tagkey >> 3`): a declared number (a kept or
// dropped field/map/oneof member arriving with the wrong wire type) is skipped silently, an undeclared
// one trips the unknown-fields bit -- matching C++ (which flags only unrecognized field numbers, never
// a known number with a wrong wire type).
void emit_decode_arms(Printer& printer, const NameTable& table,
                      const std::vector<std::string>& current_mod, const MessageNode& msg,
                      const FieldModes& modes, const AccessorNames& accessors,
                      const std::string& submodule, const arenagen::MessageLayout* layout) {
    const bool wants_unknown = modes.wants_unknown(&msg);
    const int mask_bits = layout != nullptr ? rust_mask_bits(*layout) : 0;
    // Every declared field number (regular -- kept OR dropped -- plus map and oneof-member numbers)
    // as an or-pattern: the set the catch-all treats as "known" so a wrong-wire occurrence never trips
    // the unknown bit. Built only for an unknown-tracked message (unused otherwise).
    std::string known_numbers;
    const auto note_known = [&](std::int64_t number) {
        if (!known_numbers.empty()) {
            known_numbers += " | ";
        }
        known_numbers += std::to_string(number) + "u32";
    };
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        emit_field_arms(printer, table, current_mod, msg.fields[i], accessors.fields[i],
                        msg.fields[i].presence == FieldPresence::Required, modes,
                        member_for(layout, msg.fields[i]), mask_bits);
        note_known(msg.fields[i].number);
    }
    for (const MapFieldNode& map : msg.map_fields) {
        if (map_mode(modes, map) != arenagen::FieldMode::Drop) {
            emit_map_arm(printer, table, current_mod, map, "rp_map_" + std::to_string(map.number));
        }
        note_known(map.number);
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        const OneofNode& oneof = msg.oneofs[i];
        const std::string enum_path = submodule + "::" + table.oneofs.at(&oneof);
        const std::vector<std::string> variants = oneof_variant_names(oneof);
        const std::string local = "rp_oneof_" + std::to_string(i);
        for (std::size_t j = 0; j < oneof.fields.size(); ++j) {
            emit_oneof_arm(printer, table, current_mod, oneof.fields[j], local, enum_path,
                           variants[j]);
            note_known(oneof.fields[j].number);
        }
    }
    printer.print("_ => {\n");
    printer.indent();
    if (wants_unknown) {
        // A declared field number with a wrong wire type is still "known" (skip, no bit); only an
        // undeclared number trips the unknown bit. The `_` arm cannot pattern out the field bits of a
        // single key, so route on `rp_tagkey >> 3` in the cold catch-all. A fieldless message has no
        // known numbers, so every field on the wire is unknown -- set the bit unconditionally (an empty
        // `matches!(.., )` pattern is a Rust syntax error).
        if (known_numbers.empty()) {
            printer.print("rp_unknown = true;\n");
        } else {
            printer.print("if !matches!(rp_tagkey >> 3, $k$) {\n", {{"k", known_numbers}});
            printer.indent();
            printer.print("rp_unknown = true;\n");  // a genuinely unknown field number was seen
            printer.outdent();
            printer.print("}\n");
        }
    }
    emit_skip_field_body(printer, loop_read());
    printer.outdent();
    printer.print("}\n");
}

// The `Ok(Msg { .. })` construct from the decode locals; a dropped field/map contributes no storage,
// `unknown-fields` the bit, and a message with no `'a`-using field the `PhantomData` marker.
void emit_decode_construct(Printer& printer, const MessageNode& msg, const FieldModes& modes,
                           const AccessorNames& accessors, const arenagen::MessageLayout* layout) {
    // Construct the message directly into the caller's slot (`Slot::write` is safe -- no `unsafe` in
    // generated code) and return the `Init` proof it yields, so LLVM builds the struct in place and
    // the caller's `emplace`/`decode_value` needs no init-on-trust. Mirrors the C++ decoder writing
    // fields straight into `out`.
    printer.print("Ok(rp_out.write($M$ {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        if (field_mode(modes, msg.fields[i]) == arenagen::FieldMode::Drop) {
            continue;
        }
        if (msg.fields[i].is_repeated) {
            printer.print(
                "$l$: rp_acc_$n$.into_slice(),\n",
                {{"l", accessors.fields[i]}, {"n", std::to_string(msg.fields[i].number)}});
        } else if (rust_store(member_for(layout, msg.fields[i]), msg.fields[i]) !=
                   RustStore::MaskedBool) {
            // A packed bool contributes no struct field (its value/presence are in `rp_presence`).
            printer.print("$l$,\n", {{"l", accessors.fields[i]}});
        }
    }
    if (layout != nullptr && rust_mask_bits(*layout) > 0) {
        printer.print("rp_presence,\n");  // the packed presence/bool mask
    }
    for (std::size_t i = 0; i < msg.map_fields.size(); ++i) {
        if (map_mode(modes, msg.map_fields[i]) != arenagen::FieldMode::Drop) {
            printer.print(
                "$l$: ::rapidproto::MapView::new(rp_map_$n$.into_slice()),\n",
                {{"l", accessors.maps[i]}, {"n", std::to_string(msg.map_fields[i].number)}});
        }
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        printer.print("$l$: rp_oneof_$i$,\n",
                      {{"l", accessors.oneofs[i]}, {"i", std::to_string(i)}});
    }
    if (modes.wants_unknown(&msg)) {
        printer.print("rp_unknown,\n");  // the has_unknown_fields() storage
    }
    if (!message_uses_lifetime(msg, modes)) {
        printer.print("_marker: ::core::marker::PhantomData,\n");
    }
    printer.outdent();
    printer.print("}))\n");
}

// A field the threaded prologue will probe, with everything its body emitter needs. Collected across a
// message's regular fields and oneof members by `collect_threaded_items`, then emitted in ascending
// field-number order.
enum class ThreadKind : std::uint8_t { Scalar, Message, Oneof, Repeated, Map };
struct ThreadItem {
    int number;
    ThreadKind kind;
    bool packable;     // repeated numeric: also probe its packed LEN form
    std::string wire;  // the native/singular tag's wire variant (what the probe matches)
    const FieldNode* field;
    std::string accessor;  // scalar/message storage local, or the map's `rp_map_<n>` accumulator
    bool required;
    const arenagen::MemberPlan* member;
    std::string oneof_local, enum_path, variant;  // oneof members only
    const MapFieldNode* map = nullptr;            // non-null only for ThreadKind::Map
};

// Append the threadable maps of `msg` to `items`. A map is a repeated series of LEN entries, so it
// self-loops like a repeated field; threaded up to a two-byte tag (<= 2047). A dropped map is left to
// the hub (skipped there).
void append_map_threaded_items(const MessageNode& msg, const FieldModes& modes,
                               std::vector<ThreadItem>& items) {
    for (const MapFieldNode& map : msg.map_fields) {
        if (map_mode(modes, map) == arenagen::FieldMode::Drop || map.number < 1 ||
            map.number > codegen::kMaxTwoByteTagField) {
            continue;
        }
        items.push_back({map.number, ThreadKind::Map, false, "Len", nullptr,
                         "rp_map_" + std::to_string(map.number), false, nullptr, "", "", "", &map});
    }
}

// Append the threadable oneof members of `msg` to `items` (each oneof member shares the field-number
// space, so they interleave with regular fields once the caller sorts).
void append_oneof_threaded_items(const NameTable& table, const MessageNode& msg,
                                 const std::string& submodule, std::vector<ThreadItem>& items) {
    for (std::size_t oi = 0; oi < msg.oneofs.size(); ++oi) {
        const OneofNode& oneof = msg.oneofs[oi];
        const std::string enum_path = submodule + "::" + table.oneofs.at(&oneof);
        const std::vector<std::string> variants = oneof_variant_names(oneof);
        const std::string local = "rp_oneof_" + std::to_string(oi);
        for (std::size_t j = 0; j < oneof.fields.size(); ++j) {
            const FieldNode& mf = oneof.fields[j];
            if (!codegen::is_threadable_singular(mf)) {
                continue;
            }
            const std::string wire = mf.is_message_type ? "Len" : scalar_wire_variant(mf);
            items.push_back({mf.number, ThreadKind::Oneof, false, wire, &mf, "", false, nullptr,
                             local, enum_path, variants[j]});
        }
    }
}

// Collect the fields this message threads (see `emit_threaded_prologue` for the scope). Regular fields
// and oneof members are gathered together; the caller sorts by field number before emitting.
std::vector<ThreadItem> collect_threaded_items(const NameTable& table, const MessageNode& msg,
                                               const FieldModes& modes,
                                               const AccessorNames& accessors,
                                               const std::string& submodule,
                                               const arenagen::MessageLayout* layout) {
    std::vector<ThreadItem> items;
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        const FieldNode& f = msg.fields[i];
        const arenagen::FieldMode mode = field_mode(modes, f);
        if (mode == arenagen::FieldMode::Drop || mode == arenagen::FieldMode::Raw) {
            continue;  // dropped/raw fields are never threaded (handled by the hub loop / catch-all)
        }
        if (f.is_repeated) {
            // Repeated fields self-loop on their own element tag. A group element (SGROUP/EGROUP-
            // delimited) must NOT thread as a bare LEN self-loop -- it dispatches on the SGroup hub arm
            // and reads via `read_group` -- so a repeated group field is excluded here (mirroring
            // arenagen's `is_threaded` "delimited never thread"); its elements decode through the hub.
            // Unlike the C++ reference (repeated threads at a one-byte tag only), Rust threads up to a
            // two-byte tag (<= 2047): lacking C++'s goto re-entry, an unthreaded field ends the
            // straight-line prologue for everything after it, so a two-byte repeated field left out
            // would break threading of a well-ordered message's tail.
            const bool group =
                f.is_message_type && f.message_encoding == MessageEncoding::Delimited;
            if (f.number >= 1 && f.number <= codegen::kMaxTwoByteTagField && !group) {
                const std::string wire = f.is_message_type ? "Len" : scalar_wire_variant(f);
                const bool packable = !f.is_message_type && wire != "Len";
                items.push_back({f.number, ThreadKind::Repeated, packable, wire, &f, "", false,
                                 nullptr, "", "", ""});
            }
            continue;
        }
        if (!codegen::is_threadable_singular(f)) {
            continue;  // number out of 1..2047, or a delimited (group) message -- not threadable
        }
        const bool required = f.presence == FieldPresence::Required;
        const ThreadKind kind = f.is_message_type ? ThreadKind::Message : ThreadKind::Scalar;
        const std::string wire = f.is_message_type ? "Len" : scalar_wire_variant(f);
        items.push_back({f.number, kind, false, wire, &f, accessors.fields[i], required,
                         member_for(layout, f), "", "", ""});
    }
    append_map_threaded_items(msg, modes, items);
    append_oneof_threaded_items(table, msg, submodule, items);
    return items;
}

// Open a threading probe: an `if`/`while` that matches the field's whole constant tag against the next
// input byte(s) and, on a match, consumes exactly that tag. `wire` is the tag's wire variant; one byte
// for field <= 15, two for 16..2047 (`b0` = low 7 bits + continuation, `b1` = next 7). The caller emits
// the body, then `emit_probe_close`.
void emit_probe_open(Printer& printer, int number, const std::string& wire, bool self_loop) {
    const std::int64_t v = raw_tag_value(number, wire);
    const char* kw = self_loop ? "while" : "if";
    if (number <= codegen::kMaxOneByteTagField) {
        printer.print("$kw$ rp_rest.first() == Some(&$t$u8) {\n",
                      {{"kw", kw}, {"t", std::to_string(v)}});
        printer.indent();
        printer.print("rp_rest = &rp_rest[1..];\n");
    } else {
        const std::int64_t b0 = (v & codegen::kVarintPayload) | codegen::kVarintContinue;
        const std::int64_t b1 = v >> codegen::kVarintShift;
        printer.print("$kw$ rp_rest.first_chunk::<2>() == Some(&[$b0$u8, $b1$u8]) {\n",
                      {{"kw", kw}, {"b0", std::to_string(b0)}, {"b1", std::to_string(b1)}});
        printer.indent();
        printer.print("rp_rest = &rp_rest[2..];\n");
    }
}

void emit_probe_close(Printer& printer) {
    printer.outdent();
    printer.print("}\n");
}

// Field-order threading: before the general peek-hub loop, probe each threadable field in field-number
// order with a cheap constant-tag compare; on a match, consume the tag and decode+store the value
// inline (via the SAME body emitters the hub arms use), then fall through to the next probe. Encoders
// write fields in ascending order, so the common case runs this straight-line chain with no tag-varint
// parse and no `match` dispatch -- the hub loop then handles only what the prologue did not consume
// (out-of-order, repeats, unknown/wrong-wire, or non-threaded fields), so correctness and last-wins
// semantics are unchanged (the prologue is a pure fast path over the SAME validated reads and stores).
// Rust has no goto, so each threaded field's body is duplicated here and in its hub arm (decode code
// grows for threaded fields); the tail-call form that would share one body relies on opportunistic TCO
// (an unbounded-stack risk on adversarial input), so the robust straight-line form is used -- the same
// choice the C++ reference makes.
//
// Scope: every singular field admitted by codegen::is_threadable_singular (scalar/enum/bool/string/
// bytes/message, one- or two-byte tag, non-group) AND oneof members likewise; plus repeated non-group
// fields and maps (one- or two-byte tag <= 2047), which self-loop on their own tag (a repeated numeric
// also probes its packed LEN form). This is BROADER than the C++ reference's is_threaded (repeated
// one-byte only; maps never): C++ re-enters threading from its hub via goto, so an unthreaded field
// mid-message costs it nothing, but Rust's straight-line prologue can't re-enter -- once it falls to
// the hub it stays there -- so leaving a map or a two-byte repeated field unthreaded would end
// threading for every later field of a well-ordered message. Threading them keeps a well-ordered
// message (fields <= 2047) on the fast path end to end. Decoded output is unchanged either way
// (threading is a pure fast path; the hub decodes whatever the prologue did not).
void emit_threaded_prologue(Printer& printer, const NameTable& table,
                            const std::vector<std::string>& current_mod, const MessageNode& msg,
                            const FieldModes& modes, const AccessorNames& accessors,
                            const std::string& submodule, const arenagen::MessageLayout* layout) {
    const int mask_bits = layout != nullptr ? rust_mask_bits(*layout) : 0;
    std::vector<ThreadItem> items =
        collect_threaded_items(table, msg, modes, accessors, submodule, layout);
    // Probe in ascending field-number order to match the typical ascending wire order.
    std::sort(items.begin(), items.end(),
              [](const ThreadItem& a, const ThreadItem& b) { return a.number < b.number; });
    for (const ThreadItem& it : items) {
        // Repeated fields and maps are a series of same-tag entries, so they self-loop (`while`);
        // singular fields and oneof members occur once (`if`).
        const bool self_loop = it.kind == ThreadKind::Repeated || it.kind == ThreadKind::Map;
        emit_probe_open(printer, it.number, it.wire, self_loop);
        switch (it.kind) {
            case ThreadKind::Scalar:
                emit_singular_scalar_store(printer, table, current_mod, *it.field, it.accessor,
                                           it.required, it.member, mask_bits, loop_read());
                break;
            case ThreadKind::Message:
                emit_singular_message_body(printer, table, current_mod, *it.field, it.accessor,
                                           it.required, it.member, mask_bits);
                break;
            case ThreadKind::Oneof:
                emit_oneof_body(printer, table, current_mod, *it.field, it.oneof_local,
                                it.enum_path, it.variant);
                break;
            case ThreadKind::Repeated:
                emit_repeated_native_body(printer, table, current_mod, *it.field);
                break;
            case ThreadKind::Map:
                emit_map_body(printer, table, current_mod, *it.map, it.accessor);
                break;
        }
        emit_probe_close(printer);
        if (it.kind == ThreadKind::Repeated && it.packable) {
            // A packed run of the same field: one LEN payload. Probed once (a second run falls to the
            // hub loop).
            emit_probe_open(printer, it.number, "Len", /*self_loop=*/false);
            emit_packed_body(printer, table, current_mod, *it.field);
            emit_probe_close(printer);
        }
    }
}

void emit_decode(Printer& printer, const NameTable& table,
                 const std::vector<std::string>& current_mod, const MessageNode& msg,
                 const AccessorNames& accessors, const FieldModes& modes,
                 const arenagen::MessageLayout* layout) {
    printer.print(
        "pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) "
        "-> Result<&'a $M$<'a>, ::rapidproto::DecodeError> {\n");
    printer.indent();
    printer.print("if input.len() > u32::MAX as usize {\n");
    printer.indent();
    printer.print(
        "return Err(::rapidproto::DecodeError::new(::rapidproto::ErrorKind::InputTooLarge, 0));\n");
    printer.outdent();
    printer.print("}\n");
    // Decode the root message directly into its final arena slot (no by-value copy), mirroring the
    // caller side of every sub-message decode below.
    printer.print(
        "let rp_root = arena.emplace(|rp_out| Self::decode_into(rp_out, input, arena, 0))?;\n");
    printer.print("Ok(&*rp_root)\n");
    printer.outdent();
    printer.print("}\n\n");

    // Emulate the C++ decoder's RP_FLATTEN / RP_NOINLINE, driven by the SAME flatten-budget decision
    // (`plan_layouts` sets `noinline_decode`, so the two generators flatten identically). A message
    // under budget is `#[inline(always)]`, so a parent's decode absorbs it -- and, since its own
    // sub-message decoders are likewise `#[inline(always)]` until the next marked message, the whole
    // closure collapses into one function (the Rust analog of flatten). A budget-marked message is
    // `#[inline(never)]`: the out-of-line boundary that stops a parent from absorbing it, which bounds
    // whole-closure (and compile-time) growth on a large or recursive schema. A message with no layout
    // (defensive; decodable messages always have one) keeps the compiler's default.
    if (layout != nullptr) {
        printer.print(layout->noinline_decode ? "#[inline(never)]\n" : "#[inline(always)]\n");
    }
    // `rp_arena` is unused when the body neither accumulates a repeated field nor allocates a
    // sub-message (a message of only singular scalar/enum/string/bytes fields); name it `_rp_arena`
    // then to avoid an unused-variable warning.
    printer.print("#[doc(hidden)]\n");
    // Takes a branded write-once `Slot` and returns the `Init<'id>` proof that `Slot::write` yields --
    // an unforgeable witness that the slot was initialized. The in-place helpers
    // (`Arena::emplace`/`DecodeVec::emplace`/`decode_value`) publish the slot only against that proof,
    // so they stay sound for ANY safe caller (no init-on-trust), not just this generated one.
    printer.print(
        "pub(crate) fn decode_into<'id>(rp_out: ::rapidproto::Slot<'id, $M$<'a>>, rp_body: "
        "&'a [u8], "
        "$ap$: &'a ::rapidproto::Arena<'a>, rp_depth: u32) -> Result<::rapidproto::Init<'id>, "
        "::rapidproto::DecodeError> "
        "{\n",
        {{"ap", body_uses_arena(msg, modes) ? "rp_arena" : "_rp_arena"}});
    printer.indent();
    printer.print("if rp_depth > ::rapidproto::MAX_DECODE_DEPTH {\n");
    printer.indent();
    printer.print(
        "return "
        "Err(::rapidproto::DecodeError::new(::rapidproto::ErrorKind::DepthLimitExceeded, 0));\n");
    printer.outdent();
    printer.print("}\n");
    // The decode impl is emitted at the end of the message's OWN module, so the oneof enum -- in the
    // message's child submodule -- is reached by its bare submodule name.
    const std::string submodule =
        msg.oneofs.empty() ? std::string() : table.messages.at(msg.fqn).submodule;
    emit_decode_locals(printer, table, current_mod, msg, modes, accessors, submodule, layout);
    printer.print("let mut rp_rest: &'a [u8] = rp_body;\n");
    emit_threaded_prologue(printer, table, current_mod, msg, modes, accessors, submodule, layout);
    printer.print("loop {\n");
    printer.indent();
    emit_peek_hub(printer, loop_read(), "rp_tag");
    printer.print("match rp_tagkey {\n");
    printer.indent();
    emit_decode_arms(printer, table, current_mod, msg, modes, accessors, submodule, layout);
    printer.outdent();
    printer.print("}\n");  // match
    printer.outdent();
    printer.print("}\n");  // loop
    // proto2 `required`: every required field must have appeared on the wire (checked after the loop,
    // matching C++), else the message is incomplete.
    for (const FieldNode& field : msg.fields) {
        if (field.presence == FieldPresence::Required) {
            const std::string nstr = std::to_string(field.number);
            printer.print("if !rp_req_$n$ {\n", {{"n", nstr}});
            printer.indent();
            printer.print(
                "return Err(::rapidproto::DecodeError::new("
                "::rapidproto::ErrorKind::MissingRequired, 0).with_field($n$));\n",
                {{"n", nstr}});
            printer.outdent();
            printer.print("}\n");
        }
    }
    emit_decode_construct(printer, msg, modes, accessors, layout);
    printer.outdent();
    printer.print("}\n");
}

// The decode body's internal identifiers (fixed locals + `_marker`, plus each repeated field / map
// accumulator, oneof slot and required seen-flag), reserved so no field accessor -- which is also the
// struct storage field name -- collides with a decode local.
std::unordered_set<std::string> decode_reserved_idents(const MessageNode& msg) {
    // Fixed decode-body locals (plus `_marker`): `rp_b`/`rp_tagkey` are the peek hub's tag byte and raw
    // key; `rp_unknown` the has_unknown_fields() storage + its decode flag; `rp_presence` the packed
    // presence/bool mask (also a storage field).
    std::unordered_set<std::string> reserved = {
        "decode_into", "_marker",    "rp_body",    "rp_arena", "_rp_arena",   "rp_depth",
        "rp_rest",     "rp_tag",     "rp_v",       "rp_p",     "rp_sub",      "rp_ptr",
        "rp_etag",     "rp_ent",     "rp_key",     "rp_val",   "rp_ent_body", "rp_b",
        "rp_tagkey",   "rp_unknown", "rp_presence"};
    for (const FieldNode& field : msg.fields) {
        if (field.is_repeated) {
            reserved.insert("rp_acc_" + std::to_string(field.number));
        }
        if (field.presence == FieldPresence::Required) {
            reserved.insert("rp_req_" + std::to_string(field.number));
        }
    }
    for (const MapFieldNode& map : msg.map_fields) {
        reserved.insert("rp_map_" + std::to_string(map.number));
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        reserved.insert("rp_oneof_" + std::to_string(i));
    }
    return reserved;
}

// Emit `pub struct Msg<'a> { .. }` -- the storage fields (= getters' return types). `current_mod` is
// the module the struct is emitted in, used to compute cross-references. A dropped field/map has no
// storage; a `raw` message field stores its payload bytes; `unknown-fields` adds the bit.
void emit_message_struct(Printer& printer, const NameTable& table,
                         const std::vector<std::string>& current_mod, const MessageNode& msg,
                         const FieldModes& modes, const AccessorNames& accessors,
                         const std::string& submodule, const arenagen::MessageLayout* layout) {
    // A fixed-size message can `derive(Default)` (all fields are scalars/enum newtypes/nested fixed
    // messages/the mask word, all `Default`), giving the zero value an absent inlined sub-message uses.
    if (layout != nullptr && layout->fixed_size) {
        printer.print("#[derive(Default)]\n");
    }
    printer.print("pub struct $M$<'a> {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        const FieldNode& field = msg.fields[i];
        if (field_mode(modes, field) == arenagen::FieldMode::Drop) {
            continue;
        }
        const arenagen::MemberPlan* member = member_for(layout, field);
        if (!field.is_repeated && rust_store(member, field) == RustStore::MaskedBool) {
            continue;  // a packed bool has no struct field (it lives in `rp_presence`)
        }
        printer.print("$l$: $t$,\n",
                      {{"l", accessors.fields[i]},
                       {"t", field_storage_type(table, current_mod, field, modes, member)}});
    }
    if (layout != nullptr && rust_mask_bits(*layout) > 0) {
        printer.print("rp_presence: $t$,\n", {{"t", mask_word_type(rust_mask_bits(*layout))}});
    }
    for (std::size_t i = 0; i < msg.map_fields.size(); ++i) {
        if (map_mode(modes, msg.map_fields[i]) != arenagen::FieldMode::Drop) {
            printer.print("$l$: $t$,\n",
                          {{"l", accessors.maps[i]},
                           {"t", map_view_type(table, current_mod, msg.map_fields[i])}});
        }
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        printer.print("$l$: $t$,\n", {{"l", accessors.oneofs[i]},
                                      {"t", oneof_storage_type(table, msg.oneofs[i], submodule)}});
    }
    if (modes.wants_unknown(&msg)) {
        printer.print("rp_unknown: bool,\n");  // has_unknown_fields() storage
    }
    if (!message_uses_lifetime(msg, modes)) {
        printer.print("_marker: ::core::marker::PhantomData<&'a ()>,\n");
    }
    printer.outdent();
    printer.print("}\n\n");
}

// Whether a message exposes any getter (so `emit_message_accessors` emits an `impl` block at all).
bool message_has_accessors(const MessageNode& msg, const FieldModes& modes) {
    if (modes.wants_unknown(&msg) || !msg.oneofs.empty()) {
        return true;
    }
    if (std::any_of(msg.fields.begin(), msg.fields.end(), [&](const FieldNode& field) {
            return field_mode(modes, field) != arenagen::FieldMode::Drop;
        })) {
        return true;
    }
    return std::any_of(msg.map_fields.begin(), msg.map_fields.end(), [&](const MapFieldNode& map) {
        return map_mode(modes, map) != arenagen::FieldMode::Drop;
    });
}

// Emit `impl<'a> Msg<'a> { <getters> }` (skipped when nothing is exposed). A dropped field/map has no
// getter; `unknown-fields` adds `has_unknown_fields()`. The decode impl is emitted separately, later.
void emit_message_accessors(Printer& printer, const NameTable& table,
                            const std::vector<std::string>& current_mod, const MessageNode& msg,
                            const FieldModes& modes, const AccessorNames& accessors,
                            const std::string& submodule, const arenagen::MessageLayout* layout) {
    if (!message_has_accessors(msg, modes)) {
        return;
    }
    const int mask_bits = layout != nullptr ? rust_mask_bits(*layout) : 0;
    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        if (field_mode(modes, msg.fields[i]) != arenagen::FieldMode::Drop) {
            emit_field_accessor(printer, table, current_mod, msg.fields[i], accessors.fields[i],
                                modes, member_for(layout, msg.fields[i]), mask_bits);
        }
    }
    for (std::size_t i = 0; i < msg.map_fields.size(); ++i) {
        if (map_mode(modes, msg.map_fields[i]) != arenagen::FieldMode::Drop) {
            emit_map_accessor(printer, table, current_mod, msg.map_fields[i], accessors.maps[i]);
        }
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        emit_oneof_accessor(printer, table, msg.oneofs[i], accessors.oneofs[i], submodule);
    }
    if (modes.wants_unknown(&msg)) {
        printer.print("pub fn has_unknown_fields(&self) -> bool {\n");
        printer.indent();
        printer.print("self.rp_unknown\n");
        printer.outdent();
        printer.print("}\n");
    }
    printer.outdent();
    printer.print("}\n");
}

// --- serde Serialize (behind the `serde` feature) --------------------------------------------------
// A message serializes as a map keyed by PROTO field names, carrying exactly the fields the debug
// dumper prints (so the two agree): explicit => when set; proto3 implicit => when non-default;
// required => always; repeated/map => when non-empty; a oneof => its active member as a direct entry.
// Values route through serde -- `bytes` via the Bytes helper (one opaque value, not a seq of u8),
// an open enum as its i32, strings/sub-messages/maps through their own impls. Reads the SAME public
// getters as the accessors, so a wrong decoded value shows up here too.

// A Rust string literal for a proto field name (names are `[A-Za-z0-9_]`, so no escaping needed).
std::string rust_str_lit(const std::string& name) {
    return "\"" + name + "\"";
}

// Is a field/map-value/oneof-member a `bytes` scalar? (Shared so the three serialize sites -- field,
// map value, oneof member, each over a different node struct -- test it one way.) The two bool flags
// are both negated and ANDed, so transposing them cannot change the result.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool is_scalar_bytes(bool is_message, bool is_enum, const std::string& type_name) {
    return !is_message && !is_enum && type_name == "bytes";
}

// Serialized as an opaque `bytes` value? True for a `bytes` scalar and for any raw-mode field (its
// payload materializes as bytes).
bool serialize_as_bytes(const FieldModes& modes, const FieldNode& field) {
    return field_mode(modes, field) == arenagen::FieldMode::Raw ||
           is_scalar_bytes(field.is_message_type, field.is_enum_type, field.type_name);
}

void emit_field_serialize(Printer& printer, const FieldNode& field, const std::string& accessor,
                          const FieldModes& modes) {
    const std::string key = rust_str_lit(field.name);
    const arenagen::FieldMode mode = field_mode(modes, field);
    const bool bytes = serialize_as_bytes(modes, field);
    printer.print("{\n");
    printer.indent();
    if (field.is_repeated) {
        printer.print("let rp_v = self.$a$();\n", {{"a", accessor}});
        printer.print("if !rp_v.is_empty() {\n");
        printer.indent();
        if (bytes) {
            printer.print(
                "rp_map.serialize_entry($n$, &::rapidproto::serde_support::BytesSeq(rp_v))?;\n",
                {{"n", key}});
        } else {
            printer.print("rp_map.serialize_entry($n$, &rp_v)?;\n", {{"n", key}});
        }
        printer.outdent();
        printer.print("}\n");
    } else if (mode == arenagen::FieldMode::Raw) {
        // Raw singular (a message/group kept as its wire payload) comes back as `Option<&[u8]>`.
        printer.print("if let Some(rp_v) = self.$a$() {\n", {{"a", accessor}});
        printer.indent();
        printer.print("rp_map.serialize_entry($n$, &::rapidproto::serde_support::Bytes(rp_v))?;\n",
                      {{"n", key}});
        printer.outdent();
        printer.print("}\n");
    } else if (field.is_message_type) {
        // A singular message getter returns `Option<&Msg>` for every presence (required => Some).
        printer.print("if let Some(rp_v) = self.$a$() {\n", {{"a", accessor}});
        printer.indent();
        printer.print("rp_map.serialize_entry($n$, rp_v)?;\n", {{"n", key}});
        printer.outdent();
        printer.print("}\n");
    } else {
        const std::string val = bytes ? "&::rapidproto::serde_support::Bytes(rp_v)" : "&rp_v";
        if (field.presence == FieldPresence::Explicit) {
            printer.print("if let Some(rp_v) = self.$a$() {\n", {{"a", accessor}});
            printer.indent();
            printer.print("rp_map.serialize_entry($n$, $val$)?;\n", {{"n", key}, {"val", val}});
            printer.outdent();
            printer.print("}\n");
        } else if (field.presence == FieldPresence::Required) {
            printer.print("let rp_v = self.$a$();\n", {{"a", accessor}});
            printer.print("rp_map.serialize_entry($n$, $val$)?;\n", {{"n", key}, {"val", val}});
        } else {  // Implicit (proto3 singular): emit only a non-default value, as protobuf JSON does.
            std::string cond;
            if (field.is_enum_type) {
                cond = "rp_v.0 != 0";
            } else if (field.type_name == "bool") {
                cond = "rp_v";
            } else if (field.type_name == "float" || field.type_name == "double") {
                // Only +0.0 is the omittable default; -0.0 (and NaN) are present -- matching the
                // dumper's is_positive_zero rule and protobuf. `!= 0.0` would wrongly drop -0.0.
                cond = "rp_v.to_bits() != 0";
            } else if (field.type_name == "string" || field.type_name == "bytes") {
                cond = "!rp_v.is_empty()";
            } else {
                cond = "rp_v != 0";
            }
            printer.print("let rp_v = self.$a$();\n", {{"a", accessor}});
            printer.print("if $c$ {\n", {{"c", cond}});
            printer.indent();
            printer.print("rp_map.serialize_entry($n$, $val$)?;\n", {{"n", key}, {"val", val}});
            printer.outdent();
            printer.print("}\n");
        }
    }
    printer.outdent();
    printer.print("}\n");
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): accessor (method name) vs submodule (module path)
void emit_oneof_serialize(Printer& printer, const NameTable& table, const OneofNode& oneof,
                          const std::string& accessor, const std::string& submodule) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const std::vector<std::string> variants = oneof_variant_names(oneof);
    const std::string enum_path = submodule + "::" + table.oneofs.at(&oneof);
    printer.print("if let Some(rp_o) = self.$a$() {\n", {{"a", accessor}});
    printer.indent();
    printer.print("match rp_o {\n");
    printer.indent();
    for (std::size_t i = 0; i < oneof.fields.size(); ++i) {
        const FieldNode& member = oneof.fields[i];
        const std::string key = rust_str_lit(member.name);
        // A `bytes` member is matched as `&&[u8]`; deref once for the Bytes helper. Everything else
        // (scalar/enum by value, string, message-by-ref) is passed through the bound reference.
        const std::string val =
            is_scalar_bytes(member.is_message_type, member.is_enum_type, member.type_name)
                ? "&::rapidproto::serde_support::Bytes(*rp_v)"
                : "rp_v";
        printer.print("$e$::$v$(rp_v) => rp_map.serialize_entry($n$, $val$)?,\n",
                      {{"e", enum_path}, {"v", variants[i]}, {"n", key}, {"val", val}});
    }
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n");
}

void emit_map_serialize(Printer& printer, const MapFieldNode& map, const std::string& accessor) {
    // A bytes-valued map must serialize its values opaquely (BytesMapView), not via the generic
    // MapView impl -- serde would otherwise render a `&[u8]` value as a seq of u8.
    const std::string val = is_scalar_bytes(map.value_is_message, map.value_is_enum, map.value_type)
                                ? "&::rapidproto::serde_support::BytesMapView(rp_v)"
                                : "&rp_v";
    printer.print("{\n");
    printer.indent();
    printer.print("let rp_v = self.$a$();\n", {{"a", accessor}});
    printer.print("if !rp_v.is_empty() {\n");
    printer.indent();
    printer.print("rp_map.serialize_entry($n$, $val$)?;\n",
                  {{"n", rust_str_lit(map.name)}, {"val", val}});
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n");
}

void emit_message_serialize(Printer& printer, const NameTable& table, const MessageNode& msg,
                            const FieldModes& modes, const AccessorNames& accessors,
                            const std::string& submodule) {
    printer.print("#[cfg(feature = \"serde\")]\n");
    printer.print("impl<'a> ::rapidproto::serde::Serialize for $M$<'a> {\n");
    printer.indent();
    printer.print(
        "fn serialize<RpS>(&self, rp_ser: RpS) -> ::core::result::Result<RpS::Ok, RpS::Error>\n");
    printer.print("where\n");
    printer.indent();
    printer.print("RpS: ::rapidproto::serde::Serializer,\n");
    printer.outdent();
    printer.print("{\n");
    printer.indent();
    printer.print("use ::rapidproto::serde::ser::SerializeMap as _;\n");
    printer.print("let mut rp_map = rp_ser.serialize_map(::core::option::Option::None)?;\n");
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        if (field_mode(modes, msg.fields[i]) != arenagen::FieldMode::Drop) {
            emit_field_serialize(printer, msg.fields[i], accessors.fields[i], modes);
        }
    }
    for (std::size_t i = 0; i < msg.map_fields.size(); ++i) {
        if (map_mode(modes, msg.map_fields[i]) != arenagen::FieldMode::Drop) {
            emit_map_serialize(printer, msg.map_fields[i], accessors.maps[i]);
        }
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        emit_oneof_serialize(printer, table, msg.oneofs[i], accessors.oneofs[i], submodule);
    }
    printer.print("rp_map.end()\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n\n");
}

// The owning-handle API (behind `alloc`): `$M$<'static>` is the zero-cost `ArenaDecode` tag (no
// separate marker type needed), and `decode_owned` is the self-contained-decode convenience over the
// runtime `::rapidproto::decode_owned` -- the Rust pendant of C++'s `decode_owned`. `$M$` is set by
// the caller.
void emit_message_owned(Printer& printer) {
    printer.print("#[cfg(feature = \"alloc\")]\n");
    printer.print("impl<'a> ::rapidproto::ArenaDecode<'a> for $M$<'static> {\n");
    printer.indent();
    printer.print("type Message = $M$<'a>;\n");
    printer.print(
        "fn decode_borrowed(rp_input: &'a [u8], rp_arena: &'a ::rapidproto::Arena<'a>) "
        "-> ::core::result::Result<&'a $M$<'a>, ::rapidproto::DecodeError> {\n");
    printer.indent();
    printer.print("$M$::decode(rp_input, rp_arena)\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n\n");

    printer.print("#[cfg(feature = \"alloc\")]\n");
    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    printer.print(
        "/// Decode `input` into a self-contained handle that owns the bytes and the arena; "
        "read\n");
    printer.print(
        "/// the message with `::rapidproto::Owned::get`. No external lifetime to manage.\n");
    printer.print(
        "pub fn decode_owned(rp_input: impl ::core::convert::Into<::rapidproto::Box<[u8]>>) "
        "-> ::core::result::Result<::rapidproto::Owned<$M$<'static>>, ::rapidproto::DecodeError> "
        "{\n");
    printer.indent();
    printer.print("::rapidproto::decode_owned::<$M$<'static>>(rp_input)\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n\n");
}

void emit_message(Printer& printer, const NameTable& table,
                  const std::vector<std::string>& current_mod, const MessageNode& msg,
                  const FieldModes& modes, const LayoutSet& layouts) {
    printer.set("M", table.messages.at(msg.fqn).type_name);
    const arenagen::MessageLayout* layout = layouts.find(msg.fqn);
    const AccessorNames accessors = message_accessor_names(msg, decode_reserved_idents(msg));
    const std::string& submodule = table.messages.at(msg.fqn).submodule;
    emit_message_struct(printer, table, current_mod, msg, modes, accessors, submodule, layout);
    // The `decode`/`decode_into` impl is emitted at the end of the arena module ([`emit_message_decode`])
    // so a reader skims the whole getter API without the wire-loop bodies in between.
    emit_message_accessors(printer, table, current_mod, msg, modes, accessors, submodule, layout);
    // The `serde::Serialize` impl (behind the `serde` feature) reads those same getters.
    emit_message_serialize(printer, table, msg, modes, accessors, submodule);
    // The owning-handle API (`decode_owned`, behind `alloc`).
    emit_message_owned(printer);
}

// Emit the `decode` + `decode_into` impl for one message, in its OWN module (`current_mod`) but after
// that module's structs/getters and submodules, so a reader skims the getters first and the wire-loop
// bodies sit at the bottom of each module. It must stay in the message's module: `decode_into`
// constructs the struct from its PRIVATE storage fields, which only the defining module may do.
void emit_message_decode(Printer& printer, const NameTable& table,
                         const std::vector<std::string>& current_mod, const MessageNode& msg,
                         const FieldModes& modes, const LayoutSet& layouts) {
    printer.set("M", table.messages.at(msg.fqn).type_name);
    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    const AccessorNames accessors = message_accessor_names(msg, decode_reserved_idents(msg));
    emit_decode(printer, table, current_mod, msg, accessors, modes, layouts.find(msg.fqn));
    printer.outdent();
    printer.print("}\n");
}

// Recursively emit arena message structs: each message as a struct+impl in `current_mod`, then -- in a
// `pub mod <snake>` one level deeper -- its oneof enums followed by its nested messages. The submodule
// name comes from the name table (deduped against sibling messages and struct/oneof-enum names), so it
// matches the paths referring in. The submodule is emitted when the message has nested messages OR
// oneofs (both live there).
void emit_arena_messages(Printer& printer, const NameTable& table,
                         const std::vector<const MessageNode*>& msgs,
                         std::vector<std::string>& current_mod, const FieldModes& modes,
                         const LayoutSet& layouts) {
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_message(printer, table, current_mod, *msgs[i], modes, layouts);
        const bool has_submodule = !msgs[i]->nested_messages.empty() || !msgs[i]->oneofs.empty();
        if (has_submodule) {
            const std::string& sub = table.messages.at(msgs[i]->fqn).submodule;
            printer.print("\npub mod $m$ {\n", {{"m", sub}});
            printer.indent();
            current_mod.push_back(sub);
            for (std::size_t k = 0; k < msgs[i]->oneofs.size(); ++k) {
                if (k != 0) {
                    printer.print("\n");
                }
                emit_oneof_enum(printer, table, current_mod, msgs[i]->oneofs[k]);
            }
            if (!msgs[i]->oneofs.empty() && !msgs[i]->nested_messages.empty()) {
                printer.print("\n");  // separate the oneof enums from the nested messages
            }
            emit_arena_messages(printer, table, addrs(msgs[i]->nested_messages), current_mod, modes,
                                layouts);
            current_mod.pop_back();
            printer.outdent();
            printer.print("}\n");
        }
    }
    // The decode impls for this module's messages, grouped at the bottom (after the structs/getters and
    // submodules above), so the getters read uninterrupted. Each stays in its message's own module.
    for (const MessageNode* msg : msgs) {
        printer.print("\n");
        emit_message_decode(printer, table, current_mod, *msg, modes, layouts);
    }
}

// Recursively emit nested enums into the common tree: a message holding enums gets a `pub mod <snake>`
// (the same table-assigned submodule name as the arena tree) with its own enums plus the recursion.
void emit_common_message_enums(Printer& printer, const NameTable& table,
                               const std::vector<const MessageNode*>& msgs) {
    for (const MessageNode* msg : msgs) {
        if (!codegen::holds_enum(*msg)) {
            continue;
        }
        printer.print("pub mod $m$ {\n", {{"m", table.messages.at(msg->fqn).submodule}});
        printer.indent();
        for (std::size_t i = 0; i < msg->enums.size(); ++i) {
            if (i != 0) {
                printer.print("\n");
            }
            emit_enum(printer, table, msg->enums[i]);
        }
        emit_common_message_enums(printer, table, addrs(msg->nested_messages));
        printer.outdent();
        printer.print("}\n");
    }
}

// The top-level messages / enums of every file at one package node, gathered into one scope.
std::vector<const MessageNode*> node_messages(const PkgTrie& node) {
    std::vector<const MessageNode*> msgs;
    for (const FileNode* file : node.files) {
        for (const MessageNode& msg : file->messages) {
            msgs.push_back(&msg);
        }
    }
    return msgs;
}
std::vector<const EnumNode*> node_enums(const PkgTrie& node) {
    std::vector<const EnumNode*> enums;
    for (const FileNode* file : node.files) {
        for (const EnumNode& node_enum : file->enums) {
            enums.push_back(&node_enum);
        }
    }
    return enums;
}

// Emit the `common` subtree for one package node: this package's enums (its files', plus the per-
// message nested-enum mirror), then a `pub mod <component>` per sub-package. `current_mod` tracks the
// emitted package path so blank-line separation matches the per-item rules.
void emit_common_node(Printer& printer, const NameTable& table, const PkgTrie& node) {
    const std::vector<const EnumNode*> enums = node_enums(node);
    for (std::size_t i = 0; i < enums.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_enum(printer, table, *enums[i]);
    }
    emit_common_message_enums(printer, table, node_messages(node));
    for (const auto& [component, child] : node.children) {
        printer.print("pub mod $m$ {\n", {{"m", child.emitted}});
        printer.indent();
        emit_common_node(printer, table, child);
        printer.outdent();
        printer.print("}\n");
    }
}

// Emit the `arena` subtree for one package node: this package's message structs, then a
// `pub mod <component>` per sub-package. `current_mod` is the node's emitted package path.
void emit_arena_node(Printer& printer, const NameTable& table, const PkgTrie& node,
                     std::vector<std::string>& current_mod, const FieldModes& modes,
                     const LayoutSet& layouts) {
    emit_arena_messages(printer, table, node_messages(node), current_mod, modes, layouts);
    for (const auto& [component, child] : node.children) {
        printer.print("\npub mod $m$ {\n", {{"m", child.emitted}});
        printer.indent();
        current_mod.push_back(child.emitted);
        emit_arena_node(printer, table, child, current_mod, modes, layouts);
        current_mod.pop_back();
        printer.outdent();
        printer.print("}\n");
    }
}

}  // namespace

std::string generate_common(const std::vector<const FileNode*>& files) {
    const PkgTrie root = [&] {
        PkgTrie trie = build_pkg_trie(files);
        assign_emitted_names(trie);
        return trie;
    }();
    const NameTable table = build_name_table(root);
    Printer printer;
    printer.print("pub mod common {\n");
    printer.indent();
    emit_common_node(printer, table, root);
    printer.outdent();
    printer.print("}\n");
    return printer.str();
}

std::vector<std::pair<std::string, std::string>> arena_type_paths(
    const std::vector<const FileNode*>& files) {
    // Rebuilds the name table independently of generate_rust (a caller may want the paths alone, and
    // naming is modes-independent, so this stays a standalone entry point). The rebuild is
    // generation-time and O(small); the type path mirrors emission -- `arena` + the module segments
    // + the type name -- so the two must move together.
    PkgTrie root = build_pkg_trie(files);
    assign_emitted_names(root);
    const NameTable table = build_name_table(root);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(table.messages.size());
    for (const auto& [fqn, names] : table.messages) {
        std::string path = "arena";
        for (const std::string& seg : names.module) {
            path += "::";
            path += seg;
        }
        path += "::";
        path += names.type_name;
        out.emplace_back(fqn, path);
    }
    return out;
}

std::string generate_arena(const std::vector<const FileNode*>& files, const LayoutSet& layouts,
                           const FieldModes& modes) {
    PkgTrie root = build_pkg_trie(files);
    assign_emitted_names(root);
    const NameTable table = build_name_table(root);
    Printer printer;
    printer.print("pub mod arena {\n");
    printer.indent();
    std::vector<std::string> current_mod;
    emit_arena_node(printer, table, root, current_mod, modes, layouts);
    printer.outdent();
    printer.print("}\n");
    return printer.str();
}

std::string generate_file(const std::vector<const FileNode*>& files, const LayoutSet& layouts,
                          const FieldModes& modes) {
    // A header comment records the active field-modes profile (its id + canonical drop/raw/unknown
    // lines) for traceability. Rust needs no ODR-style identity guard: a generated file is included
    // once as a self-contained module tree, so two differently-profiled generations cannot silently
    // merge (including both into one scope is a duplicate-module compile error).
    std::string banner = "// @generated by rapidprotoc --rust -- do not edit.\n";
    if (modes.active()) {
        banner += "// field-modes profile " + modes.profile_id + ":\n";
        for (const std::string& line : modes.normalized) {
            banner += "//   " + line + "\n";
        }
    }
    return banner + "\n" + generate_common(files) + "\n" + generate_arena(files, layouts, modes);
}

}  // namespace rapidproto::rustgen
