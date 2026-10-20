// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The post-analysis Rust generation pipeline, shared by native rustgen -- driven by the
// `[rustgen-golden]` test (tests/test_rustgen.cpp), which pins the committed goldens -- and the wasm
// front-end (src/rustgen/wasm_entry.cpp) that ships as the blob. Each caller resolves the field-modes
// profile its own way and reports errors its own way; from the resolved FieldModes on, the sequence
// -- plan the (arena-shared) storage layout, then emit the one merged Rust module tree for the whole
// import closure -- is identical and lives here once. That single shared tail is what makes the
// blob's output byte-identical to native rustgen by construction, not just golden-by-golden.

#include <string>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/rustgen/generator.hpp"

namespace rapidproto::rustgen {

// Emit the Rust decoder for an analyzed `set` under the resolved profile `modes`. The Rust decoder
// shares the arena storage-layout planner, so this plans layouts the same way the arena model does
// (a default/inactive `modes` means every field materializes). The whole import closure becomes ONE
// file: every file in the set contributes its types to the single merged module tree.
inline std::string generate_rust(const ResolvedFileSet& set, const SymbolTable& symbols,
                                 const arenagen::FieldModes& modes, const RustModels& models = {}) {
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    std::vector<const FileNode*> files;
    files.reserve(set.files.size());
    for (const FileNode& file : set.files) {
        files.push_back(&file);
    }
    return generate_file(files, layouts, modes, models);
}

}  // namespace rapidproto::rustgen
