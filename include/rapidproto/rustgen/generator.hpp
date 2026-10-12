// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The Rust code generator: emits native Rust decoders from the resolved AST, the counterpart to
// arenagen/streamgen. This first slice emits the shared "common" module — the enum types both
// decoder models reference. Generated code depends only on the `rapidproto` runtime crate.

#include <string>

#include "rapidproto/ast.hpp"

namespace rapidproto::rustgen {

// Emit the `common` module body for `file`: a Rust open-enum newtype plus a classify enum for each
// top-level enum, wrapped in the file's package path as nested `pub mod`s. Enums are model-agnostic
// (plain `i32` newtypes), so this output is identical whichever decoder model is also generated.
// (Nested enums are emitted by a later slice.)
std::string generate_common(const FileNode& file);

// Emit the `arena` module body for `file`: the arena decoder's message structs, wrapped in the
// file's package path. Each top-level message becomes `pub struct Msg<'a>` with accessor signatures
// for its singular scalar/enum/string/bytes fields; `decode` and the accessor bodies are stubbed
// (this slice pins the API surface and that it compiles). Enum-typed fields reference the `common`
// tree. (Repeated/map/oneof fields, nested messages, and real decode bodies are later slices.)
std::string generate_arena(const FileNode& file);

}  // namespace rapidproto::rustgen
