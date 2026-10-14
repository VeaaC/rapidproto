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
// `extra_reserved` seeds the dedup set beyond `decode` -- for a message whose decode body is emitted
// (slice-1 scalar messages), the struct's storage fields are named by these same accessor names and
// coexist with the decode body's internal locals (`rp_rest`, `rp_tag`, ...) and the `_marker` field,
// so those identifiers are reserved here to guarantee no field accessor collides with one.
AccessorNames message_accessor_names(const MessageNode& msg,
                                     const std::unordered_set<std::string>& extra_reserved) {
    std::unordered_set<std::string> taken = extra_reserved;
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

// Whether a message is a decode candidate on its OWN shape (ignoring what it references): no maps, no
// oneofs, no delimited (group) message field, and every field has implicit/explicit presence with no
// proto2 custom default. (Required and custom defaults, maps, oneofs and groups are later slices.) A
// delimited message field arrives as SGROUP/EGROUP (wire type 3/4), not a length prefix, so a
// length-prefixed decode arm would silently drop it -- it must keep the skeleton until a group slice.
bool self_decodable(const MessageNode& msg) {
    if (!msg.map_fields.empty() || !msg.oneofs.empty()) {
        return false;
    }
    return std::all_of(msg.fields.begin(), msg.fields.end(), [](const FieldNode& field) {
        return field.presence != FieldPresence::Required && !field.default_value.has_value() &&
               (!field.is_message_type || field.message_encoding != MessageEncoding::Delimited);
    });
}

// The FQNs of messages to emit a full decoder for: self-decodable AND every message they reference
// (via a singular or repeated message field) is itself decodable -- otherwise the emitted
// `decode_into` would call one that was never emitted. A fixpoint propagates non-decodability up the
// reference graph; a cycle of otherwise-decodable messages stays decodable (the runtime depth cap
// bounds the recursion). Messages not in the set keep the skeleton (todo!()) for a later slice.
std::unordered_set<std::string> compute_decodable(const std::vector<const FileNode*>& files) {
    std::unordered_map<std::string, const MessageNode*> all;
    for (const FileNode* file : files) {
        collect_messages(addrs(file->messages), all);
    }
    std::unordered_set<std::string> decodable;
    for (const auto& [fqn, msg] : all) {
        if (self_decodable(*msg)) {
            decodable.insert(fqn);
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = decodable.begin(); it != decodable.end();) {
            const MessageNode& msg = *all.at(*it);
            const bool refs_skeleton =
                std::any_of(msg.fields.begin(), msg.fields.end(), [&](const FieldNode& field) {
                    return field.is_message_type && decodable.count(field.resolved_type_fqn) == 0;
                });
            if (refs_skeleton) {
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
// message field is a borrowed pointer; any repeated field is a borrowed slice)? If not (only singular
// numeric / enum / bool fields), the struct carries a `PhantomData<&'a ()>` to use `'a`.
bool message_uses_lifetime(const MessageNode& msg) {
    return std::any_of(msg.fields.begin(), msg.fields.end(), [](const FieldNode& field) {
        return field.is_repeated || field.is_message_type || field.type_name == "string" ||
               field.type_name == "bytes";
    });
}

// Does the decode body allocate from the arena (so its `decode_into` uses the `arena` param)? Repeated
// fields accumulate into an arena `DecodeVec`; a singular message field is arena-allocated. A message
// of only singular scalar/enum/string/bytes fields does neither.
bool body_uses_arena(const MessageNode& msg) {
    return std::any_of(msg.fields.begin(), msg.fields.end(), [](const FieldNode& field) {
        return field.is_repeated || field.is_message_type;
    });
}

// The struct storage type (= the getter return type) for a field: a repeated field is a borrowed
// slice; a singular message is `Option<&'a T<'a>>`; an explicit-presence scalar wraps in `Option`;
// everything else is the bare value type.
std::string field_storage_type(const NameTable& table, const std::vector<std::string>& current_mod,
                               const FieldNode& field) {
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
// an explicit-presence scalar, else the proto3 zero value. (Repeated fields use a `DecodeVec`, not
// this.)
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
                         const std::string& accessor, bool real) {
    // The return type is exactly the storage type: a getter hands back a copy of the stored value (a
    // repeated slice, a singular-message `Option<&T>`, an explicit-presence `Option`, or a bare value).
    const std::string ret = field_storage_type(table, current_mod, field);
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

// The body-relative offset for an error on the current read -- the readers leave the caller's slice
// unadvanced on `Err`, so `rp_rest`'s position is where the failing item began.
constexpr const char* kOffExpr = "rp_body.len() - rp_rest.len()";

// The body-relative offset of the current element inside a packed run: `rp_rest` is already fixed past
// the whole payload, and `rp_p` (the unconsumed payload suffix) shrinks as elements are read, so this
// points at the element being read -- not at the end of the field, as `kOffExpr` would here.
constexpr const char* kPackedOffExpr = "rp_body.len() - rp_rest.len() - rp_p.len()";

// Emit a scalar (Varint/I32/I64) value read from `src` into `rp_v`, erroring body-relative. After it
// `scalar_convert_expr` yields the typed value. `packed` selects the error offset: inside a packed run
// the current element is at `kPackedOffExpr` (the field's own `rp_p`); otherwise at `kOffExpr`.
void emit_scalar_value_read(Printer& printer, const FieldNode& field, const std::string& src,
                            bool packed) {
    const char* off = packed ? kPackedOffExpr : kOffExpr;
    const std::string wire = scalar_wire_variant(field);
    if (wire == "I32") {
        printer.print("let mut rp_v = 0u32;\n");
        printer.print("$s$ = match ::rapidproto::wire::read_fixed32($s$, &mut rp_v) {\n",
                      {{"s", src}});
    } else if (wire == "I64") {
        printer.print("let mut rp_v = 0u64;\n");
        printer.print("$s$ = match ::rapidproto::wire::read_fixed64($s$, &mut rp_v) {\n",
                      {{"s", src}});
    } else {
        printer.print("let mut rp_v = 0u64;\n");
        printer.print("$s$ = match ::rapidproto::wire::read_varint($s$, &mut rp_v) {\n",
                      {{"s", src}});
    }
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", off}});
    printer.outdent();
    printer.print("};\n");
}

// Emit a length-delimited payload read from `src` into `rp_p`, erroring body-relative.
void emit_len_read(Printer& printer, const std::string& src) {
    printer.print("let mut rp_p: &[u8] = &[];\n");
    printer.print("$s$ = match ::rapidproto::wire::read_length_delimited($s$, &mut rp_p) {\n",
                  {{"s", src}});
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", kOffExpr}});
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

// Emit the `match (field, wire)` arm(s) for one field: one arm for a singular value, two for a
// repeated numeric (unpacked + packed), reading via the value-threaded wire primitives and storing
// into the field's local (`accessor`) or pushing into its accumulator (`rp_acc_<n>`). A singular
// message is recursed into with `decode_into` at depth+1 and arena-allocated; a repeated message's
// element is pushed by value. A singular message seen twice is rejected (matching C++).
void emit_field_arms(Printer& printer, const NameTable& table,
                     const std::vector<std::string>& current_mod, const FieldNode& field,
                     const std::string& accessor) {
    const std::string nstr = std::to_string(field.number);
    const std::string acc = "rp_acc_" + nstr;

    if (field.is_message_type && !field.is_repeated) {
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        printer.print("if $a$.is_some() {\n", {{"a", accessor}});
        printer.indent();
        printer.print(
            "return Err(::rapidproto::DecodeError::new("
            "::rapidproto::ErrorKind::RepeatedSingularMessage, $off$).with_field($n$));\n",
            {{"off", kOffExpr}, {"n", nstr}});
        printer.outdent();
        printer.print("}\n");
        emit_len_read(printer, "rp_rest");
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
        printer.outdent();
        printer.print("}\n");
        return;
    }
    if (field.is_message_type && field.is_repeated) {
        const std::string path = arena_message_path(table, current_mod, field.resolved_type_fqn);
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        emit_len_read(printer, "rp_rest");
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
        emit_scalar_value_read(printer, field, "rp_rest", false);
        emit_push(printer, acc, conv);
        printer.outdent();
        printer.print("}\n");
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        emit_len_read(printer, "rp_rest");
        printer.print("while !rp_p.is_empty() {\n");
        printer.indent();
        emit_scalar_value_read(printer, field, "rp_p", true);
        emit_push(printer, acc, conv);
        printer.outdent();
        printer.print("}\n");
        printer.outdent();
        printer.print("}\n");
        return;
    }
    if (field.is_repeated) {  // repeated string/bytes (wire == Len, not packable)
        printer.print("($n$u32, ::rapidproto::wire::WireType::Len) => {\n", {{"n", nstr}});
        printer.indent();
        emit_len_read(printer, "rp_rest");
        emit_push(printer, acc, conv);
        printer.outdent();
        printer.print("}\n");
        return;
    }
    // Singular scalar / enum / string / bytes.
    printer.print("($n$u32, ::rapidproto::wire::WireType::$w$) => {\n", {{"n", nstr}, {"w", wire}});
    printer.indent();
    if (wire == "Len") {
        emit_len_read(printer, "rp_rest");
    } else {
        emit_scalar_value_read(printer, field, "rp_rest", false);
    }
    printer.print(field.presence == FieldPresence::Explicit ? "$a$ = Some($c$);\n" : "$a$ = $c$;\n",
                  {{"a", accessor}, {"c", conv}});
    printer.outdent();
    printer.print("}\n");
}

// Emit `decode()` + the `decode_into()` worker for a decodable message: a field loop over `rp_body`
// driving the fused `read_tag_or_end`, matching `(field, wire)` to read each known field (a wrong wire
// type for a known field falls to `skip`, like an unknown field), accumulating repeated fields in
// `DecodeVec`s and recursing into sub-messages, then constructing the message. `decode_into` is
// `pub(crate)` so sub-messages in other modules can recurse into it; `field_locals` are the storage
// field names (= accessors).
void emit_decode(Printer& printer, const NameTable& table,
                 const std::vector<std::string>& current_mod, const MessageNode& msg,
                 const std::vector<std::string>& field_locals) {
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
        {{"ap", body_uses_arena(msg) ? "rp_arena" : "_rp_arena"}});
    printer.indent();
    printer.print("if rp_depth > ::rapidproto::MAX_DECODE_DEPTH {\n");
    printer.indent();
    printer.print(
        "return "
        "Err(::rapidproto::DecodeError::new(::rapidproto::ErrorKind::DepthLimitExceeded, 0));\n");
    printer.outdent();
    printer.print("}\n");
    // Storage locals: a repeated field accumulates into a `DecodeVec`; the rest are plain values.
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        const FieldNode& field = msg.fields[i];
        if (field.is_repeated) {
            printer.print("let mut rp_acc_$n$ = ::rapidproto::DecodeVec::new(rp_arena);\n",
                          {{"n", std::to_string(field.number)}});
        } else {
            printer.print("let mut $l$: $t$ = $d$;\n",
                          {{"l", field_locals[i]},
                           {"t", field_storage_type(table, current_mod, field)},
                           {"d", field_default_expr(table, current_mod, field)}});
        }
    }
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
                  {{"off", kOffExpr}});
    printer.outdent();
    printer.print("};\n");
    printer.print("match (rp_tag.field, rp_tag.wire) {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        emit_field_arms(printer, table, current_mod, msg.fields[i], field_locals[i]);
    }
    printer.print("_ => {\n");
    printer.indent();
    printer.print("rp_rest = match ::rapidproto::wire::skip(rp_rest, rp_tag) {\n");
    printer.indent();
    printer.print("Ok(r) => r,\n");
    printer.print("Err(k) => return Err(::rapidproto::DecodeError::new(k, $off$)),\n",
                  {{"off", kOffExpr}});
    printer.outdent();
    printer.print("};\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n");  // match
    printer.outdent();
    printer.print("}\n");  // loop
    printer.print("Ok($M$ {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        if (msg.fields[i].is_repeated) {
            printer.print("$l$: rp_acc_$n$.into_slice(),\n",
                          {{"l", field_locals[i]}, {"n", std::to_string(msg.fields[i].number)}});
        } else {
            printer.print("$l$,\n", {{"l", field_locals[i]}});
        }
    }
    if (!message_uses_lifetime(msg)) {
        printer.print("_marker: ::core::marker::PhantomData,\n");
    }
    printer.outdent();
    printer.print("})\n");
    printer.outdent();
    printer.print("}\n");
}

// Emit `pub struct Msg<'a> { .. } impl<'a> Msg<'a> { accessors; decode }`. `current_mod` is the module
// the struct is emitted in, used to compute cross-references. A decodable message
// ([`compute_decodable`]) gets real field storage, reading accessors and a real decode; any other
// message keeps the skeleton (`PhantomData`, `todo!()` accessors + decode) for a later slice.
void emit_message(Printer& printer, const NameTable& table,
                  const std::vector<std::string>& current_mod, const MessageNode& msg,
                  const std::unordered_set<std::string>& decodable) {
    printer.set("M", table.messages.at(msg.fqn).type_name);
    const bool supported = decodable.count(msg.fqn) != 0;

    // Reserve the decode body's internal identifiers (and `_marker`, and each repeated field's
    // accumulator) so no field accessor -- which is also the struct storage field name -- collides.
    std::unordered_set<std::string> reserved;
    if (supported) {
        reserved = {"decode_into", "_marker", "rp_body", "rp_arena", "_rp_arena", "rp_depth",
                    "rp_rest",     "rp_tag",  "rp_v",    "rp_p",     "rp_sub"};
        for (const FieldNode& field : msg.fields) {
            if (field.is_repeated) {
                reserved.insert("rp_acc_" + std::to_string(field.number));
            }
        }
    }
    const AccessorNames accessors = message_accessor_names(msg, reserved);

    printer.print("pub struct $M$<'a> {\n");
    printer.indent();
    if (supported) {
        for (std::size_t i = 0; i < msg.fields.size(); ++i) {
            printer.print("$l$: $t$,\n",
                          {{"l", accessors.fields[i]},
                           {"t", field_storage_type(table, current_mod, msg.fields[i])}});
        }
        if (!message_uses_lifetime(msg)) {
            printer.print("_marker: ::core::marker::PhantomData<&'a ()>,\n");
        }
    } else {
        printer.print("_marker: ::core::marker::PhantomData<&'a ()>,\n");
    }
    printer.outdent();
    printer.print("}\n\n");

    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        emit_field_accessor(printer, table, current_mod, msg.fields[i], accessors.fields[i],
                            supported);
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
    if (supported) {
        emit_decode(printer, table, current_mod, msg, accessors.fields);
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
                         const std::unordered_set<std::string>& decodable) {
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_message(printer, table, current_mod, *msgs[i], decodable);
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
                                decodable);
            current_mod.pop_back();
            printer.outdent();
            printer.print("}\n");
        }
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
                     const std::unordered_set<std::string>& decodable) {
    emit_arena_messages(printer, table, node_messages(node), current_mod, decodable);
    for (const auto& [component, child] : node.children) {
        printer.print("\npub mod $m$ {\n", {{"m", child.emitted}});
        printer.indent();
        current_mod.push_back(child.emitted);
        emit_arena_node(printer, table, child, current_mod, decodable);
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

std::string generate_arena(const std::vector<const FileNode*>& files) {
    PkgTrie root = build_pkg_trie(files);
    assign_emitted_names(root);
    const NameTable table = build_name_table(root);
    const std::unordered_set<std::string> decodable = compute_decodable(files);
    Printer printer;
    printer.print("pub mod arena {\n");
    printer.indent();
    std::vector<std::string> current_mod;
    emit_arena_node(printer, table, root, current_mod, decodable);
    printer.outdent();
    printer.print("}\n");
    return printer.str();
}

std::string generate_file(const std::vector<const FileNode*>& files) {
    return "// @generated by rapidprotoc --rust -- do not edit.\n\n" + generate_common(files) +
           "\n" + generate_arena(files);
}

}  // namespace rapidproto::rustgen
