// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Model-agnostic plumbing shared by osmstat_arena.cpp and osmstat_stream.cpp: reading the file,
// walking the PBF blob framing, and inflating blob payloads. Nothing here decodes protobuf --
// each main does that with its own model -- so both programs stay readable on their own.
//
// A .osm.pbf file is a sequence of [4-byte big-endian length][BlobHeader][Blob]. The BlobHeader
// says what the Blob contains ("OSMHeader" once at the start, then "OSMData" blocks) and how big
// the Blob message is; the Blob wraps the payload either raw or zlib-deflated.

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace osmpbf_input {

inline std::optional<std::string> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) return std::nullopt;
    return data;
}

// One framing step: at `offset` in `file`, yield the BlobHeader bytes and the Blob bytes, and
// advance `offset` past both. Returns false (without touching the outputs) at a clean
// end-of-file; malformed framing is reported and also returns false with `offset` poisoned to
// the file end, so callers need only one loop condition.
struct Framed {
    std::string_view header_bytes;
    std::string_view blob_bytes;
};

inline bool next_frame(std::string_view file, std::size_t& offset, Framed& out) {
    if (offset == file.size()) return false;
    if (file.size() - offset < 4) {
        std::fprintf(stderr, "osmstat: truncated frame length at offset %zu\n", offset);
        offset = file.size();
        return false;
    }
    const auto b = [&](std::size_t i) { return std::uint32_t(static_cast<unsigned char>(file[offset + i])); };
    const std::uint32_t header_len = b(0) << 24 | b(1) << 16 | b(2) << 8 | b(3);
    offset += 4;
    // The format caps BlobHeader at 64 KiB and a Blob at 32 MiB; enforcing the caps bounds every
    // allocation this loop makes from untrusted input.
    if (header_len > 64u * 1024 || file.size() - offset < header_len) {
        std::fprintf(stderr, "osmstat: bad BlobHeader length %u at offset %zu\n", header_len, offset - 4);
        offset = file.size();
        return false;
    }
    out.header_bytes = file.substr(offset, header_len);
    offset += header_len;
    return true;
}

inline bool take_blob(std::string_view file, std::size_t& offset, std::int32_t datasize, Framed& out) {
    if (datasize < 0 || datasize > 32 * 1024 * 1024 ||
        file.size() - offset < static_cast<std::size_t>(datasize)) {
        std::fprintf(stderr, "osmstat: bad Blob datasize %d at offset %zu\n", int(datasize), offset);
        offset = file.size();
        return false;
    }
    out.blob_bytes = file.substr(offset, static_cast<std::size_t>(datasize));
    offset += static_cast<std::size_t>(datasize);
    return true;
}

// Inflate `deflated` into `out` (resized to `raw_size`, the size the Blob declares). The buffer
// is caller-owned and reused across blocks: everything decoded from a block -- every borrowed
// string_view -- points into it, so it must stay untouched until that block's walk is done.
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
