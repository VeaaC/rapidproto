// libFuzzer harness: arena decode() over arbitrary (untrusted) bytes, into a fresh Arena, through
// FOUR generated decoders whose arms differ most: the rich proto3 message (strings, repeated, maps,
// oneofs, recursion, the depth guard), the wire-exhaustive message (groups, fixed32/64, packed),
// the >64-required-fields message (the multi-word transient required mask), and the
// --unknown-present variant (the per-message unknown bit, incl. its bool-wrapper inlining). On a
// successful decode a few accessors exercise the read side. ASan + UBSan catch any out-of-bounds /
// overflow / leak / UB. Build:
//   clang++-20 -std=c++17 -O1 -g -Iinclude -Itests -fsanitize=fuzzer,address,undefined \
//     tests/fuzz/fuzz_arena.cpp -o build/fuzz/fuzz_arena
#include <cstddef>
#include <cstdint>

#include "arenagen_golden/arena_manyreq.rp.hpp"
#include "arenagen_golden/arena_unknown.rp.hpp"
#include "arenagen_golden/proto3.rp.hpp"
#include "arenagen_golden/wire_all.rp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const rapidproto::ByteView input(reinterpret_cast<const char*>(data), size);
    volatile std::size_t sink = 0;
    {
        rapidproto::Arena arena;
        rapidproto::ArenaDecodeError err;
        if (const p3::Msg* msg = p3::Msg::decode(input, arena, &err)) {
            sink += msg->name().size();
            for (const std::int32_t v : msg->nums()) {
                sink += static_cast<std::size_t>(v);
            }
            if (const p3::Msg* self = msg->self()) {
                sink += self->name().size();
            }
        }
    }
    {
        rapidproto::Arena arena;
        if (const wire::AllWire* w = wire::AllWire::decode(input, arena)) {
            sink += static_cast<std::size_t>(w->fx().value_or(0));
            sink += w->packed().size();
            if (const wire::AllWire* nested = w->nested()) {
                sink += static_cast<std::size_t>(nested->zz().value_or(0));
            }
        }
    }
    {
        rapidproto::Arena arena;
        if (const mr::ManyRequired* r = mr::ManyRequired::decode(input, arena)) {
            sink += static_cast<std::size_t>(r->f1());
        }
    }
    {
        rapidproto::Arena arena;
        if (const au::Holder* h = au::Holder::decode(input, arena)) {
            sink += static_cast<std::size_t>(h->has_unknown_fields());
        }
    }
    return 0;
}
