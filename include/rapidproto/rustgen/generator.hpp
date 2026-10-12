// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The Rust code generator: emits native Rust decoders from the resolved AST, the counterpart to
// arenagen/streamgen. Generated code depends only on the `rapidproto` runtime crate.

#include <optional>
#include <string>

#include "rapidproto/ast.hpp"

namespace rapidproto::rustgen {

// Emit the `common` module body for `file`: a Rust open-enum newtype plus a classify enum for each
// enum (top-level and nested), wrapped in the file's package path as nested `pub mod`s. Enums are
// model-agnostic (plain `i32` newtypes), so this output is identical whichever decoder model is also
// generated. Nested enums live in a per-message submodule mirroring the arena nesting.
std::string generate_common(const FileNode& file);

// Emit the `arena` module body for `file`: the arena decoder's message structs (nested messages in
// per-message submodules), each `pub struct Msg<'a>` with accessor signatures for its fields
// (scalar/enum/string/bytes, repeated slices, map views) and a native-enum type per oneof; `decode`
// and the accessor bodies are stubbed. Enum-typed fields reference the sibling `common` tree.
std::string generate_arena(const FileNode& file);

// The full generated Rust source for `file`: the `common` tree followed by the `arena` tree (the two
// are siblings, included together under one parent module). This is the single source of truth for a
// schema's `.rs` content -- the CLI writes it and the golden test compares against it.
std::string generate_file(const FileNode& file);

// Why `file` cannot yet be emitted as Rust, or nullopt if it can. The one current reason is a
// cross-file type reference (a field/map/oneof referencing a type -- including a well-known type --
// declared in another file): the per-file module layout does not compose across separately generated
// files yet, so the CLI refuses such a schema cleanly rather than emitting a dangling path. The
// returned string is a human-readable reason naming the offending reference.
//
// It inspects the reference shapes the generator actually emits (message/enum fields, map values,
// oneof fields; recursively). `extend` blocks are NOT inspected -- safe only because the generator
// emits nothing for them; if extension emission is ever added, add its cross-file refs here too.
std::optional<std::string> cross_file_dependency(const FileNode& file);

}  // namespace rapidproto::rustgen
