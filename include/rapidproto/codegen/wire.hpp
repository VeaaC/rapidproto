// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Generator-agnostic wire-format codegen facts used by BOTH streamgen and arenagen: how to read a
// scalar, whether a wire type is packable, and the import-header naming rule. (Each generator decides
// the wire type of a non-scalar field itself, interleaved with its own value handling, so that
// decision is not shared here.) The conversion pre/post strings name ::rapidproto:: helpers that both
// runtimes expose identically.

#include <string>
#include <string_view>
#include <unordered_map>

namespace rapidproto::codegen {

// Wire-format facts for a scalar proto type: the wire type and the value-conversion wrapping the raw
// read result. (The string/bytes value type stays each generator's own -- streamgen's callback
// std::string_view vs arena's ArenaString storage; the one value-typed column that IS identical for
// every C++ generator, the numeric C++ type, is shared separately via cpp_numeric_type below.)
struct ScalarWire {
    std::string_view
        wire;  // WireType enumerator; the reader is derived from it (each generator emits
               // the matching rapidproto::wire::read_* call)
    std::string_view pre;   // conversion prefix wrapping the raw read
    std::string_view post;  // conversion suffix
    // For the Varint scalar types: the NAMED rapidproto::wire:: conversion functor the packed-varint
    // kernel is templated on (so decode_packed_varints instantiates once per type, not per field --
    // see runtime.hpp). Empty for non-Varint types (their packed paths don't use the kernel).
    std::string_view packed_conv;
};

// The wire/conversion facts for a scalar proto type, or nullptr if `type` is not a scalar keyword.
inline const ScalarWire* find_scalar_wire(std::string_view type) {
    static const std::unordered_map<std::string_view, ScalarWire> kTable = {
        {"int32",
         {"Varint", "::rapidproto::varint_to_int32(", ")", "::rapidproto::wire::conv_int32"}},
        {"int64",
         {"Varint", "::rapidproto::varint_to_int64(", ")", "::rapidproto::wire::conv_int64"}},
        {"uint32",
         {"Varint", "static_cast<std::uint32_t>(", ")", "::rapidproto::wire::conv_uint32"}},
        {"uint64", {"Varint", "", "", "::rapidproto::wire::conv_uint64"}},
        {"sint32",
         {"Varint", "::rapidproto::zigzag_decode_32(static_cast<std::uint32_t>(", "))",
          "::rapidproto::wire::conv_sint32"}},
        {"sint64",
         {"Varint", "::rapidproto::zigzag_decode_64(", ")", "::rapidproto::wire::conv_sint64"}},
        {"bool", {"Varint", "::rapidproto::varint_to_bool(", ")", "::rapidproto::wire::conv_bool"}},
        {"fixed32", {"I32", "", "", ""}},
        {"sfixed32", {"I32", "static_cast<std::int32_t>(", ")", ""}},
        {"float", {"I32", "::rapidproto::bit_cast_float(", ")", ""}},
        {"fixed64", {"I64", "", "", ""}},
        {"sfixed64", {"I64", "static_cast<std::int64_t>(", ")", ""}},
        {"double", {"I64", "::rapidproto::bit_cast_double(", ")", ""}},
        {"string", {"Len", "", "", ""}},
        {"bytes", {"Len", "", "", ""}},
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
