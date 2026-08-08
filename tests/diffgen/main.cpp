// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// Emits the C++ half of the randomized differential test (tests/differential.py): a program that
// decodes payloads with the generated arena decoder and prints each decoded tree through the debug
// dumper, one compact dump per line. The Python half generates the payloads with protobuf, and
// compares those dumps against protobuf's own JSON for the same bytes.
//
// TEST-ONLY -- deliberately not part of rapidprotoc. Its whole job is to know, for every message in
// a schema, the exact C++ type name the arena header declared. It gets that from the SAME
// CppNameTable the generators use rather than re-deriving it from the proto name, which is where a
// hand-rolled harness would break: `sanitize()` escapes keywords, and a name colliding with its
// own class or a sibling picks up `_` suffixes that no naming convention predicts.
//
// It also writes a small JSON sidecar (--meta) telling the Python side two things it cannot derive
// on its own: which message FQNs the harness can decode, and -- for every enum in the resolved set
// -- the exact text the dumper prints per value. That text is the C++ ENUMERATOR the generated enum
// class declares (the proto value name prefix-stripped where the enum allows it, sanitized, then
// deduped within the enum), computed HERE by the same codegen helper the dumper's own table uses, so
// the comparison never re-derives those names and cannot drift from them.
//
// Usage: rapidproto_diffgen -I<dir> --out <harness.cpp> --meta <meta.json> <schema.proto>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/cli/driver.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

namespace {

// Every message in `file`, nested ones included, paired with its resolved FQN. The FQN is what the
// Python side names a message by, since that is what protobuf's descriptors use.
void collect(const rapidproto::MessageNode& message,
             std::vector<const rapidproto::MessageNode*>& out) {
    out.push_back(&message);
    for (const rapidproto::MessageNode& nested : message.nested_messages) {
        collect(nested, out);
    }
}

// Every enum defined in `file`, including those nested inside messages.
void collect_enums(const rapidproto::MessageNode& message,
                   std::vector<const rapidproto::EnumNode*>& out) {
    for (const rapidproto::EnumNode& node : message.enums) {
        out.push_back(&node);
    }
    for (const rapidproto::MessageNode& nested : message.nested_messages) {
        collect_enums(nested, out);
    }
}

// A JSON string body: the sidecar carries proto identifiers and enum value names, so only the quote
// and backslash can realistically appear, but escape the control range too rather than emit
// something a JSON parser would reject.
std::string json_escaped(std::string_view text) {
    std::string out;
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (static_cast<unsigned char>(ch) < 0x20) {
            static constexpr std::string_view kHex = "0123456789abcdef";
            out += "\\u00";
            out += kHex[(static_cast<unsigned char>(ch) >> 4U) & 0xFU];
            out += kHex[static_cast<unsigned char>(ch) & 0xFU];
        } else {
            out += ch;
        }
    }
    return out;
}

// The schema's file stem ("dir/proto3.proto" -> "proto3"), which names its generated headers.
std::string stem_of(const std::string& filename) {
    std::string stem = filename;
    const auto slash = stem.find_last_of('/');
    if (slash != std::string::npos) {
        stem = stem.substr(slash + 1);
    }
    const std::string kProto = ".proto";
    if (stem.size() >= kProto.size() &&
        stem.compare(stem.size() - kProto.size(), kProto.size(), kProto) == 0) {
        stem = stem.substr(0, stem.size() - kProto.size());
    }
    return stem;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> includes;
    std::string out_path;
    std::string meta_path;
    std::string entry;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            includes.push_back(arg.substr(2));
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--meta" && i + 1 < argc) {
            meta_path = argv[++i];
        } else if (arg.rfind('-', 0) == 0) {
            std::cerr << "error: unknown flag '" << arg << "'\n";
            return 2;
        } else {
            entry = arg;
        }
    }
    if (entry.empty() || out_path.empty() || meta_path.empty()) {
        std::cerr << "usage: rapidproto_diffgen -I<dir> --out <harness.cpp> --meta <meta.json>"
                     " <schema.proto>\n";
        return 2;
    }

    rapidproto::ResolverConfig config;
    config.include_paths = includes;
    auto analyzed = rapidproto::cli::resolve_and_analyze({entry}, config);
    if (!analyzed) {
        return 1;
    }
    const rapidproto::ResolvedFileSet& set = analyzed->first;
    if (set.files.empty()) {
        std::cerr << "error: " << entry << " resolved to no files\n";
        return 1;
    }
    const rapidproto::codegen::CppNameTable names =
        rapidproto::codegen::build_cpp_names(set.files.front(), set.files, "", "");

    // The ENTRY file is the last in the resolved set (imports come first), matching how the CLI
    // picks the file to emit for.
    const rapidproto::FileNode& file = set.files.back();
    std::vector<const rapidproto::MessageNode*> messages;
    for (const rapidproto::MessageNode& message : file.messages) {
        collect(message, messages);
    }

    std::string out;
    out += "// Generated by rapidproto_diffgen. DO NOT EDIT.\n";
    out += "#include <cstdint>\n#include <fstream>\n#include <iostream>\n#include <string>\n";
    out += "#include <vector>\n\n";
    out += "#include \"rapidproto/arena_runtime.hpp\"\n";
    out += "#include \"" + stem_of(file.filename) + ".rp.hpp\"\n";
    out += "#include \"" + stem_of(file.filename) + ".rp.dump.hpp\"\n\n";
    out += R"(namespace {

