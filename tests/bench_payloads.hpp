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
//   <dir>/rv/<stem>/<i>.bin    buffer i of a varint-sweep pool (both benches; bench_varint.hpp)
//   <dir>/rv/<stem>/pool.txt   the pool's buffer count, written last: a pool without it is incomplete
//   <dir>/external/<stem>.bin  a real-world dataset's input (osm_blocks: the pool <stem>/)
//   <dir>/external/<stem>.sum  the checksum the C++ arms agree on over that input
// external/ holds only the datasets this arena-bench build has, and only the Rust runners read it:
// the C++ arms load the datasets from their own sources. The Rust runners build none of these
// files, so they are the runners' whole contract.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

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

// A varint-sweep pool's directory under the payload root.
inline std::filesystem::path rv_pool_path(const char* dir, const std::string& stem) {
    return std::filesystem::path(dir) / "rv" / stem;
}

// The varint-sweep pool `stem` -- (dist, count, field_number) -- loaded from <dir>/rv/<stem>/ when
// RAPIDPROTO_BENCH_DIR is set (exactly the buffers its pool.txt counts), else built.
inline VarintPool load_varint_pool(const std::string& stem, const VarintDist& dist, int count,
                                   int field_number = 1) {
    const char* const dir = payload_dir();
    if (dir == nullptr) {
        return build_varint_pool(dist, count, field_number);
    }
    const std::filesystem::path pool_dir = rv_pool_path(dir, stem);
    const std::string manifest = read_payload((pool_dir / "pool.txt").string());
    const long n = std::strtol(manifest.c_str(), nullptr, 10);
    if (n <= 0) {
        std::fprintf(stderr, "bench: bad pool count in '%s'\n",
                     (pool_dir / "pool.txt").string().c_str());
        std::exit(1);
    }
    VarintPool pool;
    double total_bytes = 0;
    for (long i = 0; i < n; ++i) {
        pool.buffers.push_back(read_payload((pool_dir / (std::to_string(i) + ".bin")).string()));
        total_bytes += static_cast<double>(pool.buffers.back().size());
    }
    pool.avg_bytes = total_bytes / static_cast<double>(n);
    return pool;
}

// Write `buffers` as the pool <pool_dir>/<i>.bin, replacing what a previous dump left there, and its
// pool.txt last, so an interrupted dump leaves no loadable pool.
inline bool write_pool(const std::filesystem::path& pool_dir,
                       const std::vector<std::string>& buffers) {
    std::error_code ec;
    std::filesystem::remove_all(pool_dir, ec);
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        if (!write_payload(pool_dir / (std::to_string(i) + ".bin"), buffers[i])) {
            return false;
        }
    }
    return write_payload(pool_dir / "pool.txt", std::to_string(buffers.size()) + "\n");
}

// Write the varint-sweep pool `stem` -- (dist, count, field_number) -- to <dir>/rv/<stem>/.
inline bool dump_varint_pool(const char* dir, const std::string& stem, const VarintDist& dist,
                             int count, int field_number = 1) {
    return write_pool(rv_pool_path(dir, stem),
                      build_varint_pool(dist, count, field_number).buffers);
}

}  // namespace rpbench
