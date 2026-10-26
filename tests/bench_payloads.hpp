// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The shared payload directory: the bytes both C++ benches and the Rust benches
// (rust/rapidproto-bench) decode, so a Rust and a C++ number are measured on byte-identical input.
// Each C++ bench's dump mode (RAPIDPROTO_BENCH_DUMP_DIR=<dir>) writes the inputs of every scenario
// it compares and exits; RAPIDPROTO_BENCH_DIR=<dir> makes a run load them instead of building them
// in-process. Layout:
//   <dir>/<name>.bin           an arena scenario, protoc-serialized (bench_arena.cpp)
//   <dir>/stream/<stem>.bin    a streaming scenario (bench_streamgen.cpp)
//   <dir>/rv/<stem>.pool       a varint-sweep pool (both benches; bench_varint.hpp)
//   <dir>/external/<stem>.bin  a real-world dataset's input (osm_blocks: the pool <stem>.pool)
//   <dir>/external/<stem>.sum  the checksum the C++ arms agree on over that input
// A .pool file holds a pool's buffers in order, each a 4-byte little-endian length and its bytes.
// external/ holds only the datasets this arena-bench build has, and only the Rust runners read it:
// the C++ arms load the datasets from their own sources. The Rust runners build none of these
// files, so they are the runners' whole contract.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "bench_varint.hpp"

namespace rpbench {

// RAPIDPROTO_BENCH_DIR, or nullptr when the bench builds its payloads itself.
inline const char* payload_dir() {
    return std::getenv("RAPIDPROTO_BENCH_DIR");
}

// Read a whole payload file, fatal on a missing OR empty file (a 0-byte file would otherwise decode as
// an empty message and silently "measure" nothing rather than erroring).
inline std::string read_payload(const std::string& path) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "bench: cannot read '%s'\n", path.c_str());
        std::exit(1);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string bytes = ss.str();
    if (bytes.empty()) {
        std::fprintf(stderr, "bench: '%s' is empty\n", path.c_str());
        std::exit(1);
    }
    return bytes;
}

// Write `bytes` to `path`, creating its directory. False (after a message) on failure.
inline bool write_payload(const std::filesystem::path& path, const std::string& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    if (!ec && file) {
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
    }
    if (ec || !file) {
        std::fprintf(stderr, "bench: cannot write '%s'\n", path.string().c_str());
        return false;
    }
    return true;
}

// Write `sum` to `path` in decimal: the checksum the C++ arms agree on over a real-world dataset's
// input, which the Rust runner holds its own (field-for-field ported) walks to.
inline bool write_sum(const std::filesystem::path& path, std::uint64_t sum) {
    return write_payload(path, std::to_string(sum) + "\n");
}

// A varint-sweep pool's file under the payload root.
inline std::filesystem::path rv_pool_path(const char* dir, const std::string& stem) {
    return std::filesystem::path(dir) / "rv" / (stem + ".pool");
}

// Fatal: `path` is not a pool, its buffer at byte `at` being truncated or empty.
[[noreturn]] inline void bad_pool(const std::string& path, std::size_t at) {
    std::fprintf(stderr, "bench: '%s' is not a pool (bad buffer at byte %zu)\n", path.c_str(), at);
    std::exit(1);
}

// The buffers of the .pool file `path`, fatal on a malformed file or an empty buffer.
inline std::vector<std::string> read_pool(const std::string& path) {
    const std::string file = read_payload(path);
    std::vector<std::string> buffers;
    std::size_t at = 0;
    while (at < file.size()) {
        if (file.size() - at < 4) {
            bad_pool(path, at);
        }
        std::uint32_t len = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            len |= static_cast<std::uint32_t>(static_cast<unsigned char>(file[at + i])) << (8 * i);
        }
        if (len == 0 || len > file.size() - at - 4) {
            bad_pool(path, at);
        }
        buffers.emplace_back(file, at + 4, len);
        at += 4 + std::size_t{len};
    }
    return buffers;
}

// The varint-sweep pool `stem` -- (dist, count, field_number) -- loaded from <dir>/rv/<stem>.pool when
// RAPIDPROTO_BENCH_DIR is set, else built.
inline VarintPool load_varint_pool(const std::string& stem, const VarintDist& dist, int count,
                                   int field_number = 1) {
    const char* const dir = payload_dir();
    if (dir == nullptr) {
        return build_varint_pool(dist, count, field_number);
    }
    VarintPool pool;
    pool.buffers = read_pool(rv_pool_path(dir, stem).string());
    for (const std::string& b : pool.buffers) {
        pool.total_bytes += static_cast<double>(b.size());
    }
    return pool;
}

// Write `buffers` as the .pool file `path`, through a temporary file renamed into place, so a pool
// is never left partly written. The temporary file is this process's own, so two dumps into one
// directory at once cannot rename each other's.
inline bool write_pool(const std::filesystem::path& path, const std::vector<std::string>& buffers) {
    std::string file;
    for (const std::string& b : buffers) {
        if (b.empty() || b.size() > std::numeric_limits<std::uint32_t>::max()) {
            std::fprintf(stderr, "bench: a pool buffer for '%s' is empty or over 4 GiB\n",
                         path.string().c_str());
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            file.push_back(static_cast<char>((b.size() >> (8 * i)) & 0xFFU));
        }
        file += b;
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp." + std::to_string(::getpid());
    std::error_code ec;
    if (!write_payload(tmp, file)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::fprintf(stderr, "bench: cannot write '%s': %s\n", path.string().c_str(),
                     ec.message().c_str());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// Write the varint-sweep pool `stem` -- (dist, count, field_number) -- to <dir>/rv/<stem>.pool.
inline bool dump_varint_pool(const char* dir, const std::string& stem, const VarintDist& dist,
                             int count, int field_number = 1) {
    return write_pool(rv_pool_path(dir, stem),
                      build_varint_pool(dist, count, field_number).buffers);
}

}  // namespace rpbench
