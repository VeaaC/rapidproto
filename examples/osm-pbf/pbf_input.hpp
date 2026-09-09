// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Model-agnostic plumbing shared by osmstat_arena.cpp and osmstat_stream.cpp (and reused by the
// benchmark's OSM arm): reading the file, walking the PBF blob framing, and inflating blob
// payloads. Nothing here decodes protobuf -- each main does that with its own model -- so both
// programs stay readable on their own.
//
// A .osm.pbf file is a sequence of [4-byte big-endian length][BlobHeader][Blob]. The BlobHeader
// says what the Blob contains ("OSMHeader" once at the start, then "OSMData" blocks) and how big
// the Blob message is; the Blob wraps the payload either raw or zlib-deflated.

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace osmpbf_input {

// The spec's size discipline: a BlobHeader must stay under 64 KiB and a Blob's UNCOMPRESSED
// payload under 32 MiB ("should" for writers, so readers commonly accept some headroom -- the
// inflate cap below allows 2x). Enforcing bounds here caps every allocation this code makes
// from untrusted input; the serialized-Blob cap reuses the payload figure, which is
// conservative (a Blob never usefully exceeds its payload by much).
inline constexpr std::uint32_t kMaxBlobHeader = 64u * 1024;
inline constexpr std::int64_t kMaxBlob = 32 * 1024 * 1024;
inline constexpr std::int64_t kMaxRawSize = 2 * kMaxBlob;

inline std::optional<std::string> read_file(const char* path) {
    // C stdio, not ifstream: libstdc++'s filebuf throws on read errors (e.g. the path is a
    // directory) regardless of the exception mask, which would abort the program.
    std::FILE* const f = std::fopen(path, "rb");
    if (f == nullptr) {
        return std::nullopt;
    }
    std::string data;
    char buf[1 << 16];
    std::size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) {
        data.append(buf, got);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) {
        return std::nullopt;
    }
    return data;
}

// One framing step: at `offset` in `file`, yield the BlobHeader bytes and advance past them.
// Returns false at a clean end-of-file (offset == file.size(), outputs untouched); malformed
// framing is reported and also returns false with `offset` poisoned PAST the end -- so a caller
// distinguishes "done" from "broken" by `offset == file.size()` after its loop.
struct Framed {
    std::string_view header_bytes;
    std::string_view blob_bytes;
};

inline bool next_frame(std::string_view file, std::size_t& offset, Framed& out) {
    if (offset >= file.size()) {
        return false;  // clean EOF at ==; a poisoned offset (>) stays false and stays poisoned
    }
    if (file.size() - offset < 4) {
        std::fprintf(stderr, "osmstat: truncated frame length at offset %zu\n", offset);
        offset = file.size() + 1;
        return false;
    }
    const auto b = [&](std::size_t i) {
        return std::uint32_t(static_cast<unsigned char>(file[offset + i]));
    };
    const std::uint32_t header_len = b(0) << 24 | b(1) << 16 | b(2) << 8 | b(3);
    offset += 4;
    if (header_len >= kMaxBlobHeader || file.size() - offset < header_len) {
        std::fprintf(stderr, "osmstat: bad BlobHeader length %u at offset %zu\n", header_len,
                     offset - 4);
        offset = file.size() + 1;
        return false;
    }
    out.header_bytes = file.substr(offset, header_len);
    offset += header_len;
    return true;
}

inline bool take_blob(std::string_view file, std::size_t& offset, std::int32_t datasize,
                      Framed& out) {
    if (datasize < 0 || datasize > kMaxBlob ||
        file.size() - offset < static_cast<std::size_t>(datasize)) {
        std::fprintf(stderr, "osmstat: bad Blob datasize %d at offset %zu\n", int(datasize),
                     offset);
        offset = file.size() + 1;
        return false;
    }
    out.blob_bytes = file.substr(offset, static_cast<std::size_t>(datasize));
    offset += static_cast<std::size_t>(datasize);
    return true;
}

// Inflate `deflated` into `out` (resized to `raw_size`, the size the Blob declares -- validate
// it against kMaxRawSize BEFORE calling). The buffer is caller-owned and reused across blocks:
// everything decoded from a block -- every borrowed string_view -- points into it, so it must
// stay untouched until that block's walk is done.
inline bool inflate_blob(std::string_view deflated, std::size_t raw_size, std::string& out) {
    out.resize(raw_size);
    uLongf dest_len = raw_size;
    const int rc = uncompress(reinterpret_cast<Bytef*>(&out[0]), &dest_len,
                              reinterpret_cast<const Bytef*>(deflated.data()), deflated.size());
    if (rc != Z_OK || dest_len != raw_size) {
        std::fprintf(stderr, "osmstat: zlib inflate failed (rc=%d)\n", rc);
        return false;
    }
    return true;
}

// Wall-clock timing for the stderr report: tiny wrapper so the mains read as prose.
struct Stopwatch {
    using Clock = std::chrono::steady_clock;
    Clock::time_point t0 = Clock::now();
    double take() {
        const auto t1 = Clock::now();
        const double s = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
        return s;
    }
};

}  // namespace osmpbf_input
