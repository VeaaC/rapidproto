// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The Rust generator's request frame (see rapidproto/rustgen/frame.hpp): decode it, resolve and
// analyze the schema it carries, and run the shared generation tail.

#include "rapidproto/rustgen/frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/rustgen/generator.hpp"  // RustModels
#include "rapidproto/rustgen/pipeline.hpp"
#include "rapidproto/source.hpp"

namespace rapidproto::rustgen {
namespace {

constexpr unsigned kBitsPerByte = 8;
constexpr std::size_t kU32Bytes = 4;  // width of a little-endian u32 (and of each length prefix)

// A forward cursor over the frame. Every read is bounds-checked against the end, so a truncated or
// hostile frame fails cleanly rather than reading out of bounds. The first failure sticks: later
// reads return empty values without advancing, and the caller checks ok() before using anything.
// Holds a view of the input (which outlives it) and reads by index, so there is no raw pointer
// arithmetic.
class FrameReader {
public:
    explicit FrameReader(std::string_view data) : m_data(data) {}

    void expect_magic() {
        static constexpr std::string_view kMagic = "RPWF3\n";
        // substr clamps to what is present, so a short input simply compares unequal -- no throw.
        if (m_data.substr(m_pos, kMagic.size()) != kMagic) {
            fail("input is not a RPWF3 frame");
            return;
        }
        m_pos += kMagic.size();
    }

    [[nodiscard]] std::uint32_t u32() {
        if (!ok() || m_data.size() - m_pos < kU32Bytes) {
            fail("truncated frame: expected a 4-byte length");
            return 0;
        }
        // Little-endian, byte by byte -- independent of the host's endianness.
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < kU32Bytes; ++i) {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(m_data[m_pos + i]))
                     << (kBitsPerByte * i);
        }
        m_pos += kU32Bytes;
        return value;
    }

    [[nodiscard]] std::uint8_t u8() {
        if (!ok() || m_data.size() - m_pos < 1) {
            fail("truncated frame: expected a byte");
            return 0;
        }
        return static_cast<std::uint8_t>(m_data[m_pos++]);
    }

    [[nodiscard]] std::string bytes() {
        const std::uint32_t len = u32();
        if (!ok() || m_data.size() - m_pos < len) {
            fail("truncated frame: declared length runs past the input");
            return {};
        }
        std::string out(m_data.substr(m_pos, len));
        m_pos += len;
        return out;
    }

    void expect_end() {
        if (ok() && m_pos != m_data.size()) {
            fail("trailing bytes after the frame");
        }
    }

    // Bytes not yet consumed. Used to cap a count-driven reserve() at what the input could possibly
    // hold, so a hostile count cannot turn a tiny frame into a huge (aborting) allocation.
    [[nodiscard]] std::size_t remaining() const { return m_data.size() - m_pos; }

    [[nodiscard]] bool ok() const { return m_error.empty(); }
    [[nodiscard]] const std::string& error() const { return m_error; }

private:
    void fail(std::string message) {
        if (ok()) {
            m_error = std::move(message);
        }
    }

    std::string_view m_data;
    std::size_t m_pos = 0;
    std::string m_error;
};

// The exported macros' crate-root names embed the prefix, so it must be an identifier.
bool is_export_prefix(std::string_view prefix) {
    const auto alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return !prefix.empty() && alpha(prefix.front()) &&
           std::all_of(prefix.begin(), prefix.end(), [&](char c) { return alpha(c) || digit(c); });
}

FrameResult failure(FrameStatus status, std::string message) {
    return FrameResult{status, std::move(message)};
}

// A decoded frame.
struct Request {
    std::vector<std::string> entries;
    std::unordered_map<std::string, std::string> sources;  // name -> body (the host's superset)
    std::string modes_text;
    bool unknown_all = false;
    bool emit_arena = false;
    bool emit_stream = false;
    bool export_macros = false;
    std::string export_prefix;
};

