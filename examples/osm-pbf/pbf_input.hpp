// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Plumbing shared by both osmstat programs (and the benchmark's OSM arm): file reading, blob
// framing, zlib inflate. Nothing here decodes protobuf -- each main does that with its own
// model.
//
// A .osm.pbf file is a sequence of [4-byte big-endian length][BlobHeader][Blob]: the BlobHeader
// names the blob type and the Blob's size, the Blob wraps the payload raw or zlib-deflated.

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace osmpbf_input {

// The spec keeps a BlobHeader under 64 KiB and a Blob's uncompressed payload under 32 MiB
// ("should" for writers, so the inflate cap allows 2x headroom). These bounds cap every
// allocation made from untrusted input.
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

// One framing step: the BlobHeader bytes at `offset`, advancing past them. Returns nullopt
// at clean end-of-file (offset == file.size()) AND on malformed framing -- but the latter
// reports the error and poisons `offset` PAST the end, so after the loop a caller tells them
// apart with `offset == file.size()`.
inline std::optional<std::string_view> next_frame(std::string_view file, std::size_t& offset) {
    if (offset >= file.size()) {
        return std::nullopt;  // clean EOF at ==; a poisoned offset (>) stays that way
    }
    if (file.size() - offset < 4) {
        std::fprintf(stderr, "osmstat: truncated frame length at offset %zu\n", offset);
        offset = file.size() + 1;
        return std::nullopt;
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
        return std::nullopt;
    }
    const std::string_view header = file.substr(offset, header_len);
    offset += header_len;
    return header;
}

// The Blob bytes after a frame's header, sized by the BlobHeader's datasize field.
inline std::optional<std::string_view> take_blob(std::string_view file, std::size_t& offset,
                                                 std::int32_t datasize) {
    if (datasize < 0 || datasize > kMaxBlob ||
        file.size() - offset < static_cast<std::size_t>(datasize)) {
        std::fprintf(stderr, "osmstat: bad Blob datasize %d at offset %zu\n", int(datasize),
                     offset);
        offset = file.size() + 1;
        return std::nullopt;
    }
    const std::string_view blob = file.substr(offset, static_cast<std::size_t>(datasize));
    offset += static_cast<std::size_t>(datasize);
    return blob;
}

// Inflate `deflated` into `out`, resized to `raw_size` (validate against kMaxRawSize before
// calling). The buffer is reused across blocks, and every string_view decoded from a block
// borrows from it -- it must stay untouched until that block's walk is done.
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
