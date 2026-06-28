// libFuzzer harness: streaming read() over arbitrary (untrusted) bytes, with a catch-all callback for
// known fields and an unknown-field callback. Exercises the wire walk + the compile-time dispatch on a
// rich proto3 message. ASan + UBSan catch any out-of-bounds / overflow / UB. Build:
//   clang++-20 -std=c++17 -O1 -g -Iinclude -Itests -fsanitize=fuzzer,address,undefined \
//     tests/fuzz/fuzz_stream.cpp -o build/fuzz_stream
#include <cstddef>
#include <cstdint>

#include "streamgen_golden/proto3.rp.stream.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const rapidproto::ByteView input(reinterpret_cast<const char*>(data), size);
    volatile std::size_t sink = 0;
    (void)p3::stream::Msg{input}.decode(
        [&](auto /*tag*/, auto&& /*value*/) { sink += 1; },              // catch-all (known fields)
        [&](rapidproto::UnknownField uf) { sink += uf.bytes.size(); });  // not-in-schema fields
    return 0;
}
