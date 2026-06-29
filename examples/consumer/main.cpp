// Consumer smoke test for rapidproto_generate(GENERATOR both): decode the SAME bytes two ways in ONE
// translation unit -- the arena object-tree decoder (demo::Shape) and the streaming callback decoder
// (demo::stream::Shape) -- proving the two models coexist and SHARE one enum type (demo::Kind, from the
// common header message.rp.common.hpp). This is the in-tree mirror of what a downstream
// find_package(rapidproto) / FetchContent consumer writes.

#include <cstdio>
#include <string_view>
#include <type_traits>
#include <utility>

#include "message.rp.hpp"  // arena: demo::Shape / demo::Point (pulls types.rp.hpp + the common)
#include "message.rp.stream.hpp"  // streaming: demo::stream::Shape (pulls the same common header)
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

// The schema's enum is ONE C++ type shared by both models (it lives in message.rp.common.hpp, not in
// either decoder): the arena accessor returns it and the streaming callback delivers it, both demo::Kind.
static_assert(std::is_same_v<decltype(std::declval<const demo::Shape&>().kind()), demo::Kind>);
static_assert(std::is_same_v<demo::stream::Shape::kind::Value, demo::Kind>);

int main() {
    // Wire bytes for demo.Shape{ name: "hi", origin: { x: 3, y: 4 }, kind: KIND_CIRCLE }:
    //   field 1 (name, len):    0x0A 0x02 'h' 'i'
    //   field 2 (origin, msg):  0x12 0x04  [0x08 0x03  0x10 0x04]
    //   field 3 (kind, varint): 0x18 0x01
    const unsigned char buf[] = {0x0A, 0x02, 'h',  'i',  0x12, 0x04,
                                 0x08, 0x03, 0x10, 0x04, 0x18, 0x01};
    const rapidproto::ByteView bytes(reinterpret_cast<const char*>(buf), sizeof(buf));

    // (1) Arena model: materialize the whole tree (including the cross-file Shape -> Point reference).
    rapidproto::Arena arena;
    rapidproto::ArenaDecodeError err{};
    const demo::Shape* shape = demo::Shape::decode(bytes, arena, &err);
    if (shape == nullptr) {
        std::fprintf(stderr, "consumer: arena decode failed\n");
        return 1;
    }

    // (2) Streaming model: walk the SAME bytes with callbacks -- in the same TU as the arena types.
    std::string_view stream_name;
    demo::Kind stream_kind = demo::Kind::UNKNOWN;
    const rapidproto::DecodeStatus status = demo::stream::Shape{bytes}.decode(
        [&](demo::stream::Shape::name, std::string_view v) { stream_name = v; },
        [&](demo::stream::Shape::kind, demo::Kind v) { stream_kind = v; });
    if (!status.ok()) {
        std::fprintf(stderr, "consumer: stream decode failed\n");
        return 1;
    }

    // Both models decoded the same bytes to the same values and agree on the shared enum.
    const bool ok = shape->name() == "hi" && shape->origin() != nullptr &&
                    shape->origin()->x() == 3 && shape->origin()->y() == 4 &&
                    shape->kind() == demo::Kind::CIRCLE && stream_name == shape->name() &&
                    stream_kind == shape->kind();
    if (!ok) {
        std::fprintf(stderr, "consumer: arena/streaming values disagree\n");
        return 1;
    }
    std::puts(
        "consumer: arena + streaming decoded demo.Shape consistently (one TU, shared demo::Kind)");
    return 0;
}
