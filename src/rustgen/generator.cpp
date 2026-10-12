// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter

#include "rapidproto/rustgen/generator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
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

// Split a proto package ("a.b.c") into its components ("" -> none).
std::vector<std::string> package_components(std::string_view package) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= package.size()) {
        const std::size_t dot = package.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? package.size() : dot;
        if (end > start) {
            out.emplace_back(package.substr(start, end - start));
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return out;
}

}  // namespace

std::string generate_common(const FileNode& file) {
    Printer printer;
    const std::vector<std::string> pkg = package_components(file.package);

    for (const std::string& component : pkg) {
        printer.print("pub mod $m$ {\n", {{"m", escape_rust_ident(component)}});
        printer.indent();
    }

    for (std::size_t i = 0; i < file.enums.size(); ++i) {
        if (i != 0) {
            printer.print("\n");
        }
        emit_enum(printer, file.enums[i]);
    }

    for (std::size_t i = 0; i < pkg.size(); ++i) {
        printer.outdent();
        printer.print("}\n");
    }

    return printer.str();
}

}  // namespace rapidproto::rustgen
