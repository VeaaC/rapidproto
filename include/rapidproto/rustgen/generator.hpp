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
#include <utility>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"  // LayoutSet: the shared storage-layout planner's output
#include "rapidproto/arenagen/modes.hpp"  // FieldModes: the shared drop/raw/unknown-fields resolver output
#include "rapidproto/ast.hpp"

namespace rapidproto::rustgen {

// Field modes (drop / raw / unknown-fields) are a CONSUMER selection resolved by the shared
// arenagen resolver; the Rust generator consumes the same `FieldModes`. A default-constructed
// (inactive) value means "materialize everything" -- byte-identical to a no-modes generation.
using arenagen::FieldModes;

// The Rust decoder shares arenagen's STORAGE-LAYOUT planner (`arenagen::plan_layouts`): the generator
// consumes each message's `MessageLayout` for the storage DECISIONS -- which singular sub-messages are
// inlined by value (fixed-size, <= the cutoff) vs behind an arena pointer, and the bit-packed
// presence/bool mask (explicit-presence fields + bool values share mask words). The planner's C++ byte
// offsets/sizes are ignored; only the model-agnostic decisions are used, so the two decoders inline and
// track presence for the same fields. Caller computes it (`plan_layouts(set, symbols, {&modes})`).
using arenagen::LayoutSet;

// Emit the `common` module body for `files`: a Rust open-enum newtype plus a classify enum for each
// enum (top-level and nested), under `pub mod common` with one `pub mod` per package component. Enums
// are model-agnostic (plain `i32` newtypes), identical whichever decoder model is also generated.
// Same-package files merge into one module scope; nested enums live in a per-message submodule.
std::string generate_common(const std::vector<const FileNode*>& files);

// Emit the `arena` module body for `files`: the arena decoder's message structs (nested messages in
// per-message submodules, oneofs as native enums), under `pub mod arena` with one `pub mod` per
// package component. `modes` selects per-field drop/raw and per-message unknown-fields handling;
// default (inactive) materializes everything. Enum-typed fields reference the sibling `common` tree.
std::string generate_arena(const std::vector<const FileNode*>& files, const LayoutSet& layouts,
                           const FieldModes& modes = {});

// Emit the `stream` module body for `files`, under `pub mod stream` mirroring `arena`'s module
// structure: per message a `<Msg>Visitor<'a>` trait (one `const WANT_<field>: bool = false` + a
// defaulted `on_<field>` method per field), a `<Msg>::decode<V: <Msg>Visitor>(input, visitor)` driver
// whose per-field arm gates on `V::WANT_<field>` (an unselected field const-folds to a wire-skip), and
// a per-message `<msg>_stream! { field => closure, .. }` builder macro. A handler receives a scalar as
// its Rust primitive, a string as `ProtoStr`, bytes/message/group as `&[u8]` (the message/group
// payload -- the consumer recurses with the child decoder), and an ENUM as its raw `i32` (NOT the
// `common`-tree newtype): the macro expands at the consumer's call site, so every value type it names
// must be a builtin or an absolute `::rapidproto::` path, never a tree-relative one. Field modes and
// map fields are not yet wired into streaming (a map field is not selectable).
std::string generate_stream(const std::vector<const FileNode*>& files);

// Which decoder model(s) `generate_file` emits. `common` is always emitted (both models reference it).
// Default is arena only -- byte-for-byte what every existing arena consumer gets. At least one must be
// set; the caller enforces that.
struct RustModels {
    bool arena = true;
    bool stream = false;
};

// The full generated Rust source for `files`: the `common` tree, then the selected decoder tree(s)
// (`arena` and/or `stream`), all siblings included together under one parent module. This is the single
// source of truth for an invocation's `.rs` content -- the CLI writes it and the golden test compares
// against it. `modes` is the resolved field-modes profile (default: materialize everything); `models`
// selects which decoder model(s) to emit (default: arena only).
std::string generate_file(const std::vector<const FileNode*>& files, const LayoutSet& layouts,
                          const FieldModes& modes = {}, const RustModels& models = {});

// The absolute arena type path (e.g. `arena::pkg::Msg`) for every message in `files`, keyed by its
// protobuf FQN -- the path that `include!`-ing the generated `.rs` exposes. The differential test's
// harness generator uses this to decode a message named at runtime by its FQN (the Rust analog of
// the C++ harness computing C++ type names); it is independent of the layout/modes, since naming is.
std::vector<std::pair<std::string, std::string>> arena_type_paths(
    const std::vector<const FileNode*>& files);

}  // namespace rapidproto::rustgen