// Decode the frame into `request`; the reader's error if it is malformed.
std::optional<std::string> read_request(std::string_view frame, Request& request) {
    FrameReader reader(frame);
    reader.expect_magic();

    const std::uint32_t n_entries = reader.u32();
    // Clamp the reserve to what the rest of the frame could hold (each entry is at least its 4-byte
    // length prefix): a hostile count alone must not drive a huge allocation. A count that overstates
    // the real data stops the loop at the first failed read.
    request.entries.reserve(std::min<std::size_t>(n_entries, reader.remaining() / kU32Bytes));
    for (std::uint32_t i = 0; i < n_entries && reader.ok(); ++i) {
        request.entries.push_back(reader.bytes());
    }

    const std::uint32_t n_files = reader.u32();
    // Each file is at least two length prefixes (name + body), so cap likewise.
    request.sources.reserve(
        std::min<std::size_t>(n_files, reader.remaining() / (kU32Bytes + kU32Bytes)));
    for (std::uint32_t i = 0; i < n_files && reader.ok(); ++i) {
        std::string name = reader.bytes();
        std::string body = reader.bytes();
        request.sources.emplace(std::move(name), std::move(body));
    }

    request.modes_text = reader.bytes();
    request.unknown_all = reader.u8() != 0;
    request.emit_arena = reader.u8() != 0;
    request.emit_stream = reader.u8() != 0;
    request.export_macros = reader.u8() != 0;
    request.export_prefix = reader.bytes();
    reader.expect_end();
    if (!reader.ok()) {
        return reader.error();
    }
    return std::nullopt;
}

// What is wrong with the request's options, if anything.
std::optional<std::string> check_options(const Request& request) {
    if (request.entries.empty()) {
        return "no entry files given";
    }
    if (!request.emit_arena && !request.emit_stream) {
        return "no decoder model selected: enable arena and/or stream";
    }
    if (!request.export_macros) {
        if (!request.export_prefix.empty()) {
            return "a stream-macro export prefix without exporting";
        }
        return std::nullopt;
    }
    if (!request.emit_stream) {
        return "exporting the stream macros needs the stream model: enable stream";
    }
    if (!is_export_prefix(request.export_prefix)) {
        return "stream-macro export prefix '" + request.export_prefix +
               "' is not an identifier ([A-Za-z_][A-Za-z0-9_]*)";
    }
    return std::nullopt;
}

// The request's field modes into `modes`; the error message if the profile is malformed or names
// something that does not exist. The `.modes` text covers the drop/raw/unknown-fields directives
// (exactly the C++ CLI's --field-modes/--drop/--raw/--unknown, which the host serializes into that
// text); the global --unknown-present rides as its own flag.
std::optional<std::string> resolve_modes(const Request& request, const ResolvedFileSet& set,
                                         const SymbolTable& symbols, arenagen::FieldModes& modes) {
    if (request.modes_text.empty() && !request.unknown_all) {
        return std::nullopt;
    }
    arenagen::FieldModesSpec spec;
    spec.unknown_all = request.unknown_all;
    if (!request.modes_text.empty()) {
        if (auto r = arenagen::parse_modes_file(request.modes_text, "<field-modes>", spec); !r) {
            return r.error().message;
        }
    }
    auto resolved = arenagen::resolve_field_modes(spec, set, symbols);
    if (!resolved) {
        return resolved.error().message;
    }
    modes = std::move(resolved).value();
    return std::nullopt;
}

FrameResult generate(const Request& request) {
    // The resolver reads every source through this map; the embedded well-known types are still
    // consulted as a fallback (the host need not carry them).
    ResolverConfig config;
    config.source_provider = [&request](const std::string& name) -> std::optional<std::string> {
        const auto it = request.sources.find(name);
        if (it == request.sources.end()) {
            return std::nullopt;
        }
        return it->second;
    };

    SourceRegistry registry;  // populated as files load, so errors render as file:line:col
    auto resolved = resolve(request.entries, config, registry);
    if (!resolved) {
        return failure(FrameStatus::kResolveError, render_error(resolved.error(), registry));
    }
    ResolvedFileSet set = std::move(resolved).value();

    auto analyzed = analyze(set);
    if (!analyzed) {
        return failure(FrameStatus::kResolveError, render_error(analyzed.error(), registry));
    }
    const SymbolTable symbols = std::move(analyzed).value();

    arenagen::FieldModes modes;
    if (auto error = resolve_modes(request, set, symbols, modes)) {
        return failure(FrameStatus::kModesError, std::move(*error));
    }

    // Plan layouts and emit the merged module tree through the shared post-analysis pipeline, the
    // exact tail native rustgen runs (see rustgen/pipeline.hpp) -- so the two cannot drift.
    const RustModels models{request.emit_arena, request.emit_stream, request.export_prefix};
    return FrameResult{FrameStatus::kOk, generate_rust(set, symbols, modes, models)};
}

}  // namespace

FrameResult run_frame(std::string_view frame) {
    Request request;
    if (auto error = read_request(frame, request)) {
        return failure(FrameStatus::kBadFrame, std::move(*error));
    }
    if (auto error = check_options(request)) {
        return failure(FrameStatus::kBadFrame, std::move(*error));
    }
    return generate(request);
}

}  // namespace rapidproto::rustgen
