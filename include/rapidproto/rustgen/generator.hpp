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
// (Nested enums and message types are emitted by later slices.)
std::string generate_common(const FileNode& file);

}  // namespace rapidproto::rustgen
