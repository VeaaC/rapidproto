// Consumer smoke test for rapidproto_generate(GENERATOR both): decode the SAME bytes two ways in ONE
// translation unit -- the arena object-tree decoder (demo::Shape) and the streaming callback decoder
// (demo::stream::Shape) -- proving the two models coexist and SHARE one enum type (demo::Kind, from the
// common header message.rp.common.hpp). Along the way it shows the API surfaces a real consumer
// uses: byte_view() over a raw buffer, a repeated field read both ways, an unknown-field handler
// (forward compatibility), and the arena error diagnostics on malformed input. This is the in-tree
// mirror of what a downstream find_package(rapidproto) / FetchContent consumer writes.

#include <cstdint>
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
    // Wire bytes for demo.Shape{ name: "hi", origin: { x: 3, y: 4 }, kind: KIND_CIRCLE,
    // sides: [3, 4, 5] }, plus a field number 99 that is NOT in the schema (a newer producer):
    //   field 1 (name, len):      0x0A 0x02 'h' 'i'
    //   field 2 (origin, msg):    0x12 0x04  [0x08 0x03  0x10 0x04]
    //   field 3 (kind, varint):   0x18 0x01
    //   field 4 (sides, packed):  0x22 0x03  [0x03 0x04 0x05]
    //   field 99 (unknown):       0x98 0x06 0x2A            (varint 42)
    const unsigned char buf[] = {0x0A, 0x02, 'h',  'i',  0x12, 0x04, 0x08, 0x03, 0x10, 0x04,
                                 0x18, 0x01, 0x22, 0x03, 0x03, 0x04, 0x05, 0x98, 0x06, 0x2A};
    // byte_view(): a ByteView over a uint8_t-family buffer, no manual cast.
    const rapidproto::ByteView bytes = rapidproto::byte_view(buf, sizeof(buf));

    // (1) Arena model: materialize the whole tree (including the cross-file Shape -> Point
    // reference). The unknown field 99 is skipped and dropped.
    rapidproto::Arena arena;
    rapidproto::ArenaDecodeError err{};
    const demo::Shape* shape = demo::Shape::decode(bytes, arena, &err);
    if (shape == nullptr) {
        std::fprintf(stderr, "consumer: arena decode failed\n");
        return 1;
    }
    int side_sum = 0;
    for (const std::int32_t s : shape->sides()) {  // repeated: a contiguous ArrayView
        side_sum += s;
    }

    // (2) Streaming model: walk the SAME bytes with callbacks -- in the same TU as the arena types.
    // `sides` fires once per element; the one-argument handler receives fields NOT in the schema.
    std::string_view stream_name;
    demo::Kind stream_kind = demo::Kind::UNKNOWN;
    int stream_side_sum = 0;
    unsigned unknown_seen = 0;
    const demo::Point* hybrid_origin = nullptr;  // materialized from within the streaming walk
    const rapidproto::DecodeStatus status = demo::stream::Shape{bytes}.decode(
        [&](demo::stream::Shape::name, std::string_view v) { stream_name = v; },
        [&](demo::stream::Shape::kind, demo::Kind v) { stream_kind = v; },
        [&](demo::stream::Shape::sides, std::int32_t v) { stream_side_sum += v; },
        // Hybrid: rp_bytes() is the sub-message's exact field bytes, so the ARENA model can
        // materialize just this field mid-stream. The resulting tree borrows those bytes (a slice of
        // `bytes`), so it stays valid only while `bytes` and `arena` outlive it -- both do here.
        [&](demo::stream::Shape::origin, demo::stream::Point p) {
            hybrid_origin = demo::Point::decode(p.rp_bytes(), arena);
        },
        [&](rapidproto::UnknownField uf) {
            unknown_seen += static_cast<unsigned>(uf.field_number == 99);
        });
    if (!status.ok()) {
        std::fprintf(stderr, "consumer: stream decode failed\n");
        return 1;
    }

    // (3) Malformed input fails CLEANLY with a diagnostic, never UB: truncate mid-record and read
    // the error's code and byte offset (the pattern a real consumer logs).
    rapidproto::Arena scratch;
    rapidproto::ArenaDecodeError bad{};
    if (demo::Shape::decode(bytes.substr(0, 5), scratch, &bad) != nullptr ||
        bad.code != rapidproto::ArenaDecodeError::Code::Wire) {
        std::fprintf(stderr, "consumer: truncated input was not rejected as a wire error\n");
        return 1;
    }
    std::printf("consumer: truncated input rejected (wire error at byte offset %zu)\n", bad.offset);

    // Both models decoded the same bytes to the same values and agree on the shared enum.
    const bool ok = shape->name() == "hi" && shape->origin() && shape->origin()->x() == 3 &&
                    shape->origin()->y() == 4 && shape->kind() == demo::Kind::CIRCLE &&
                    side_sum == 12 && stream_name == shape->name() &&
                    stream_kind == shape->kind() && stream_side_sum == side_sum &&
                    unknown_seen == 1 && hybrid_origin != nullptr && hybrid_origin->x() == 3 &&
                    hybrid_origin->y() == 4;
    if (!ok) {
        std::fprintf(stderr, "consumer: arena/streaming values disagree\n");
        return 1;
    }
    std::puts(
        "consumer: arena + streaming decoded demo.Shape consistently (one TU, shared demo::Kind)");
    return 0;
}
