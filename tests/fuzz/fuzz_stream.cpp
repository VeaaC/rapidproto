// libFuzzer harness: streaming decode() over arbitrary (untrusted) bytes, through THREE generated
// decoders: the rich proto3 message (catch-all + unknown-field handler over the top-level walk),
// the wire-exhaustive message (group / fixed / packed arms), and the editions message decoded
// RECURSIVELY -- the sub-decoder path a real consumer runs on nested data, capped by a harness
// depth counter (unbounded LEN nesting would exhaust the native stack -- recursion happens in
// consumer code, so bounding it is the consumer's responsibility, not a decoder bug). ASan + UBSan catch any out-of-bounds / overflow /
// UB. Build:
//   clang++-20 -std=c++17 -O1 -g -Iinclude -Itests -fsanitize=fuzzer,address,undefined \
//     tests/fuzz/fuzz_stream.cpp -o build/fuzz/fuzz_stream
#include <cstddef>
#include <cstdint>

#include "streamgen_golden/editions2023.rp.stream.hpp"
#include "streamgen_golden/proto3.rp.stream.hpp"
#include "streamgen_golden/wire_all.rp.stream.hpp"

namespace {

constexpr int kMaxWalkDepth = 32;

// Recurse into both message-typed fields (LEN-framed `child`, group-framed `delim`); past the
// depth cap the sub-decoder is left undecoded (its bytes were already validated by the framing).
rapidproto::DecodeStatus walk(ed23::stream::M m, int depth, volatile std::size_t& sink) {
    if (depth >= kMaxWalkDepth) {
        return rapidproto::DecodeStatus::success();
    }
    return m.decode(
        [&](ed23::stream::M::child, ed23::stream::M sub) { return walk(sub, depth + 1, sink); },
        [&](ed23::stream::M::delim, ed23::stream::M sub) { return walk(sub, depth + 1, sink); },
        [&](auto /*tag*/, auto&& /*value*/) { sink += 1; },
        [&](rapidproto::UnknownField uf) { sink += uf.bytes.size(); });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const rapidproto::ByteView input(reinterpret_cast<const char*>(data), size);
    volatile std::size_t sink = 0;
    (void)p3::stream::Msg{input}.decode(
        [&](auto /*tag*/, auto&& /*value*/) { sink += 1; },              // catch-all (known fields)
        [&](rapidproto::UnknownField uf) { sink += uf.bytes.size(); });  // not-in-schema fields
    (void)wire::stream::AllWire{input}.decode(
        [&](auto /*tag*/, auto&& /*value*/) { sink += 1; },
        [&](rapidproto::UnknownField uf) { sink += uf.bytes.size(); });
    (void)walk(ed23::stream::M{input}, 0, sink);
    return 0;
}
