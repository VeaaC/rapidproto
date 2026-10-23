// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The shared payload directory: the bytes both C++ benches and the Rust benches
// (rust/rapidproto-bench) decode, so a Rust and a C++ number are measured on byte-identical input.
// Each C++ bench's dump mode (RAPIDPROTO_BENCH_DUMP_DIR=<dir>) writes every payload it measures and
// exits; RAPIDPROTO_BENCH_DIR=<dir> makes a run load them instead of building them in-process. Layout:
//   <dir>/<name>.bin           an arena scenario, protoc-serialized (bench_arena.cpp)
//   <dir>/stream/<stem>.bin    a streaming scenario (bench_streamgen.cpp)
//   <dir>/rv/<stem>/<i>.bin    buffer i of a varint-sweep pool (both benches; bench_varint.hpp)
// The Rust side is decode-only: it builds none of these, so the files are its whole contract.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

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
    if (ec || !file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        std::fprintf(stderr, "bench: cannot write '%s'\n", path.string().c_str());
        return false;
    }
    return true;
}

// A varint-sweep pool's directory under the payload root.
inline std::filesystem::path rv_pool_path(const char* dir, const std::string& stem) {
    return std::filesystem::path(dir) / "rv" / stem;
}

// The varint-sweep pool `stem` -- (dist, count, field_number) -- loaded from <dir>/rv/<stem>/ when
// RAPIDPROTO_BENCH_DIR is set, else built.
inline VarintPool load_varint_pool(const std::string& stem, const VarintDist& dist, int count,
                                   int field_number = 1) {
    const char* const dir = payload_dir();
    if (dir == nullptr) {
        return build_varint_pool(dist, count, field_number);
    }
    VarintPool pool;
    double total_bytes = 0;
    for (int i = 0;; ++i) {
        const std::filesystem::path path = rv_pool_path(dir, stem) / (std::to_string(i) + ".bin");
        if (!std::filesystem::exists(path)) {
            break;
        }
        pool.buffers.push_back(read_payload(path.string()));
        total_bytes += static_cast<double>(pool.buffers.back().size());
    }
    if (pool.buffers.empty()) {
        std::fprintf(stderr, "bench: no pool buffers under '%s'\n",
                     rv_pool_path(dir, stem).string().c_str());
        std::exit(1);
    }
    pool.avg_bytes = total_bytes / static_cast<double>(pool.buffers.size());
    return pool;
}

// Write the pool `stem` -- (dist, count, field_number) -- to <dir>/rv/<stem>/<i>.bin, replacing what
// a previous dump left there (a smaller pool must not keep stale higher-index buffers).
inline bool dump_varint_pool(const char* dir, const std::string& stem, const VarintDist& dist,
                             int count, int field_number = 1) {
    const std::filesystem::path pool_dir = rv_pool_path(dir, stem);
    std::error_code ec;
    std::filesystem::remove_all(pool_dir, ec);
    const VarintPool pool = build_varint_pool(dist, count, field_number);
    for (std::size_t i = 0; i < pool.buffers.size(); ++i) {
        if (!write_payload(pool_dir / (std::to_string(i) + ".bin"), pool.buffers[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace rpbench
