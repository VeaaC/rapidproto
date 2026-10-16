// rapidproto's Rust code-generator front-end, compiled to a standalone wasm32-wasi command module.
//
// The native `rustgen` library and the wasm blob are ONE codebase: this entry drives the same
// resolve -> analyze -> plan_layouts(modes) -> rustgen::generate_file pipeline the C++ CLI drives
// (src/rapidprotoc/main.cpp), so the blob's output is byte-identical to native rustgen for the same
// input -- the `rapidproto-build` crate ships the blob and never needs a C++ toolchain. A CI gate
// rebuilds the blob and byte-diffs it against the committed copy; a second check diffs blob output
// against native, so the two can never silently drift.
//
// No filesystem: wasi gives us only stdin/stdout/stderr, so the host (the build crate) feeds the
// whole source closure in on stdin and the resolver reads it through an in-memory provider
// (ResolverConfig::source_provider). Field-modes parity with the C++ CLI is kept by carrying the
// profile as its `.modes` text (parsed in-blob via arenagen::parse_modes_file) plus the global
// "unknown fields present" flag -- the same raw/drop/unknown knobs native exposes.
//
// Stdin frame (all lengths little-endian u32; bodies are raw bytes, so a proto's newlines/quotes
// need no escaping):
//   "RPWF1\n"                                     magic
//   u32 n_entries   { u32 len, bytes }*n_entries  entry names (the import-closure roots)
//   u32 n_files     { u32 len, name,              every source the host could offer (a superset);
//                     u32 len, body }*n_files      the resolver pulls exactly the closure it needs
//   u32 len, modes_text                           the `.modes` profile ("" = no field modes)
//   u8  unknown_all                               1 => reserve the unknown-fields bit on every message
// Stdout: the generated Rust. On any error: a message on stderr and a nonzero exit; stdout stays empty.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/rustgen/generator.hpp"
#include "rapidproto/source.hpp"

namespace {

using namespace rapidproto;

// Exit codes: distinct per failure stage so a host (or a test) can tell a malformed frame from a bad
// schema without parsing stderr.
enum ExitCode {
    kOk = 0,
    kBadFrame = 2,      // stdin was not a well-formed RPWF1 frame
    kResolveError = 3,  // an entry/import was missing or the schema failed to parse/analyze
    kModesError = 4,    // the `.modes` profile was malformed or named something that does not exist
};

[[noreturn]] void fail(ExitCode code, const std::string& message) {
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
    std::exit(code);
}

// A forward cursor over the stdin frame. Every read is bounds-checked against the buffer end, so a
// truncated or hostile frame fails cleanly as kBadFrame rather than reading out of bounds.
class FrameReader {
public:
    explicit FrameReader(const std::string& data) : m_data(data) {}

    void expect_magic() {
        static constexpr char kMagic[] = {'R', 'P', 'W', 'F', '1', '\n'};
        if (m_data.size() - m_pos < sizeof(kMagic) ||
            std::memcmp(m_data.data() + m_pos, kMagic, sizeof(kMagic)) != 0) {
            fail(kBadFrame, "input is not a RPWF1 frame");
        }
        m_pos += sizeof(kMagic);
    }

    std::uint32_t u32() {
        if (m_data.size() - m_pos < 4) {
            fail(kBadFrame, "truncated frame: expected a 4-byte length");
        }
        // Little-endian, byte by byte -- independent of the host's endianness.
        const auto* p = reinterpret_cast<const unsigned char*>(m_data.data() + m_pos);
        const std::uint32_t value =
            static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
            (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        m_pos += 4;
        return value;
    }

    std::uint8_t u8() {
        if (m_data.size() - m_pos < 1) {
            fail(kBadFrame, "truncated frame: expected a byte");
        }
        return static_cast<std::uint8_t>(m_data[m_pos++]);
    }

    std::string bytes() {
        const std::uint32_t len = u32();
        if (m_data.size() - m_pos < len) {
            fail(kBadFrame, "truncated frame: declared length runs past the input");
        }
        std::string out = m_data.substr(m_pos, len);
        m_pos += len;
        return out;
    }

    void expect_end() {
        if (m_pos != m_data.size()) {
            fail(kBadFrame, "trailing bytes after the frame");
        }
    }

private:
    const std::string& m_data;
    std::size_t m_pos = 0;
};

std::string read_all_stdin() {
    std::string data;
    // Modest stack buffer: the wasm stack is small (see the linker's stack-size), so a large
    // on-stack read buffer would overflow it. 8 KiB keeps the read loop cheap and the stack safe.
    char buffer[8192];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        data.append(buffer, n);
    }
    return data;
}

}  // namespace

