// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter

#pragma once

// The Rust code generator: emits native Rust decoders from the resolved AST, the counterpart to
// arenagen/streamgen. Generated code depends only on the `rapidproto` runtime crate.
//
// Unlike the C++ generators (one header per .proto), the Rust generator emits ONE module tree for a
// whole invocation: the import closure is merged into a single `common` + `arena` pair, so a cross-
// file or cross-package reference resolves inside one file with no include/compose step. Callers pass
// the files to emit together (the resolved set) as a vector of pointers.

#include <string>
#include <vector>

#include "rapidproto/arenagen/modes.hpp"  // FieldModes: the shared drop/raw/unknown-fields resolver output
#include "rapidproto/ast.hpp"

namespace rapidproto::rustgen {

// Field modes (drop / raw / unknown-fields) are a CONSUMER selection resolved by the shared
// arenagen resolver; the Rust generator consumes the same `FieldModes`. A default-constructed
// (inactive) value means "materialize everything" -- byte-identical to a no-modes generation.
using arenagen::FieldModes;

// Emit the `common` module body for `files`: a Rust open-enum newtype plus a classify enum for each
// enum (top-level and nested), under `pub mod common` with one `pub mod` per package component. Enums
// are model-agnostic (plain `i32` newtypes), identical whichever decoder model is also generated.
// Same-package files merge into one module scope; nested enums live in a per-message submodule.
std::string generate_common(const std::vector<const FileNode*>& files);

// Emit the `arena` module body for `files`: the arena decoder's message structs (nested messages in
// per-message submodules, oneofs as native enums), under `pub mod arena` with one `pub mod` per
// package component. `modes` selects per-field drop/raw and per-message unknown-fields handling;
// default (inactive) materializes everything. Enum-typed fields reference the sibling `common` tree.
std::string generate_arena(const std::vector<const FileNode*>& files, const FieldModes& modes = {});

// The full generated Rust source for `files`: the `common` tree followed by the `arena` tree (the two
// are siblings, included together under one parent module). This is the single source of truth for an
// invocation's `.rs` content -- the CLI writes it and the golden test compares against it. `modes` is
// the resolved field-modes profile (default: materialize everything).
std::string generate_file(const std::vector<const FileNode*>& files, const FieldModes& modes = {});

}  // namespace rapidproto::rustgen
