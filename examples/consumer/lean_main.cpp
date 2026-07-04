// Consumer smoke test for rapidproto_generate(FIELD_MODES): the SAME schema as main.cpp, decoded
// through the `lean` profile (examples/consumer/lean.modes) -- demo.Shape.sides is DROPPED (no
// storage, no accessor: reading it would not compile) and demo.Shape.origin is RAW (the
// sub-message's payload lands as an arena-copied ByteView instead of a materialized Point). The
// view is exactly what Point::decode() accepts, so the tree is built only when -- and if -- the
// consumer asks: the deferred-decode pattern the profile exists for. The profile also stamps the
// generated header: the types live in an inline rp_modes_* namespace (reachable as demo::Shape),
// so a TU generated under a DIFFERENT profile cannot silently exchange trees with this one -- it
// fails to link.

#include <cstdio>

#include "message.rp.hpp"  // arena decoder, generated UNDER the lean profile
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

int main() {
    // The same wire bytes as main.cpp: demo.Shape{ name: "hi", origin: {3,4}, kind: KIND_CIRCLE,
    // sides: [3,4,5] } plus an unknown field 99. The dropped sides and the unknown field are
    // both skip-validated; name/kind materialize; origin lands as its payload bytes.
    const unsigned char buf[] = {0x0A, 0x02, 'h',  'i',  0x12, 0x04, 0x08, 0x03, 0x10, 0x04,
                                 0x18, 0x01, 0x22, 0x03, 0x03, 0x04, 0x05, 0x98, 0x06, 0x2A};
    const rapidproto::ByteView bytes = rapidproto::byte_view(buf, sizeof(buf));

    rapidproto::Arena arena;
    const demo::Shape* shape = demo::Shape::decode(bytes, arena);
    if (shape == nullptr) {
        std::fprintf(stderr, "lean consumer: arena decode failed\n");
        return 1;
    }
    // shape->sides() does not exist under this profile; shape->origin() is the Point payload
    // (0x08 0x03 0x10 0x04), owned by the arena -- `buf` may go away.
    if (!shape->origin().has_value()) {
        std::fprintf(stderr, "lean consumer: origin payload missing\n");
        return 1;
    }

    // Deferred decode: materialize the Point only now, straight from the stored payload.
    const demo::Point* origin = demo::Point::decode(*shape->origin(), arena);
    if (origin == nullptr) {
        std::fprintf(stderr, "lean consumer: deferred Point decode failed\n");
        return 1;
    }

    if (shape->name() != "hi" || shape->kind() != demo::Kind::CIRCLE ||
        shape->origin()->size() != 4 || origin->x() != 3 || origin->y() != 4) {
        std::fprintf(stderr, "lean consumer: decoded values disagree\n");
        return 1;
    }
    std::puts("lean consumer: dropped sides skipped, raw origin decoded on demand (3,4)");
    return 0;
}
