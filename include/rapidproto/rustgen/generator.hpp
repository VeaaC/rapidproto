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
// structure: per message a reader handle `<Msg><'a>` (`Msg::new(bytes).decode(visitor)`), a
// `<Msg>Visitor<'a>` trait (one `const WANT_<field>: bool = false` + a defaulted
// `on_<field>(&mut self, V) -> Result<Flow, DecodeError>` method per field, plus an `unknown` handler),
// a `<Msg>::decode` driver whose per-field arm gates on `V::WANT_<field>` (an unselected field
// const-folds to a wire-skip), and a per-message `<msg>_stream! { field => closure, .. }` macro. A
// handler receives a scalar as its Rust primitive, a string as `ProtoStr`, bytes as `&[u8]`, an ENUM as
// its generated `common`-tree newtype, a message/group as the child's reader handle (recurse with
// `child.decode(..)`), and a MAP field as a `(key, value)` pair per entry (the value a child handle when
// it is a message); `Flow::Stop` and an `Err` propagate out of `decode`. The trait and driver name a
// foreign enum newtype / child handle by its real tree-relative path; the `*_stream!` macro expands at
// the consumer's call site where that path is invalid, so each such type is also re-exported into the
// message's stream module as a `#[doc(hidden)] pub type rp_stream_ty_<n>` alias, and the module is
// re-exported under a hidden per-message name (`rp_mod_<fqn>`) next to the runtime (`rp_rt`). Each
// expansion names everything -- trait, types, its own recursion, the runtime -- through its message's
// scope name, which a consumer's glob of the stream module brings in, so globbing several stream
// modules cannot make it ambiguous. The generator reserves the `rp_` prefix for this glue (a
// schema-derived module-scope name starting with `rp_` gains a trailing `_`). Field modes
// (drop/raw/unknown) are arena-storage options that do not apply to a streaming visitor (which
// materializes nothing), so streaming ignores them.
//
// Each macro is a hidden `macro_rules!` re-exported under its friendly name. It is `pub(crate)`
// (usable within the crate that `include!`s the file) unless `export_prefix` is non-empty: then it is
// a `#[macro_export] __rp_<prefix>_<fqn>` re-exported with `pub use`, so the including crate's
// `tests/` or another crate can call it too. The prefix must be an identifier ([A-Za-z_][A-Za-z0-9_]*,
// validated by the caller); distinct prefixes keep two exported files' crate-root names apart.
std::string generate_stream(const std::vector<const FileNode*>& files,
                            const std::string& export_prefix = {});

// Which decoder model(s) `generate_file` emits. `common` is always emitted (both models reference it).
// Default is arena only -- byte-for-byte what every existing arena consumer gets. At least one model
// must be set; the caller enforces that, and that `stream_export_prefix` comes with `stream`.
struct RustModels {
    bool arena = true;
    bool stream = false;
    // Non-empty: make every `*_stream!` macro callable from another crate, defined at the including
    // crate's root under this prefix (see generate_stream). Empty (default): `pub(crate)` macros.
    std::string stream_export_prefix;
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
