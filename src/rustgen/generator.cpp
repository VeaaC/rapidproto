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
    // and an enum newtype in this common scope cannot collide), then each newtype and its Kind.
    {
        std::unordered_set<std::string> taken = submods;
        for (const EnumNode* node : enums) {
            EnumNames names;
            names.module = module;
            names.type_name = codegen::dedup_suffix(taken, escape_rust_ident(node->name));
            names.kind_name = codegen::dedup_suffix(taken, names.type_name + "Kind");
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

// --- scalar-only message decode (M1.4 slice 1) ---------------------------------------------------
// A message whose decode body this slice emits fully: no maps, no oneofs, and every field a SINGULAR
// scalar / enum / string / bytes with implicit or explicit presence and no proto2 custom default.
// Messages with repeated / message / map / oneof fields, `required` fields, or custom defaults keep
// the skeleton (todo!() getters + todo!() decode) until later slices, so every golden still compiles.
// Collect every message (top-level and nested) across the generation, keyed by FQN.
void collect_messages(const std::vector<const MessageNode*>& msgs,
                      std::unordered_map<std::string, const MessageNode*>& out) {
    for (const MessageNode* msg : msgs) {
        out.emplace(msg->fqn, msg);
        collect_messages(addrs(msg->nested_messages), out);
    }
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

// Whether a message is a decode candidate on its OWN shape (ignoring what it references): no delimited
// (group) message field it must MATERIALIZE -- in a regular field or a oneof member. A group arrives as
// SGROUP/EGROUP (wire type 3/4), not a length prefix, so a length-prefixed decode arm would silently
// drop it -- it stays a skeleton until a group slice. A `drop` field is skip-validated (never blocks,
// and can even make a group-bearing message decodable); a `raw` field stores its payload bytes without
// decoding (never blocks) -- except a `raw` GROUP field, which would need group-span capture (deferred)
// and so still blocks. (Maps, oneofs, proto2 `required` and proto2 custom defaults are handled.)
bool self_decodable(const MessageNode& msg, const FieldModes& modes) {
    const auto field_ok = [&](const FieldNode& field) {
        switch (field_mode(modes, field)) {
            case arenagen::FieldMode::Drop:
                return true;
            case arenagen::FieldMode::Raw:
                return field.message_encoding != MessageEncoding::Delimited;
            case arenagen::FieldMode::Materialize:
                break;
        }
        return !field.is_message_type || field.message_encoding != MessageEncoding::Delimited;
    };
    if (!std::all_of(msg.fields.begin(), msg.fields.end(), field_ok)) {
        return false;
    }
    return std::all_of(msg.oneofs.begin(), msg.oneofs.end(), [&](const OneofNode& oneof) {
        return std::all_of(oneof.fields.begin(), oneof.fields.end(), field_ok);
    });
}

// The FQNs of messages to emit a full decoder for: self-decodable AND every message they reference
// (via a singular or repeated message field) is itself decodable -- otherwise the emitted
// `decode_into` would call one that was never emitted. A fixpoint propagates non-decodability up the
// reference graph; a cycle of otherwise-decodable messages stays decodable (the runtime depth cap
// bounds the recursion). Messages not in the set keep the skeleton (todo!()) for a later slice.
std::unordered_set<std::string> compute_decodable(const std::vector<const FileNode*>& files,
                                                  const FieldModes& modes) {
    std::unordered_map<std::string, const MessageNode*> all;
    for (const FileNode* file : files) {
        collect_messages(addrs(file->messages), all);
    }
    std::unordered_set<std::string> decodable;
    for (const auto& [fqn, msg] : all) {
        if (self_decodable(*msg, modes)) {
            decodable.insert(fqn);
        }
    }
    // A message references a skeleton (non-decodable) message -- via a regular field, a oneof member,
    // or a map value -- if so it cannot itself be decoded (its decode_into would call a worker that was
    // never emitted), so it drops out of the set. All three member collections are scanned. A `raw` or
    // `drop` message field is NOT a reference (it is stored as bytes / skipped, never recursed into), so
    // it does not pull its type's decodability in -- `raw` can thus rescue a message that only referenced
    // a skeleton.
    const auto field_refs_skeleton = [&](const FieldNode& field) {
        return field.is_message_type &&
               field_mode(modes, field) == arenagen::FieldMode::Materialize &&
               decodable.count(field.resolved_type_fqn) == 0;
    };
    const auto refs_skeleton = [&](const MessageNode& msg) {
        if (std::any_of(msg.fields.begin(), msg.fields.end(), field_refs_skeleton)) {
            return true;
        }
        if (std::any_of(msg.map_fields.begin(), msg.map_fields.end(), [&](const MapFieldNode& map) {
                return map_mode(modes, map) != arenagen::FieldMode::Drop && map.value_is_message &&
                       decodable.count(map.resolved_value_type_fqn) == 0;
            })) {
            return true;
        }
        return std::any_of(msg.oneofs.begin(), msg.oneofs.end(), [&](const OneofNode& oneof) {
            return std::any_of(oneof.fields.begin(), oneof.fields.end(), field_refs_skeleton);
        });
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = decodable.begin(); it != decodable.end();) {
            const MessageNode& msg = *all.at(*it);
            if (refs_skeleton(msg)) {
                it = decodable.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
    return decodable;
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

// The struct storage type (= the getter return type) for a field: a repeated field is a borrowed
// slice; a singular message is `Option<&'a T<'a>>`; an explicit-presence scalar wraps in `Option`;
// everything else is the bare value type. A proto2 `[default = ...]` is NOT materialized (matching the
// C++ decoder, which reads an absent defaulted field as `None` and leaves the default to the consumer),
// so a defaulted field is stored exactly like any other field of its presence. A `raw` message field
// keeps its undecoded LEN payload instead of the decoded tree: `Option<&'a [u8]>` singular,
// `&'a [&'a [u8]]` repeated (the consumer feeds a payload to the field type's `decode` itself).
std::string field_storage_type(const NameTable& table, const std::vector<std::string>& current_mod,
                               const FieldNode& field, const FieldModes& modes) {
    if (field_mode(modes, field) == arenagen::FieldMode::Raw) {
        return field.is_repeated ? "&'a [&'a [u8]]" : "Option<&'a [u8]>";
    }
    std::string value = field_value_type(table, current_mod, field);
    if (field.is_repeated) {
        return "&'a [" + value + "]";
    }
    if (field.is_message_type) {
        return "Option<&'a " + value + ">";
    }
    if (field.presence == FieldPresence::Explicit) {
        return "Option<" + value + ">";
    }
    return value;
}

// The default initializer for a NON-repeated field's storage local: `None` for a singular message or
// an explicit-presence scalar, else the proto3 zero value. A proto2 `[default = ...]` is not
// materialized (matching the C++ decoder -- an absent defaulted field reads `None`, the consumer
// applies the default), so it initializes exactly like any other field of its presence. (Repeated
// fields use a `DecodeVec`, not this.)
std::string field_default_expr(const NameTable& table, const std::vector<std::string>& current_mod,
                               const FieldNode& field) {
    if (field.is_message_type || field.presence == FieldPresence::Explicit) {
        return "None";
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

// Emit a `pub fn <field>(&self) -> ...` accessor. `real` emits `self.<field>` (reading the storage);
// otherwise `todo!()` (a skeleton message whose decode is not yet emitted). A singular message is
// `Option<&'a T<'a>>`; a repeated field is a borrowed slice; explicit presence wraps in `Option`.
void emit_field_accessor(Printer& printer, const NameTable& table,
                         const std::vector<std::string>& current_mod, const FieldNode& field,
                         const std::string& accessor, bool real, const FieldModes& modes) {
    // The return type is exactly the storage type: a getter hands back a copy of the stored value (a
    // repeated slice, a singular-message `Option<&T>`, an explicit-presence `Option`, a `raw` payload
    // view, or a bare value).
    const std::string ret = field_storage_type(table, current_mod, field, modes);
    printer.print("pub fn $a$(&self) -> $t$ {\n", {{"a", accessor}, {"t", ret}});
    printer.indent();
    if (real) {
        printer.print("self.$a$\n", {{"a", accessor}});
    } else {
        printer.print("todo!()\n");
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

// Emit a `pub fn <map>(&self) -> MapView<'a, K, V>` accessor. `real` reads the stored view; otherwise
// `todo!()` (a skeleton message). `accessor` is the pre-deduped name.
void emit_map_accessor(Printer& printer, const NameTable& table,
                       const std::vector<std::string>& current_mod, const MapFieldNode& map,
                       const std::string& accessor, bool real) {
    printer.print("pub fn $a$(&self) -> $t$ {\n",
                  {{"a", accessor}, {"t", map_view_type(table, current_mod, map)}});
    printer.indent();
    printer.print(real ? "self.$a$\n" : "todo!()\n", {{"a", accessor}});
    printer.outdent();
    printer.print("}\n");
}

// Emit a `pub fn <oneof>(&self) -> Option<&<submodule>::Payload['a]>` accessor (None = unset). `real`
// borrows the stored enum (`as_ref`); otherwise `todo!()`. The getter hands back a reference -- the
// oneof is not `Copy` (see `emit_oneof_enum`) -- so matching binds the variant payloads by reference.
// The oneof enum lives in this message's own submodule. `accessor` is the pre-deduped method name.
void emit_oneof_accessor(Printer& printer, const NameTable& table, const OneofNode& oneof,
                         const std::string& accessor, bool real, const std::string& submodule) {
    const std::string ret = "Option<&" + submodule + "::" + table.oneofs.at(&oneof) +
                            (oneof_borrows(oneof) ? "<'a>" : "") + ">";
    printer.print("pub fn $a$(&self) -> $t$ {\n", {{"a", accessor}, {"t", ret}});
    printer.indent();
    printer.print(real ? "self.$a$.as_ref()\n" : "todo!()\n", {{"a", accessor}});
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

// Advance `rp_rest` past the current field (whatever its wire type), erroring body-relative.
void emit_skip_body(Printer& printer) {
    printer.print("rp_rest = match ::rapidproto::wire::skip(rp_rest, rp_tag) {\n");
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", kLoopOff}});
    printer.outdent();
    printer.print("};\n");
}

// A known-field skip arm `($n$u32, _) => skip`: the field number matched for ANY wire type and
// skip-validated, so it never falls to the `_` catch-all and so never trips the unknown-fields bit.
// Used for a `drop` field/map, and (in an unknown-tracked message) as the trailing fall-through after a
// kept field's typed arm(s), so a KNOWN field arriving with the wrong wire type is still "known", not
// "unknown" -- matching the C++ decoder, which switches on field number and never flags a known number.
void emit_known_skip_arm(Printer& printer, const std::string& nstr) {
    printer.print("($n$u32, _) => {\n", {{"n", nstr}});
    printer.indent();
    emit_skip_body(printer);
    printer.outdent();
    printer.print("}\n");
}

// Emit the packed (length-delimited run) arm for a repeated numeric field, pre-sizing the
// accumulator from the span before the fill -- the Rust mirror of the C++ decoder's standalone
// `emit_packed_fill` (src/arenagen/generator.cpp). Varint elements reserve the span's byte length as
// an element upper bound (each varint is >= 1 byte), fill, then trim the over-estimate back to the
// exact count (`shrink_to_fit`). Fixed-width elements reserve the exact count (span / width); on a
// little-endian host a whole-multiple span fills in one bulk copy (`extend_from_le_bytes`, the span
// IS the native byte image), else a per-element read reports an exact truncation error.
void emit_packed_arm(Printer& printer, const NameTable& table,
                     const std::vector<std::string>& current_mod, const FieldNode& field) {
    const std::string nstr = std::to_string(field.number);
    const std::string acc = "rp_acc_" + nstr;
    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
    printer.indent();
    emit_len_read(printer, loop_read());
    if (wire == "Varint") {
        emit_reserve(printer, acc, "rp_p.len()");
        printer.print("while !rp_p.is_empty() {\n");
        printer.indent();
        emit_scalar_value_read(printer, field, packed_read());
        emit_push(printer, acc, conv);
        printer.outdent();
        printer.print("}\n");
        printer.print("$acc$.shrink_to_fit();\n", {{"acc", acc}});
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
    printer.outdent();
    printer.print("}\n");
}

// Emit the `match (field, wire)` arm(s) for one field: one arm for a singular value, two for a
// repeated numeric (unpacked + packed), reading via the value-threaded wire primitives and storing
// into the field's local (`accessor`) or pushing into its accumulator (`rp_acc_<n>`). A singular
// message is recursed into with `decode_into` at depth+1 and arena-allocated; a repeated message's
// element is pushed by value. A singular message seen twice is rejected (matching C++).
void emit_field_arms(Printer& printer, const NameTable& table,
                     const std::vector<std::string>& current_mod, const FieldNode& field,
                     const std::string& accessor, bool required, const FieldModes& modes) {
    const std::string nstr = std::to_string(field.number);
    const std::string acc = "rp_acc_" + nstr;

    const arenagen::FieldMode mode = field_mode(modes, field);
    if (mode == arenagen::FieldMode::Drop) {
        emit_known_skip_arm(printer, nstr);
        return;
    }
    if (mode == arenagen::FieldMode::Raw) {
        // A message field kept as its undecoded LEN payload bytes (no recursion). Singular rejects a
        // second occurrence like a materialized singular message; repeated pushes each payload.
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        if (field.is_repeated) {
            emit_len_read(printer, loop_read());
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
            emit_len_read(printer, loop_read());
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
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        printer.print("if $a$.is_some() {\n", {{"a", accessor}});
        printer.indent();
        printer.print(
            "return Err(::rapidproto::DecodeError::new("
            "::rapidproto::ErrorKind::RepeatedSingularMessage, $off$).with_field($n$));\n",
            {{"off", kLoopOff}, {"n", nstr}});
        printer.outdent();
        printer.print("}\n");
        emit_len_read(printer, loop_read());
        printer.print("let rp_sub = $p$::decode_into(rp_p, rp_arena, rp_depth + 1)?;\n",
                      {{"p", path}});
        printer.print("$a$ = Some(match rp_arena.alloc(rp_sub) {\n", {{"a", accessor}});
        printer.indent();
        printer.print("Ok(r) => &*r,\n");
        printer.print(
            "Err(_) => return "
            "Err(::rapidproto::DecodeError::new(::rapidproto::ErrorKind::ArenaExhausted, 0)),\n");
        printer.outdent();
        printer.print("});\n");
        if (required) {
            printer.print("rp_req_$n$ = true;\n", {{"n", nstr}});
        }
        printer.outdent();
        printer.print("}\n");
        return;
    }
    if (field.is_message_type && field.is_repeated) {
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        emit_len_read(printer, loop_read());
        printer.print("let rp_sub = $p$::decode_into(rp_p, rp_arena, rp_depth + 1)?;\n",
                      {{"p", path}});
        emit_push(printer, acc, "rp_sub");
        printer.outdent();
        printer.print("}\n");
        return;
    }

    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    if (field.is_repeated && wire != "Len") {
        // Repeated numeric: an unpacked arm (one value per occurrence) and a packed arm (a
        // length-delimited run of values). proto permits mixing both on the wire.
        printer.print("($n$u32, ::rapidproto::wire::WireType::$w$) => {\n",
                      {{"n", nstr}, {"w", wire}});
        printer.indent();
        emit_scalar_value_read(printer, field, loop_read());
        emit_push(printer, acc, conv);
        printer.outdent();
        printer.print("}\n");
        emit_packed_arm(printer, table, current_mod, field);
        return;
    }
    if (field.is_repeated) {  // repeated string/bytes (wire == Len, not packable)
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        emit_len_read(printer, loop_read());
        emit_push(printer, acc, conv);
        printer.outdent();
        printer.print("}\n");
        return;
    }
    // Singular scalar / enum / string / bytes.
    printer.print("($n$u32, ::rapidproto::wire::WireType::$w$) => {\n", {{"n", nstr}, {"w", wire}});
    printer.indent();
    if (wire == "Len") {
        emit_len_read(printer, loop_read());
    } else {
        emit_scalar_value_read(printer, field, loop_read());
    }
    printer.print(field.presence == FieldPresence::Explicit ? "$a$ = Some($c$);\n" : "$a$ = $c$;\n",
                  {{"a", accessor}, {"c", conv}});
    if (required) {
        printer.print("rp_req_$n$ = true;\n", {{"n", nstr}});
    }
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
void emit_oneof_arm(Printer& printer, const NameTable& table,
                    const std::vector<std::string>& current_mod, const FieldNode& field,
                    const std::string& oneof_local, const std::string& enum_path,
                    const std::string& variant) {
    const std::string nstr = std::to_string(field.number);
    if (field.is_message_type) {
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        printer.print("if matches!($l$, Some($e$::$v$(_))) {\n",
                      {{"l", oneof_local}, {"e", enum_path}, {"v", variant}});
        printer.indent();
        printer.print(
            "return Err(::rapidproto::DecodeError::new("
            "::rapidproto::ErrorKind::RepeatedSingularMessage, $off$).with_field($n$));\n",
            {{"off", kLoopOff}, {"n", nstr}});
        printer.outdent();
        printer.print("}\n");
        emit_len_read(printer, loop_read());
        printer.print("let rp_sub = $p$::decode_into(rp_p, rp_arena, rp_depth + 1)?;\n",
                      {{"p", path}});
        printer.print("let rp_ptr = match rp_arena.alloc(rp_sub) {\n");
        printer.indent();
        printer.print("Ok(r) => &*r,\n");
        printer.print(
            "Err(_) => return "
            "Err(::rapidproto::DecodeError::new(::rapidproto::ErrorKind::ArenaExhausted, 0)),\n");
        printer.outdent();
        printer.print("};\n");
        printer.print("$l$ = Some($e$::$v$(rp_ptr));\n",
                      {{"l", oneof_local}, {"e", enum_path}, {"v", variant}});
        printer.outdent();
        printer.print("}\n");
        return;
    }
    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    printer.print("($n$u32, ::rapidproto::wire::WireType::$w$) => {\n", {{"n", nstr}, {"w", wire}});
    printer.indent();
    if (wire == "Len") {
        emit_len_read(printer, loop_read());
    } else {
        emit_scalar_value_read(printer, field, loop_read());
    }
    printer.print("$l$ = Some($e$::$v$($c$));\n",
                  {{"l", oneof_local}, {"e", enum_path}, {"v", variant}, {"c", conv}});
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
    const std::string nstr = std::to_string(field.number);
    if (field.is_message_type) {  // the value field; held in an `Option` (None = not yet seen)
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
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
        printer.print("$l$ = Some($p$::decode_into(rp_p, rp_arena, rp_depth + 1)?);\n",
                      {{"l", local}, {"p", path}});
        printer.outdent();
        printer.print("}\n");
        return;
    }
    const std::string wire = scalar_wire_variant(field);
    const std::string conv = scalar_convert_expr(table, current_mod, field);
    printer.print("($n$u32, ::rapidproto::wire::WireType::$w$) => {\n", {{"n", nstr}, {"w", wire}});
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

// Emit the match arm for one map field: read the length-delimited entry, decode its key/value in an
// inner loop (unknown entry fields skipped), and push `(key, value)` into the map's `DecodeVec`. The
// entry is its own decode frame -- offsets inside it are entry-relative (matching C++'s map re-anchor).
void emit_map_arm(Printer& printer, const NameTable& table,
                  const std::vector<std::string>& current_mod, const MapFieldNode& map,
                  const std::string& map_local) {
    const std::string nstr = std::to_string(map.number);
    const FieldNode key = map_key_field(map);
    const FieldNode val = map_value_field(map);
    const ReadAt entry_at{"rp_ent", "rp_ent_body.len() - rp_ent.len()"};
    printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
    printer.indent();
    emit_len_read(printer, loop_read());  // the entry payload -> rp_p
    printer.print("let rp_ent_body: &'a [u8] = rp_p;\n");
    printer.print("let mut rp_key: $t$ = $d$;\n",
                  {{"t", field_value_type(table, current_mod, key)},
                   {"d", field_default_expr(table, current_mod, key)}});
    // A message value is stored by value (no `Option`), so it is accumulated in an `Option` here --
    // `None` until seen -- and filled with the default (empty) message if the entry omits field 2.
    if (val.is_message_type) {
        printer.print("let mut rp_val: Option<$t$> = None;\n",
                      {{"t", field_value_type(table, current_mod, val)}});
    } else {
        printer.print("let mut rp_val: $t$ = $d$;\n",
                      {{"t", field_value_type(table, current_mod, val)},
                       {"d", field_default_expr(table, current_mod, val)}});
    }
    printer.print("let mut rp_ent: &'a [u8] = rp_ent_body;\n");
    printer.print("loop {\n");
    printer.indent();
    printer.print(
        "let mut rp_etag = ::rapidproto::wire::Tag { field: 0, wire: "
        "::rapidproto::wire::WireType::Varint };\n");
    printer.print("rp_ent = match ::rapidproto::wire::read_tag_or_end(rp_ent, &mut rp_etag) {\n");
    printer.indent();
    printer.print("Ok(Some(r)) => r,\n");
    printer.print("Ok(None) => break,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $o$)),\n",
                  {{"o", entry_at.off}});
    printer.outdent();
    printer.print("};\n");
    printer.print("match (rp_etag.field, rp_etag.wire) {\n");
    printer.indent();
    emit_map_component(printer, table, current_mod, key, "rp_key", entry_at, map.number);
    emit_map_component(printer, table, current_mod, val, "rp_val", entry_at, map.number);
    printer.print("_ => {\n");
    printer.indent();
    printer.print("rp_ent = match ::rapidproto::wire::skip(rp_ent, rp_etag) {\n");
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $o$)),\n",
                  {{"o", entry_at.off}});
    printer.outdent();
    printer.print("};\n");
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
        printer.print("None => $p$::decode_into(&[], rp_arena, rp_depth + 1)?,\n", {{"p", path}});
        printer.outdent();
        printer.print("};\n");
    }
    emit_push(printer, map_local, "(rp_key, rp_val)");
    printer.outdent();
    printer.print("}\n");
}

// Emit `decode()` + the `decode_into()` worker for a decodable message: a field loop over `rp_body`
// driving the fused `read_tag_or_end`, matching `(field, wire)` to read each known regular / oneof-
// member / map field (a wrong wire type for a known field falls to `skip`, like an unknown field),
// accumulating repeated fields and maps in `DecodeVec`s and recursing into sub-messages, then
// constructing the message (checking proto2 `required` fields were all seen). `decode_into` is
// `pub(crate)` so sub-messages in other modules can recurse into it.
// The decode locals: a repeated field / map accumulates into a `DecodeVec`; a oneof holds its enum
// (None until a member is seen); a proto2 `required` field a transient seen-flag; `unknown-fields` a
// bool; the rest plain values at their proto3 zero / None default. A dropped field/map has no local
// (its arm skip-validates and discards). `submodule` names the message's own oneof-enum submodule.
void emit_decode_locals(Printer& printer, const NameTable& table,
                        const std::vector<std::string>& current_mod, const MessageNode& msg,
                        const FieldModes& modes, const AccessorNames& accessors,
                        const std::string& submodule) {
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        const FieldNode& field = msg.fields[i];
        if (field_mode(modes, field) == arenagen::FieldMode::Drop) {
            continue;
        }
        if (field.is_repeated) {
            printer.print("let mut rp_acc_$n$ = ::rapidproto::DecodeVec::new(rp_arena);\n",
                          {{"n", std::to_string(field.number)}});
        } else {
            printer.print("let mut $l$: $t$ = $d$;\n",
                          {{"l", accessors.fields[i]},
                           {"t", field_storage_type(table, current_mod, field, modes)},
                           {"d", field_default_expr(table, current_mod, field)}});
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

// The `match (field, wire)` arm for every regular / map / oneof-member field, then a catch-all that
// skips genuinely-unknown fields -- tripping the unknown-fields bit when the profile reserves it.
// Dropped fields/maps get a known skip arm (via emit_field_arms / emit_known_skip_arm) so they never
// reach the catch-all. For an unknown-tracked message, every KEPT known field also gets a trailing
// `(n, _)` skip arm (after its typed arm(s)) so a known field arriving with the WRONG wire type is
// skipped as known, not counted unknown -- matching C++ (which flags only unrecognized field numbers).
void emit_decode_arms(Printer& printer, const NameTable& table,
                      const std::vector<std::string>& current_mod, const MessageNode& msg,
                      const FieldModes& modes, const AccessorNames& accessors,
                      const std::string& submodule) {
    const bool wants_unknown = modes.wants_unknown(&msg);
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        emit_field_arms(printer, table, current_mod, msg.fields[i], accessors.fields[i],
                        msg.fields[i].presence == FieldPresence::Required, modes);
        if (wants_unknown && field_mode(modes, msg.fields[i]) != arenagen::FieldMode::Drop) {
            emit_known_skip_arm(printer, std::to_string(msg.fields[i].number));
        }
    }
    for (const MapFieldNode& map : msg.map_fields) {
        if (map_mode(modes, map) == arenagen::FieldMode::Drop) {
            emit_known_skip_arm(printer, std::to_string(map.number));
        } else {
            emit_map_arm(printer, table, current_mod, map, "rp_map_" + std::to_string(map.number));
            if (wants_unknown) {
                emit_known_skip_arm(printer, std::to_string(map.number));
            }
        }
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        const OneofNode& oneof = msg.oneofs[i];
        const std::string enum_path = submodule + "::" + table.oneofs.at(&oneof);
        const std::vector<std::string> variants = oneof_variant_names(oneof);
        const std::string local = "rp_oneof_" + std::to_string(i);
        for (std::size_t j = 0; j < oneof.fields.size(); ++j) {
            emit_oneof_arm(printer, table, current_mod, oneof.fields[j], local, enum_path,
                           variants[j]);
            if (wants_unknown) {
                emit_known_skip_arm(printer, std::to_string(oneof.fields[j].number));
            }
        }
    }
    printer.print("_ => {\n");
    printer.indent();
    if (wants_unknown) {
        printer.print("rp_unknown = true;\n");  // a genuinely unknown field number was seen
    }
    emit_skip_body(printer);
    printer.outdent();
    printer.print("}\n");
}

// The `Ok(Msg { .. })` construct from the decode locals; a dropped field/map contributes no storage,
// `unknown-fields` the bit, and a message with no `'a`-using field the `PhantomData` marker.
void emit_decode_construct(Printer& printer, const MessageNode& msg, const FieldModes& modes,
                           const AccessorNames& accessors) {
    printer.print("Ok($M$ {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        if (field_mode(modes, msg.fields[i]) == arenagen::FieldMode::Drop) {
            continue;
        }
        if (msg.fields[i].is_repeated) {
            printer.print(
                "$l$: rp_acc_$n$.into_slice(),\n",
                {{"l", accessors.fields[i]}, {"n", std::to_string(msg.fields[i].number)}});
        } else {
            printer.print("$l$,\n", {{"l", accessors.fields[i]}});
        }
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
    printer.print("})\n");
}

void emit_decode(Printer& printer, const NameTable& table,
                 const std::vector<std::string>& current_mod, const MessageNode& msg,
                 const AccessorNames& accessors, const FieldModes& modes) {
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
    printer.print("let rp_built = Self::decode_into(input, arena, 0)?;\n");
    printer.print("match arena.alloc(rp_built) {\n");
    printer.indent();
    printer.print("Ok(r) => Ok(&*r),\n");
    printer.print(
        "Err(_) => Err(::rapidproto::DecodeError::new(::rapidproto::ErrorKind::ArenaExhausted, "
        "0)),\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n\n");

    // `rp_arena` is unused when the body neither accumulates a repeated field nor allocates a
    // sub-message (a message of only singular scalar/enum/string/bytes fields); name it `_rp_arena`
    // then to avoid an unused-variable warning.
    printer.print("#[doc(hidden)]\n");
    printer.print(
        "pub(crate) fn decode_into(rp_body: &'a [u8], $ap$: &'a ::rapidproto::Arena<'a>, rp_depth: "
        "u32) -> Result<$M$<'a>, ::rapidproto::DecodeError> {\n",
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
    emit_decode_locals(printer, table, current_mod, msg, modes, accessors, submodule);
    printer.print("let mut rp_rest: &'a [u8] = rp_body;\n");
    printer.print("loop {\n");
    printer.indent();
    printer.print(
        "let mut rp_tag = ::rapidproto::wire::Tag { field: 0, wire: "
        "::rapidproto::wire::WireType::Varint };\n");
    printer.print("rp_rest = match ::rapidproto::wire::read_tag_or_end(rp_rest, &mut rp_tag) {\n");
    printer.indent();
    printer.print("Ok(Some(r)) => r,\n");
    printer.print("Ok(None) => break,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", kLoopOff}});
    printer.outdent();
    printer.print("};\n");
    printer.print("match (rp_tag.field, rp_tag.wire) {\n");
    printer.indent();
    emit_decode_arms(printer, table, current_mod, msg, modes, accessors, submodule);
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
    emit_decode_construct(printer, msg, modes, accessors);
    printer.outdent();
    printer.print("}\n");
}

// The decode body's internal identifiers (fixed locals + `_marker`, plus each repeated field / map
// accumulator, oneof slot and required seen-flag), reserved so no field accessor -- which is also the
// struct storage field name -- collides with a decode local. Empty for a skeleton message (no body).
std::unordered_set<std::string> decode_reserved_idents(const MessageNode& msg, bool supported) {
    if (!supported) {
        return {};
    }
    std::unordered_set<std::string> reserved =
        {
            "decode_into", "_marker",     "rp_body",   "rp_arena", "_rp_arena",
            "rp_depth",    "rp_rest",     "rp_tag",    "rp_v",     "rp_p",
            "rp_sub",      "rp_ptr",      "rp_etag",   "rp_ent",   "rp_key",
            "rp_val",      "rp_ent_body", "rp_unknown"};  // the has_unknown_fields() storage field + its decode flag
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

// Emit `pub struct Msg<'a> { .. } impl<'a> Msg<'a> { accessors; decode }`. `current_mod` is the module
// the struct is emitted in, used to compute cross-references. A decodable message
// ([`compute_decodable`]) gets real field storage, reading accessors and a real decode; any other
// message keeps the skeleton (`PhantomData`, `todo!()` accessors + decode) for a later slice.
// Emit `pub struct Msg<'a> { .. }` -- the storage fields (= getters' return types). A dropped field/map
// has no storage; a `raw` message field stores its payload bytes; `unknown-fields` adds the bit.
void emit_message_struct(Printer& printer, const NameTable& table,
                         const std::vector<std::string>& current_mod, const MessageNode& msg,
                         const FieldModes& modes, const AccessorNames& accessors,
                         const std::string& submodule, bool supported) {
    printer.print("pub struct $M$<'a> {\n");
    printer.indent();
    if (!supported) {
        printer.print("_marker: ::core::marker::PhantomData<&'a ()>,\n");
        printer.outdent();
        printer.print("}\n\n");
        return;
    }
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        if (field_mode(modes, msg.fields[i]) != arenagen::FieldMode::Drop) {
            printer.print("$l$: $t$,\n",
                          {{"l", accessors.fields[i]},
                           {"t", field_storage_type(table, current_mod, msg.fields[i], modes)}});
        }
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
                            const std::string& submodule, bool supported) {
    if (!message_has_accessors(msg, modes)) {
        return;
    }
    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        if (field_mode(modes, msg.fields[i]) != arenagen::FieldMode::Drop) {
            emit_field_accessor(printer, table, current_mod, msg.fields[i], accessors.fields[i],
                                supported, modes);
        }
    }
    for (std::size_t i = 0; i < msg.map_fields.size(); ++i) {
        if (map_mode(modes, msg.map_fields[i]) != arenagen::FieldMode::Drop) {
            emit_map_accessor(printer, table, current_mod, msg.map_fields[i], accessors.maps[i],
                              supported);
        }
    }
    for (std::size_t i = 0; i < msg.oneofs.size(); ++i) {
        emit_oneof_accessor(printer, table, msg.oneofs[i], accessors.oneofs[i], supported,
                            submodule);
    }
    if (modes.wants_unknown(&msg)) {
        printer.print("pub fn has_unknown_fields(&self) -> bool {\n");
        printer.indent();
        printer.print(supported ? "self.rp_unknown\n" : "todo!()\n");
        printer.outdent();
        printer.print("}\n");
    }
    printer.outdent();
    printer.print("}\n");
}

void emit_message(Printer& printer, const NameTable& table,
                  const std::vector<std::string>& current_mod, const MessageNode& msg,
                  const std::unordered_set<std::string>& decodable, const FieldModes& modes) {
    printer.set("M", table.messages.at(msg.fqn).type_name);
    const bool supported = decodable.count(msg.fqn) != 0;
    const AccessorNames accessors =
        message_accessor_names(msg, decode_reserved_idents(msg, supported));
    const std::string& submodule = table.messages.at(msg.fqn).submodule;
    emit_message_struct(printer, table, current_mod, msg, modes, accessors, submodule, supported);
    // The `decode`/`decode_into` impl is emitted at the end of the arena module ([`emit_message_decode`])
    // so a reader skims the whole getter API without the wire-loop bodies in between.
    emit_message_accessors(printer, table, current_mod, msg, modes, accessors, submodule,
                           supported);
}

// Emit the `decode` + `decode_into` impl for one message, in its OWN module (`current_mod`) but after
// that module's structs/getters and submodules, so a reader skims the getters first and the wire-loop
// bodies sit at the bottom of each module. It must stay in the message's module: `decode_into`
// constructs the struct from its PRIVATE storage fields, which only the defining module may do. A
// skeleton (not-yet-decodable) message gets just a `todo!()` `decode`.
void emit_message_decode(Printer& printer, const NameTable& table,
                         const std::vector<std::string>& current_mod, const MessageNode& msg,
                         const std::unordered_set<std::string>& decodable,
                         const FieldModes& modes) {
    printer.set("M", table.messages.at(msg.fqn).type_name);
    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    if (decodable.count(msg.fqn) != 0) {
        const AccessorNames accessors =
            message_accessor_names(msg, decode_reserved_idents(msg, true));
        emit_decode(printer, table, current_mod, msg, accessors, modes);
    } else {
        printer.print(
            "pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) "
            "-> Result<&'a $M$<'a>, ::rapidproto::DecodeError> {\n");
        printer.indent();
        printer.print("todo!()\n");
        printer.outdent();
        printer.print("}\n");
    }
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
                         std::vector<std::string>& current_mod,
                         const std::unordered_set<std::string>& decodable,
                         const FieldModes& modes) {
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_message(printer, table, current_mod, *msgs[i], decodable, modes);
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
            emit_arena_messages(printer, table, addrs(msgs[i]->nested_messages), current_mod,
                                decodable, modes);
            current_mod.pop_back();
            printer.outdent();
            printer.print("}\n");
        }
    }
    // The decode impls for this module's messages, grouped at the bottom (after the structs/getters and
    // submodules above), so the getters read uninterrupted. Each stays in its message's own module.
    for (const MessageNode* msg : msgs) {
        printer.print("\n");
        emit_message_decode(printer, table, current_mod, *msg, decodable, modes);
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
                     std::vector<std::string>& current_mod,
                     const std::unordered_set<std::string>& decodable, const FieldModes& modes) {
    emit_arena_messages(printer, table, node_messages(node), current_mod, decodable, modes);
    for (const auto& [component, child] : node.children) {
        printer.print("\npub mod $m$ {\n", {{"m", child.emitted}});
        printer.indent();
        current_mod.push_back(child.emitted);
        emit_arena_node(printer, table, child, current_mod, decodable, modes);
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

std::string generate_arena(const std::vector<const FileNode*>& files, const FieldModes& modes) {
    PkgTrie root = build_pkg_trie(files);
    assign_emitted_names(root);
    const NameTable table = build_name_table(root);
    const std::unordered_set<std::string> decodable = compute_decodable(files, modes);
    Printer printer;
    printer.print("pub mod arena {\n");
    printer.indent();
    std::vector<std::string> current_mod;
    emit_arena_node(printer, table, root, current_mod, decodable, modes);
    printer.outdent();
    printer.print("}\n");
    return printer.str();
}

std::string generate_file(const std::vector<const FileNode*>& files, const FieldModes& modes) {
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
    return banner + "\n" + generate_common(files) + "\n" + generate_arena(files, modes);
}

}  // namespace rapidproto::rustgen