int main() {
    const std::string input = read_all_stdin();
    FrameReader frame(input);
    frame.expect_magic();

    std::vector<std::string> entries;
    const std::uint32_t n_entries = frame.u32();
    entries.reserve(n_entries);
    for (std::uint32_t i = 0; i < n_entries; ++i) {
        entries.push_back(frame.bytes());
    }

    std::unordered_map<std::string, std::string> sources;  // name -> body (the host's superset)
    const std::uint32_t n_files = frame.u32();
    sources.reserve(n_files);
    for (std::uint32_t i = 0; i < n_files; ++i) {
        std::string name = frame.bytes();
        std::string body = frame.bytes();
        sources.emplace(std::move(name), std::move(body));
    }

    const std::string modes_text = frame.bytes();
    const bool unknown_all = frame.u8() != 0;
    frame.expect_end();

    if (entries.empty()) {
        fail(kBadFrame, "no entry files given");
    }

    // The resolver reads every source through this map; the embedded well-known types are still
    // consulted as a fallback (the host need not carry them).
    ResolverConfig config;
    config.source_provider = [&sources](const std::string& name) -> std::optional<std::string> {
        const auto it = sources.find(name);
        if (it == sources.end()) {
            return std::nullopt;
        }
        return it->second;
    };

    SourceRegistry registry;  // populated as files load, so errors render as file:line:col
    auto resolved = resolve(entries, config, registry);
    if (!resolved) {
        fail(kResolveError, render_error(resolved.error(), registry));
    }
    ResolvedFileSet set = std::move(resolved).value();

    auto analyzed = analyze(set);
    if (!analyzed) {
        fail(kResolveError, render_error(analyzed.error(), registry));
    }
    const SymbolTable symbols = std::move(analyzed).value();

    // Field modes: the `.modes` text covers the drop/raw/unknown-fields directives (exactly the C++
    // CLI's --field-modes/--drop/--raw/--unknown, which the host serializes into that text); the
    // global --unknown-present rides as its own flag.
    arenagen::FieldModes modes;
    const bool modes_requested = !modes_text.empty() || unknown_all;
    if (modes_requested) {
        arenagen::FieldModesSpec spec;
        spec.unknown_all = unknown_all;
        if (!modes_text.empty()) {
            if (auto r = arenagen::parse_modes_file(modes_text, "<field-modes>", spec); !r) {
                fail(kModesError, r.error().message);
            }
        }
        auto resolved_modes = arenagen::resolve_field_modes(spec, set, symbols);
        if (!resolved_modes) {
            fail(kModesError, resolved_modes.error().message);
        }
        modes = std::move(resolved_modes).value();
    }

    arenagen::LayoutOptions layout_options;
    layout_options.modes = &modes;  // the Rust decoder shares the arena storage-layout planner
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, layout_options);

    // ONE file for the whole import closure: every file in the set contributes its types to the
    // single merged module tree (matches the CLI's --rust=<file> Option A).
    std::vector<const FileNode*> files;
    files.reserve(set.files.size());
    for (const FileNode& file : set.files) {
        files.push_back(&file);
    }
    const std::string out = rustgen::generate_file(files, layouts, modes);
    std::fwrite(out.data(), 1, out.size(), stdout);
    return kOk;
}
