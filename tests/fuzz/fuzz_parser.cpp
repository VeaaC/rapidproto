// libFuzzer harness: the schema FRONT-END over arbitrary bytes -- lexer, parser, and the semantic
// passes (editions features, FQN computation, type resolution, decode-relevant option
// interpretation). The three decode targets beside this one leave the whole front-end unexercised,
// yet it is the half that runs on a developer's machine over hand-written and generated files.
//
// This is a ROBUSTNESS bar, not a trust boundary: a schema is trusted input (SECURITY.md puts a
// malicious `.proto` out of scope, unlike wire bytes), but "never crash or invoke UB on any input"
// still applies -- a malformed schema must be a clean diagnostic, never a crash.
//
// Filesystem-free by construction: the bytes are treated as one already-read file, so no import
// resolution runs. That path is I/O and path handling rather than parsing, and driving it would make
// every input depend on what happens to be on disk.
//
// Build:
//   clang++-20 -std=c++17 -O1 -g -Iinclude -fsanitize=fuzzer,address,undefined \
//     tests/fuzz/fuzz_parser.cpp src/lexer.cpp src/parser.cpp src/features.cpp src/resolve.cpp \
//     src/interpret.cpp src/source.cpp src/resolver.cpp src/wellknown_generated.cpp \
//     -o build/fuzz/fuzz_parser
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string source(reinterpret_cast<const char*>(data), size);

    auto lexed = rapidproto::lex(std::move(source));
    if (lexed.is_err()) {
        return 0;
    }
    const rapidproto::LexResult tokens = std::move(lexed).value();

    auto parsed = rapidproto::parse_file(rapidproto::Range<rapidproto::Token>(tokens.tokens));
    if (parsed.is_err()) {
        return 0;
    }
    // Trailing tokens mean the file is not fully parsed -- rapidprotoc rejects that, so the semantic
    // passes never see such an AST and neither should this.
    if (!parsed.value().remaining.empty()) {
        return 0;
    }

    // The semantic passes run over a file SET, so wrap the one parsed file in a set the way the
    // resolver would have. `analyze` mutates the AST in place (features, FQNs, resolved type
    // references), which is the part most worth driving with hostile input.
    rapidproto::ResolvedFileSet set;
    set.files.push_back(std::move(parsed).value().value);
    set.files.front().filename = "fuzz.proto";
    set.file_index.emplace(set.files.front().filename, 0);
    auto analyzed = rapidproto::analyze(set);
    if (analyzed.is_err()) {
        return 0;
    }
    return 0;
}
