#pragma once

// Editions feature-inheritance pass. After parsing, this walks the AST top-down and
// resolves each decode-relevant FeatureSet field through the inheritance chain:
//
//   edition defaults -> file option features.* -> message option features.*
//                    -> field/enum element [features.*]
//
// Resolved values are written back into the typed AST fields (FieldNode::presence,
// FieldNode::repeated_encoding, FieldNode::message_encoding, EnumNode::openness).
// Proto2/proto3 files are a no-op: their values were finalized at parse time.

#include "rapidproto/ast.hpp"

namespace rapidproto {

struct ResolvedFileSet;  // defined in rapidproto/resolver.hpp

void resolve_features(FileNode& file);
void resolve_features(ResolvedFileSet& file_set);

}  // namespace rapidproto
