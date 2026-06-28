// libFuzzer harness: the wire reader over arbitrary (untrusted) bytes. Walks every tag and skips its
// value (exercising the group walk + length handling), then runs the read_message collector. ASan +
// UBSan catch any out-of-bounds / overflow / UB. Build:
//   clang++-20 -std=c++17 -O1 -g -Iinclude -fsanitize=fuzzer,address,undefined \
//     tests/fuzz/fuzz_wire.cpp -o build/fuzz_wire
#include <cstddef>
#include <cstdint>

#include "rapidproto/runtime.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const rapidproto::ByteView input(reinterpret_cast<const char*>(data), size);

    rapidproto::WireReader reader{input};
    while (!reader.at_end()) {
        const auto tag = reader.read_tag();
        if (!tag) {
            break;
        }
        if (!reader.skip(tag->wire_type, tag->field_number)) {
            break;
        }
    }

    rapidproto::WireError err = rapidproto::WireError::None;
    (void)rapidproto::read_message(input, &err);
    return 0;
}
