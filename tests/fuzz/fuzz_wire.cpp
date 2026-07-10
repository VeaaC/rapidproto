// libFuzzer harness: the wire reader over arbitrary (untrusted) bytes. Drives the rapidproto::wire
// free readers -- walk every tag with read_tag_or_end and skip its value with skip_value (exercising
// the recursive group walk + length handling). ASan + UBSan catch any out-of-bounds / overflow / UB.
// Build:
//   clang++-20 -std=c++17 -O1 -g -Iinclude -fsanitize=fuzzer,address,undefined \
//     tests/fuzz/fuzz_wire.cpp -o build/fuzz_wire
#include <cstddef>
#include <cstdint>

#include "rapidproto/runtime.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace wire = rapidproto::wire;
    const rapidproto::ByteView input(reinterpret_cast<const char*>(data), size);

    const std::uint8_t* p = wire::byte_ptr(input);
    const std::uint8_t* const begin = p;
    const std::uint8_t* const end = p + size;
    rapidproto::WireError err = rapidproto::WireError::None;
    while (true) {
        rapidproto::Tag tag{};
        wire::TagState state = wire::TagState::End;
        p = wire::read_tag_or_end(p, end, &tag, &err, &state);
        if (state != wire::TagState::Tag) {
            break;
        }
        std::size_t fail_off = 0;
        p = wire::skip_value(p, end, begin, tag, 0, &err, &fail_off);
        if (p == nullptr) {
            break;
        }
    }
    return 0;
}
