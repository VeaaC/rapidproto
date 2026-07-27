// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The third-party baseline arms of the arena benchmark, declared here and defined in their OWN
// translation units.
//
// This separation is load-bearing, not tidiness. GCC's inline-growth budget is a whole-TU
// property, so while the protozero arms lived beside the RapidProto decoders in
// bench_arena.cpp, changing the size of OUR code changed the codegen of THEIRS: across a build
// with and without RP_FLATTEN, `checksum_protozero` -- which contains no RapidProto code at all
// -- compiled 4.2x smaller (11,974 -> 2,879 bytes) and measured 25% slower. A baseline that
// moves when the code under test changes is not a baseline; it made an A/B across those builds
// uninterpretable, because the control moved as far as the signal did.
//
// Kept in its own object, a baseline is unaffected by anything the RapidProto arms do, so any
// delta it shows across two builds is placement/link noise -- a usable noise meter rather than a
// confound. See architecture.md#decoder-performance.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "rapidproto/runtime.hpp"

namespace rpbaseline {

// Peak memory of a protoc parse, for the like-with-like comparison against the arena's
// bytes_used / bytes_reserved.
struct ProtocMemory {
    std::size_t used;
    std::size_t held;
};

// Each entry point PARSES AND CHECKSUMS in its own translation unit -- exporting only the
// checksum would leave the parse (the measured work) in the caller's TU and reintroduce the
// coupling. Checksums are computed identically to the corresponding RapidProto arm, so the two
// are comparable and a mismatch is a correctness failure.
std::uint64_t protoc_dataset(const std::string& buf);
std::uint64_t protoc_big_set(const std::string& buf);
std::uint64_t protoc_wide(const std::string& buf);
std::uint64_t protoc_record(const std::string& buf);
std::uint64_t protoc_big(const std::string& buf);
std::uint64_t protoc_big_zz(const std::string& buf);
std::uint64_t protoc_big_kinds(const std::string& buf);
ProtocMemory protoc_dataset_memory(const std::string& buf);
#if __has_include(<protozero/pbf_reader.hpp>)
#define RAPIDPROTO_HAVE_PROTOZERO 1
std::uint64_t protozero_dataset(::rapidproto::ByteView buf);
std::uint64_t protozero_big(::rapidproto::ByteView buf);
std::uint64_t protozero_big_zz(::rapidproto::ByteView buf);
std::uint64_t protozero_big_kinds(::rapidproto::ByteView buf);
#endif

}  // namespace rpbaseline
