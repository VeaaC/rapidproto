// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// rapidproto's Rust generator as a standalone wasm32-wasi command module: the request frame
// (rapidproto/rustgen/frame.hpp) on stdin, the generated Rust on stdout. On an error: the message on
// stderr, the frame status as the exit code, stdout empty.
//
// The blob runs the same code as native rustgen -- the frame decode and rustgen::generate_rust, the
// tail the `[rustgen-golden]` test pins -- so its output is byte-identical to native for the same
// input. A CI gate rebuilds the blob and byte-diffs it against the committed copy, and the golden
// dogfood diffs blob output against the native goldens, so the two cannot silently drift.
//
// No filesystem: wasi gives only stdin/stdout/stderr, so the host (the build crate) sends the whole
// source closure in the frame.

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

#include "rapidproto/rustgen/frame.hpp"

namespace {

std::string read_all_stdin() {
    std::string data;
    // Modest stack buffer: the wasm stack is small (see the linker's stack-size), so a large
    // on-stack read buffer would overflow it. 8 KiB keeps the read loop cheap and the stack safe.
    static constexpr std::size_t kReadChunk = 8192;
    std::array<char, kReadChunk> buffer{};
    std::size_t n = 0;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), stdin)) > 0) {
        data.append(buffer.data(), n);
    }
    return data;
}

}  // namespace

int main() {
    const rapidproto::rustgen::FrameResult result =
        rapidproto::rustgen::run_frame(read_all_stdin());
    if (result.status != rapidproto::rustgen::FrameStatus::kOk) {
        std::fwrite(result.text.data(), 1, result.text.size(), stderr);
        std::fputc('\n', stderr);
        return static_cast<int>(result.status);
    }
    std::fwrite(result.text.data(), 1, result.text.size(), stdout);
    return 0;
}