// Wide enough that every dump stays on ONE line, so the Python side reads one message per line.
constexpr std::size_t kOneLine = 1u << 30U;

// Far above any payload this test generates, far below a length that would exhaust memory.
constexpr std::size_t kMaxPayload = 64u << 20U;

// Payloads are length-delimited (4-byte little-endian length, then that many bytes), which keeps a
// payload holding newlines or NULs from being mistaken for a record boundary.
std::vector<std::string> read_payloads(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> payloads;
    while (true) {
        unsigned char header[4];
        if (!in.read(reinterpret_cast<char*>(header), sizeof header)) {
            break;
        }
        std::size_t size = 0;
        for (std::size_t i = 0; i < sizeof header; ++i) {
            size |= static_cast<std::size_t>(header[i]) << (i * 8U);
        }
        // Bounded before allocating: the length comes from a file, and a corrupt one saying 4 GB
        // must fail as a short read rather than by throwing bad_alloc out of the harness.
        if (size > kMaxPayload) {
            std::cerr << "harness: payload length " << size << " exceeds the cap\n";
            break;
        }
        std::string payload(size, '\0');
        if (size != 0 && !in.read(payload.data(), static_cast<std::streamsize>(size))) {
            break;
        }
        payloads.push_back(std::move(payload));
    }
    return payloads;
}

// Decode each payload and print its dump. A payload the decoder REJECTS prints the `!decode-failed`
// marker rather than aborting: protobuf accepted these bytes when it produced them, so a rejection
// is itself a difference the Python side reports.
template <class T>
void run(const std::vector<std::string>& payloads) {
    for (const std::string& payload : payloads) {
        ::rapidproto::Arena arena;
        const T* message = T::decode(::rapidproto::ByteView(payload), arena);
        if (message == nullptr) {
            std::cout << "!decode-failed\n";
            continue;
        }
        std::cout << rp_dump_string(*message, kOneLine) << '\n';  // found by ADL on T
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: harness <message-fqn> <payload-file>\n";
        return 2;
    }
    const std::string which = argv[1];
    const std::vector<std::string> payloads = read_payloads(argv[2]);
)";
    for (const rapidproto::MessageNode* message : messages) {
        out += "    if (which == \"" + message->fqn + "\") {\n";
        out += "        run<" + rapidproto::codegen::cpp_type_name(names, message->fqn) +
               ">(payloads);\n";
        out += "        return 0;\n    }\n";
    }
    out += "    std::cerr << \"harness: no decoder for \" << which << '\\n';\n";
    out += "    return 2;\n}\n";

    // Enums from EVERY resolved file, not just the entry: a field can reference an enum declared in
    // an import, and the dumper renders it by name there too. Collecting only the entry's would
    // leave the Python side unable to resolve that name -- reported as a field mismatch, when
    // nothing about the decode is wrong.
    std::vector<const rapidproto::EnumNode*> enums;
    for (const rapidproto::FileNode& each : set.files) {
        for (const rapidproto::EnumNode& node : each.enums) {
            enums.push_back(&node);
        }
        for (const rapidproto::MessageNode& message : each.messages) {
            collect_enums(message, enums);
        }
    }

    // The dumper prints the C++ enumerator name, which codegen::enum_value_names computes -- the
    // same call the enum and the dumper's own table make, so this table is what a dump contains.
    std::string meta = "{\n  \"messages\": [";
    std::string sep;
    for (const rapidproto::MessageNode* message : messages) {
        meta += sep + "\"" + json_escaped(message->fqn) + "\"";
        sep = ", ";
    }
    meta += "],\n  \"enums\": {\n";
    sep.clear();
    for (const rapidproto::EnumNode* node : enums) {
        const std::vector<std::string> value_names = rapidproto::codegen::enum_value_names(*node);
        meta += sep + "    \"" + json_escaped(node->fqn) + "\": {";
        std::string value_sep;
        // Keyed by NUMBER, so an `allow_alias` enum records only the FIRST name for a number --
        // which is the one the dumper's switch returns, since its cases are emitted in this order.
        std::unordered_set<std::int32_t> seen;
        for (std::size_t i = 0; i < node->values.size(); ++i) {
            if (!seen.insert(node->values[i].number).second) {
                continue;
            }
            meta += value_sep + "\"" + std::to_string(node->values[i].number) + "\": \"" +
                    json_escaped(value_names[i]) + "\"";
            value_sep = ", ";
        }
        meta += "}";
        sep = ",\n";
    }
    meta += "\n  }\n}\n";
    std::ofstream meta_out(meta_path, std::ios::binary);
    if (!meta_out) {
        std::cerr << "error: cannot write " << meta_path << '\n';
        return 1;
    }
    meta_out << meta;

    std::ofstream file_out(out_path, std::ios::binary);
    if (!file_out) {
        std::cerr << "error: cannot write " << out_path << '\n';
        return 1;
    }
    file_out << out;
    return file_out.good() ? 0 : 1;
}
