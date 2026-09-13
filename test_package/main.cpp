// Conan test_package: prove find_package(rapidproto) + rapidproto_generate() work against the
// packaged tool -- generate a decoder for point.proto, decode a buffer, check the fields.
#include <cstdio>

#include "point.rp.hpp"
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

int main() {
    // tp.Point{ x: 3, y: 4 }:  field 1 (varint) 0x08 0x03, field 2 (varint) 0x10 0x04
    const unsigned char wire[] = {0x08, 0x03, 0x10, 0x04};
    rapidproto::Arena arena;
    const auto* pt = rp::arena::tp::Point::decode(
        rapidproto::ByteView(reinterpret_cast<const char*>(wire), sizeof wire), arena);
    if (pt == nullptr || pt->x() != 3 || pt->y() != 4) {
        std::puts("FAIL: point did not decode as {3, 4}");
        return 1;
    }
    std::puts("ok: decoded tp.Point{3, 4}");
    return 0;
}
