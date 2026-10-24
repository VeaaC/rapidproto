// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The Rust generator's request frame: how the `rapidproto-build` crate hands it a schema -- the
// sources it can offer, the entries, the options -- and gets the generated Rust back. The crate runs
// the generator either in-process (src/rustgen/native_entry.cpp, compiled by its build script) or as
// the embedded wasm command (src/rustgen/wasm_entry.cpp); both decode the frame here, so the two
// produce the same bytes and the same errors.
//
// Frame (all lengths little-endian u32; bodies are raw bytes, so a proto's newlines/quotes need no
// escaping):
//   "RPWF3\n"                                     magic
//   u32 n_entries   { u32 len, bytes }*n_entries  entry names (the import-closure roots)
//   u32 n_files     { u32 len, name,              every source the host could offer (a superset);
//                     u32 len, body }*n_files      the resolver pulls exactly the closure it needs
//   u32 len, modes_text                           the `.modes` profile ("" = no field modes)
//   u8  unknown_all                               1 => reserve the unknown-fields bit on every message
//   u8  arena                                     1 => emit the random-access `arena` decoder model
//   u8  stream                                    1 => emit the `stream` visitor decoder model
//   u8  export_stream_macros                      1 => export the `*_stream!` macros, under:
//   u32 len, export_prefix                        the export prefix ("" when not exporting)
// At least one of arena/stream must be set; exporting needs stream and a prefix that is an identifier
// ([A-Za-z_][A-Za-z0-9_]*). This is the one place those options are validated.

#include <cstdint>
#include <string>
#include <string_view>

namespace rapidproto::rustgen {

// Distinct per failure stage, so a host can tell a malformed frame from a bad schema without parsing
// the message. The wasm command exits with these.
enum class FrameStatus : std::uint8_t {
    kOk = 0,
    kBadFrame = 2,      // not a well-formed RPWF3 frame
    kResolveError = 3,  // an entry/import was missing or the schema failed to parse/analyze
    kModesError = 4,    // the `.modes` profile was malformed or named something that does not exist
};

struct FrameResult {
    FrameStatus status = FrameStatus::kOk;
    std::string text;  // the generated Rust on kOk, else the error message
};

// Decode one frame and generate. Never exits or throws for a bad frame or schema: the failure comes
// back as a status and a message.
FrameResult run_frame(std::string_view frame);

}  // namespace rapidproto::rustgen
