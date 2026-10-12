// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter

#include "rapidproto/rustgen/generator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/rustgen/names.hpp"

namespace rapidproto::rustgen {
namespace {

using codegen::Printer;

// Emit the newtype + classify enum for one proto enum.
//
//   #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
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
// surface and `kind()` fuses to a branchless switch at the call site.
void emit_enum(Printer& printer, const EnumNode& node) {
    const std::string type_name = escape_rust_ident(node.name);
    const std::string kind_name = type_name + "Kind";  // TODO(rustgen): dedup vs user type names
    const std::vector<std::string> consts = enum_const_names(node);
    const std::vector<std::string> variants = enum_variant_names(node);

    printer.set("T", type_name);
    printer.set("K", kind_name);

    printer.print("#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]\n");
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

// A reference from inside `arena::<pkg>::Msg` to an enum at `fqn` in the `common` tree. Enums live
// under `common::<pkg>::Name`, a sibling of `arena` under the include root, so climb `super` out of
// the message's package (plus the `arena` root) and descend into `common`.
std::string common_type_path(const FileNode& file, const std::string& fqn) {
    const std::size_t supers = 1 + dotted_components(file.package).size();
    std::string out;
    for (std::size_t i = 0; i < supers; ++i) {
        out += "super::";
    }
    out += "common";
    for (const std::string& part : dotted_components(fqn)) {
        out += "::";
        out += escape_rust_ident(part);
    }
    return out;
}

// Accessor method names for a message's regular fields, in declaration order: each proto field name
// escaped, then deduped against the other fields AND the reserved method names this `impl` also
// defines (`decode`), with a trailing `_` on collision. Without this, a field named `decode`, or two
// fields escaping to the same identifier (`self` -> `self_` beside a field `self_`), would emit
// duplicate `impl` items -- non-compiling Rust for a protoc-valid schema. Mirrors the enum dedup.
std::vector<std::string> message_accessor_names(const MessageNode& msg) {
    std::unordered_set<std::string> taken;
    taken.insert("decode");  // the generated decode() method lives in the same impl
    std::vector<std::string> out;
    out.reserve(msg.fields.size());
    for (const FieldNode& field : msg.fields) {
        std::string name = escape_rust_ident(field.name);
        while (!taken.insert(name).second) {
            name += '_';
        }
        out.push_back(std::move(name));
    }
    return out;
}

// Emit the accessor signature + stubbed body for one regular (non-oneof, non-map) field, or skip it
// with a note for the field kinds a later slice adds. `decode()` is stubbed, so bodies are `todo!()`
// and the struct carries no field storage yet -- this slice pins the accessor SIGNATURES and that
// they compile, not the arena layout. `accessor` is the pre-deduped method name.
void emit_field_accessor(Printer& printer, const FileNode& file, const FieldNode& field,
                         const std::string& accessor) {
    if (field.is_repeated || field.is_message_type) {
        printer.print("// TODO: field $n$ ($kind$) -- added by a later slice\n",
                      {{"n", field.name}, {"kind", field.is_repeated ? "repeated" : "message"}});
        return;
    }
    const std::string base = field.is_enum_type ? common_type_path(file, field.resolved_type_fqn)
                                                : rust_scalar_type(field.type_name);
    const std::string ret =
        field.presence == FieldPresence::Explicit ? "Option<" + base + ">" : base;
    printer.print("pub fn $a$(&self) -> $t$ {\n", {{"a", accessor}, {"t", ret}});
    printer.indent();
    printer.print("todo!()\n");
    printer.outdent();
    printer.print("}\n");
}

// Emit `pub struct Msg<'a> { .. } impl<'a> Msg<'a> { accessors; decode }`.
void emit_message(Printer& printer, const FileNode& file, const MessageNode& msg) {
    const std::string type_name = escape_rust_ident(msg.name);
    printer.set("M", type_name);
    printer.print("pub struct $M$<'a> {\n");
    printer.indent();
    printer.print("_marker: ::core::marker::PhantomData<&'a ()>,\n");
    printer.outdent();
    printer.print("}\n\n");

    printer.print("impl<'a> $M$<'a> {\n");
    printer.indent();
    const std::vector<std::string> accessors = message_accessor_names(msg);
    for (std::size_t i = 0; i < msg.fields.size(); ++i) {
        emit_field_accessor(printer, file, msg.fields[i], accessors[i]);
    }
    for (const MapFieldNode& map : msg.map_fields) {
        printer.print("// TODO: map field $n$ -- added by a later slice\n", {{"n", map.name}});
    }
    for (const OneofNode& oneof : msg.oneofs) {
        printer.print("// TODO: oneof $n$ -- added by a later slice\n", {{"n", oneof.name}});
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

}  // namespace

std::string generate_common(const FileNode& file) {
    Printer printer;
    const std::vector<std::string> pkg = dotted_components(file.package);
    open_module_tree(printer, "common", pkg);
    for (std::size_t i = 0; i < file.enums.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_enum(printer, file.enums[i]);
    }
    close_module_tree(printer, pkg);
    return printer.str();
}

std::string generate_arena(const FileNode& file) {
    Printer printer;
    const std::vector<std::string> pkg = dotted_components(file.package);
    open_module_tree(printer, "arena", pkg);
    for (std::size_t i = 0; i < file.messages.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_message(printer, file, file.messages[i]);
    }
    close_module_tree(printer, pkg);
    return printer.str();
}

}  // namespace rapidproto::rustgen
