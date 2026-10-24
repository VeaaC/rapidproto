// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// rapidproto's Rust generator as a C ABI, for the `rapidproto-build` crate to call in-process when
// its build script compiled the generator natively: the request frame (rapidproto/rustgen/frame.hpp)
// in, the generated Rust or an error message out -- what the wasm command (wasm_entry.cpp) puts on
// stdout and stderr.

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include "rapidproto/rustgen/frame.hpp"

namespace {

// The status reported for a failure inside the generator rather than in the schema, as the wasm
// host reports a trap.
constexpr int kInternalError = -1;

}  // namespace

extern "C" {

// Receives the output: `context` as the caller passed it, and `len` bytes at `data`, valid only for
// the duration of the call.
using RapidprotoRustgenSink = void (*)(void* context, const char* data, std::size_t len);

// Generate from the `frame_len`-byte frame at `frame`, handing the generated Rust -- or the error
// message -- to `sink` exactly once before returning. Returns 0 on success, a
// rapidproto::rustgen::FrameStatus for a bad frame or schema, -1 for an internal failure (an
// exception, which never crosses this boundary).
int rapidproto_rustgen_run(const char* frame, std::size_t frame_len, RapidprotoRustgenSink sink,
                           void* context) noexcept {
    int status = kInternalError;
    std::string text;
    try {
        rapidproto::rustgen::FrameResult result =
            rapidproto::rustgen::run_frame(std::string_view(frame, frame_len));
        status = static_cast<int>(result.status);
        text = std::move(result.text);
    } catch (const std::exception& e) {
        text = e.what();
    } catch (...) {
        text = "unknown exception";
    }
    sink(context, text.data(), text.size());
    return status;
}

}  // extern "C"
