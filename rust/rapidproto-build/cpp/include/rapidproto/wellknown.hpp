#pragma once

// Lookup for the embedded well-known-type .proto sources. The 11 WKT files are
// vendored under wellknown/ (BSD-3-Clause, Google) and embedded into a generated
// translation unit (src/wellknown_generated.cpp) by wellknown/embed_wellknown.py.
//
// The resolver consults this AFTER the configured include paths, so a WKT present on
// disk overrides the embedded copy.

#include <optional>
#include <string_view>

namespace rapidproto {

// Returns the embedded source for a canonical WKT import path (e.g.
// "google/protobuf/descriptor.proto"), or nullopt if it is not a well-known type.
std::optional<std::string_view> wellknown_source(std::string_view import_path);

}  // namespace rapidproto
