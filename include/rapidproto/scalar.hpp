// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Scalar-type classification for proto field types, shared across the front-end passes (parsing,
// feature resolution, type resolution). The full keyword set lives in ONE place (is_scalar_type); the
// narrower predicates derive from it by exclusion, so the list can never drift between callers.

#include <string_view>

namespace rapidproto {

// Every built-in proto scalar keyword (the 15 wire scalar types).
inline bool is_scalar_type(std::string_view type) {
    return type == "int32" || type == "int64" || type == "uint32" || type == "uint64" ||
           type == "sint32" || type == "sint64" || type == "fixed32" || type == "fixed64" ||
           type == "sfixed32" || type == "sfixed64" || type == "float" || type == "double" ||
           type == "bool" || type == "string" || type == "bytes";
}

// Scalars that may be PACKED in a repeated field: every scalar except the length-delimited
// string/bytes (which are never packable).
inline bool is_packable_scalar(std::string_view type) {
    return is_scalar_type(type) && type != "string" && type != "bytes";
}

// Scalars valid as a MAP KEY: integral and string types only. Proto forbids float/double/bytes as
// keys (and never an enum or message); a malformed schema with any other key is rejected in analysis.
inline bool is_valid_map_key_type(std::string_view type) {
    return is_scalar_type(type) && type != "float" && type != "double" && type != "bytes";
}

}  // namespace rapidproto
