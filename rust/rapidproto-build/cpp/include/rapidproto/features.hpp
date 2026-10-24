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

#include <variant>

#include "rapidproto/ast.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {

struct ResolvedFileSet;  // defined in rapidproto/resolver.hpp

// Resolve editions features into the AST's decode-relevant fields. A no-op for proto2/proto3, whose
// presence/openness/encoding are fixed at parse time. Errors on an edition whose feature defaults
// this decoder does not know: applying another edition's defaults could decode the schema wrongly,
// and silently, so the file is refused instead.
Result<std::monostate> resolve_features(FileNode& file);
Result<std::monostate> resolve_features(ResolvedFileSet& file_set);

}  // namespace rapidproto
