// libFuzzer harness: arena decode() over arbitrary (untrusted) bytes, into a fresh Arena. Exercises the
// full materializing decode path (allocation, strings, repeated, maps, oneofs, recursion, the depth
// guard) of a rich proto3 message; on success it touches a few accessors to exercise the read side.
// ASan + UBSan catch any out-of-bounds / overflow / leak / UB. Build:
//   clang++-20 -std=c++17 -O1 -g -Iinclude -Itests -fsanitize=fuzzer,address,undefined \
//     tests/fuzz/fuzz_arena.cpp -o build/fuzz_arena
#include <cstddef>
#include <cstdint>

#include "arenagen_golden/proto3.rp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    rapidproto::Arena arena;
    const rapidproto::ByteView input(reinterpret_cast<const char*>(data), size);
    rapidproto::ArenaDecodeError err;
    const p3::Msg* msg = p3::Msg::decode(input, arena, &err);
    if (msg != nullptr) {
        volatile std::size_t sink = 0;
        sink += msg->name().size();
        for (const std::int32_t v : msg->nums()) {
            sink += static_cast<std::size_t>(v);
        }
        if (const auto self =
                msg->self()) {  // MessageRef: truthy when present, deref like a pointer
            sink += self->name().size();
        }
    }
    return 0;
}
