#pragma once

// Decode-relevant option interpretation: lift the fixed, hardcoded set of built-in options that
// affect decoding into typed AST fields, in place. Everything else stays raw (option names are not
// resolved; values stay as the parsed text-format tree). No descriptor.proto dependency.
//
// Interpreted here:
//   [packed = true/false]  -> FieldNode::repeated_encoding (repeated fields)
//   proto2 [default = X]    -> FieldNode::default_value
// Rejected:
//   option message_set_wire_format = true;  -> error (that wire format is not supported)
//
// FeatureSet options (field_presence/enum_type/repeated_field_encoding/message_encoding) are
// handled earlier by resolve_features; group syntax sets is_group/Delimited at parse time.
//
// Preconditions: compute_fqns (for the message_set_wire_format error message) and resolve_types
// (so [packed] is correctly gated on scalar fields) have run. The composed `analyze()`
// (resolve.hpp) runs all the passes in the right order.

#include <variant>

#include "rapidproto/ast.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {

struct ResolvedFileSet;  // defined in rapidproto/resolver.hpp

Result<std::monostate> interpret_options(FileNode& file);
Result<std::monostate> interpret_options(ResolvedFileSet& file_set);

}  // namespace rapidproto
