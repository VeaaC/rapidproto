// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Generator-agnostic wire-format codegen facts used by BOTH streamgen and arenagen: how to read a
// scalar, whether a wire type is packable, and the import-header naming rule. (Each generator decides
// the wire type of a non-scalar field itself, interleaved with its own value handling, so that
// decision is not shared here.) Read CALLS are bare WireReader methods with no reader variable baked
// in; each generator prefixes them with its own reader. The conversion pre/post strings name
// ::rapidproto:: helpers that both runtimes expose identically.

#include <string>
#include <string_view>
#include <unordered_map>

namespace rapidproto::codegen {

// Wire-format facts for a scalar proto type: the read method and the value-conversion wrapping the
// raw result. (The string/bytes value type stays each generator's own -- streamgen's callback
// std::string_view vs arena's ArenaString storage; the one value-typed column that IS identical for
// every C++ generator, the numeric C++ type, is shared separately via cpp_numeric_type below.)
struct ScalarWire {
    std::string_view wire;  // WireType enumerator
    std::string_view read;  // WireReader method (no reader variable)
    std::string_view pre;   // conversion prefix wrapping the raw read
    std::string_view post;  // conversion suffix
};

// The wire/conversion facts for a scalar proto type, or nullptr if `type` is not a scalar keyword.
inline const ScalarWire* find_scalar_wire(std::string_view type) {
    static const std::unordered_map<std::string_view, ScalarWire> kTable = {
        {"int32", {"Varint", "read_varint()", "::rapidproto::varint_to_int32(", ")"}},
        {"int64", {"Varint", "read_varint()", "::rapidproto::varint_to_int64(", ")"}},
        {"uint32", {"Varint", "read_varint()", "static_cast<std::uint32_t>(", ")"}},
        {"uint64", {"Varint", "read_varint()", "", ""}},
        {"sint32",
         {"Varint", "read_varint()", "::rapidproto::zigzag_decode_32(static_cast<std::uint32_t>(",
          "))"}},
        {"sint64", {"Varint", "read_varint()", "::rapidproto::zigzag_decode_64(", ")"}},
        {"bool", {"Varint", "read_varint()", "::rapidproto::varint_to_bool(", ")"}},
        {"fixed32", {"I32", "read_fixed32()", "", ""}},
        {"sfixed32", {"I32", "read_fixed32()", "static_cast<std::int32_t>(", ")"}},
        {"float", {"I32", "read_fixed32()", "::rapidproto::bit_cast_float(", ")"}},
        {"fixed64", {"I64", "read_fixed64()", "", ""}},
        {"sfixed64", {"I64", "read_fixed64()", "static_cast<std::int64_t>(", ")"}},
        {"double", {"I64", "read_fixed64()", "::rapidproto::bit_cast_double(", ")"}},
        {"string", {"Len", "read_length_delimited()", "", ""}},
        {"bytes", {"Len", "read_length_delimited()", "", ""}},
    };
    const auto it = kTable.find(type);
    return it != kTable.end() ? &it->second : nullptr;
}

// The C++ type of a NUMERIC/bool proto scalar (int32 -> std::int32_t, fixed64 -> std::uint64_t, ...),
// or "" if `type` is not a numeric scalar keyword. string/bytes are excluded ON PURPOSE: their C++
// type is each generator's own (streamgen's callback `std::string_view` vs arena's `ArenaString`
// storage), so the shared layer commits only to the numeric mapping -- which IS identical for every
// C++ generator. (This is the one value-typed column the generators genuinely share; the wire facts
// above and this together replace the per-generator scalar -> C++-type tables.)
inline std::string_view cpp_numeric_type(std::string_view type) {
    static const std::unordered_map<std::string_view, std::string_view> kTable = {
        {"int32", "std::int32_t"},
        {"sint32", "std::int32_t"},
        {"sfixed32", "std::int32_t"},
        {"uint32", "std::uint32_t"},
        {"fixed32", "std::uint32_t"},
        {"int64", "std::int64_t"},
        {"sint64", "std::int64_t"},
        {"sfixed64", "std::int64_t"},
        {"uint64", "std::uint64_t"},
        {"fixed64", "std::uint64_t"},
        {"float", "float"},
        {"double", "double"},
        {"bool", "bool"},
    };
    const auto it = kTable.find(type);
    return it != kTable.end() ? it->second : std::string_view{};
}

// Whether a repeated element of the given wire type may also arrive packed in a single LEN payload
// (the numeric scalars and enums -- everything that is neither length-delimited nor a group).
inline bool is_packable_wire(std::string_view wire) {
    return wire != "Len" && wire != "SGroup";
}

// The generated header file name for an imported .proto path: strip the ".proto" suffix and append
// the generator's own extension (e.g. ".rp.stream.hpp" / ".rp.hpp").
inline std::string import_header(std::string_view path, std::string_view extension) {
    constexpr std::string_view kProto = ".proto";
    if (path.size() >= kProto.size() && path.substr(path.size() - kProto.size()) == kProto) {
        path.remove_suffix(kProto.size());
    }
    return std::string(path) + std::string(extension);
}

// The shared "common header" include a generated decoder emits for its OWN enums. The decoder and its
// common are always written into the SAME directory (cli::header_path gives both the same stem), so
// reference the common by BASENAME -- a same-directory quote-include that resolves with no -I
// dependency, even when the decoder is nested under a subdir (e.g. a well-known type under
// google/protobuf/). `filename` is the file's import path (possibly subdir'd or absolute); its last
// path component names the sibling.
inline std::string common_sibling_include(std::string_view filename) {
    const auto slash = filename.find_last_of('/');
    if (slash != std::string_view::npos) {
        filename.remove_prefix(slash + 1);
    }
    return import_header(filename, ".rp.common.hpp");
}

}  // namespace rapidproto::codegen
