// Isolated decode micro-benchmark, built as the standalone `rapidproto_bench` executable at
// -O3 -DNDEBUG, NOT linked into the Catch2 test binary. Why standalone: measuring decoders inside
// the large (3.5MB+) test binary makes results layout-sensitive; an unrelated code change can
// swing even protozero's (unchanged) numbers by 30%+. A small dedicated binary is layout-stable and
// fast to rebuild while iterating on the wire layer.
//
// The harness self-pins to the core it starts on and reaches steady-state frequency before timing.
// On a hybrid (P/E-core) CPU still launch it on a performance core so it does not pin to an E-core:
//   taskset -c 0 ./build/gcc/rapidproto_bench
//
// Each scenario decodes a large buffer that stresses one decode path (focused) or several at once
// (mixed), comparing three decoders over the same bytes:
//   generated: the rapidproto-generated decoder (p2::stream::Scalars; the product)
//   wire:      a hand loop on the rapidproto::wire primitives, feature-equivalent (dispatch + validate)
//   protozero: mapbox protozero, an established minimal-overhead pull parser (yardstick; its
//                wire-type checks are protozero_assert()s compiled out under NDEBUG, so it validates
//                marginally less than we do).
// Methodology lives in bench_harness.hpp: each arm is measured against the baseline (arm 0) as a
// frequency-drift-invariant cost ratio, sampled adaptively, and reported with a three-way
// significance verdict; cycles/byte where the kernel permits it. All
// decoders must agree on a checksum (guards correctness and stops the loops being optimized away);
// a mismatch is reported and makes the process exit non-zero.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "bench_harness.hpp"  // rpbench: the shared measurement harness (also used by the arena bench)
#include "bench_two_tier.hpp"  // micro-vs-large-TU decode (out-of-line-primitive penalty)
#include "proto2.rp.stream.hpp"  // generated p2::stream::Scalars; -Itests/streamgen_golden (pulls runtime.hpp)
#include "rapidproto/runtime.hpp"

#if __has_include(<protozero/pbf_reader.hpp>)
#include <protozero/pbf_reader.hpp>
#define RAPIDPROTO_HAVE_PROTOZERO 1
#endif

using namespace rapidproto;  // NOLINT(google-build-using-namespace): bench convenience

namespace {

using Fn = std::uint64_t (*)(ByteView);
struct Arm {
    const char* label;
    Fn fn;
};

// Encoder-side buffer builders (the decoders under test are the SUT, never these).
void put_varint(std::string& b, std::uint64_t v) {
    while (v >= 0x80U) {
        b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
        v >>= 7U;
    }
    b.push_back(static_cast<char>(v));
}
void put_tag(std::string& b, std::uint32_t field, std::uint32_t wire) {
    put_varint(b, (static_cast<std::uint64_t>(field) << 3U) | wire);
}
void put_fixed32(std::string& b, std::uint32_t v) {
    for (unsigned i = 0; i < 4U; ++i) {
        b.push_back(static_cast<char>((v >> (8U * i)) & 0xFFU));
    }
}
void put_fixed64(std::string& b, std::uint64_t v) {
    for (unsigned i = 0; i < 8U; ++i) {
        b.push_back(static_cast<char>((v >> (8U * i)) & 0xFFU));
    }
}
std::uint64_t zigzag64(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1U) ^ static_cast<std::uint64_t>(v >> 63);
}
std::uint64_t double_bits(double d) {
    std::uint64_t b = 0;
    std::memcpy(&b, &d, sizeof b);
    return b;
}
std::uint64_t float_bits(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

// Adapter: the scenarios below build arms as pure Fn(ByteView); the shared harness takes nullary
// closures (so it can also drive stateful decoders, e.g. the arena bench). Bind the buffer here.
template <std::size_t N>
int run(const char* scenario, ByteView bytes, const Arm (&arms)[N]) {
    std::vector<rpbench::Arm> wrapped;
    wrapped.reserve(N);
    for (const auto& a : arms) {
        wrapped.push_back({a.label, [fn = a.fn, bytes]() { return fn(bytes); }});
    }
    return rpbench::run(scenario, static_cast<double>(bytes.size()), wrapped);
}

constexpr int kN = 2'000'000;

// Shared wire/protozero arms reused by the simple single-handled-field scenarios. Each scenario
// differs only in which field/type it builds and decodes, so the arm bodies are written per case.

int scenario_varint_1byte() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, static_cast<std::uint64_t>(i) & 0x7FU);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 1 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(v));
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("varint-1byte", ByteView(buf), arms);
}

int scenario_varint_multibyte() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, 0xF000'0000ULL + (static_cast<std::uint64_t>(i) & 0xFFFFU));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 1 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(v));
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("varint-multibyte", ByteView(buf), arms);
}

int scenario_zigzag() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        const std::int64_t v = static_cast<std::int64_t>(i) * ((i & 1) != 0 ? -1003 : 1003);
        put_tag(buf, 6, 0);
        put_varint(buf, zigzag64(v));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::s64, std::int64_t v) {
                 s += static_cast<std::uint64_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 6 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint64_t>(zigzag_decode_64(v));
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 6) {
                     s += static_cast<std::uint64_t>(r.get_sint64());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("zigzag-sint64", ByteView(buf), arms);
}

int scenario_fixed32() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 14, 5);
        put_fixed32(buf, float_bits(static_cast<float>(i) * 1.5F));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::fl, float v) { s += float_bits(v); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 14 && t.wire_type == WireType::I32) {
                     std::uint32_t v = 0;
                     p = wire::read_fixed32(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += v;
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 14) {
                     s += float_bits(r.get_float());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("fixed32-float", ByteView(buf), arms);
}

int scenario_fixed64() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 15, 1);
        put_fixed64(buf, double_bits(static_cast<double>(i) * -2.25));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::db, double v) { s += double_bits(v); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 15 && t.wire_type == WireType::I64) {
                     std::uint64_t v = 0;
                     p = wire::read_fixed64(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += v;
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 15) {
                     s += double_bits(r.get_double());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("fixed64-double", ByteView(buf), arms);
}

int scenario_string() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 12, 2);
        put_varint(buf, 8);
        buf.append("abcdefgh", 8);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::s, std::string_view v) { s += v.size(); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 12 && t.wire_type == WireType::Len) {
                     ByteView v;
                     p = wire::read_length_delimited(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += v.size();
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 12) {
                     s += r.get_view().size();
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("len-string", ByteView(buf), arms);
}

int scenario_packed() {
    std::string payload;
    for (int i = 0; i < kN; ++i) {
        put_varint(payload, static_cast<std::uint64_t>(i) & 0x3FFFFU);
    }
    std::string buf;
    put_tag(buf, 17, 2);
    put_varint(buf, payload.size());
    buf += payload;
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::packed_nums, std::int32_t v) {
                     s += static_cast<std::uint32_t>(v);
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 17 && t.wire_type == WireType::Len) {
                     ByteView span;
                     p = wire::read_length_delimited(p, end, &span, &we);
                     if (p == nullptr) {
                         break;
                     }
                     const std::uint8_t* ip = wire::byte_ptr(span);
                     const std::uint8_t* const iend = ip + span.size();
                     while (ip < iend) {
                         std::uint64_t v = 0;
                         ip = wire::read_varint(ip, iend, &v, &we);
                         if (ip == nullptr) {
                             break;
                         }
                         s += static_cast<std::uint32_t>(varint_to_int32(v));
                     }
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 17) {
                     auto packed = r.get_packed_int32();
                     for (auto v : packed) {
                         s += static_cast<std::uint32_t>(v);
                     }
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("packed-int32", ByteView(buf), arms);
}

int scenario_skip_heavy() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 11, 0);  // field 11 (bool), no callback so it gets skipped
        put_varint(buf, static_cast<std::uint64_t>(i) & 1U);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 1 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(v));
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("skip-heavy", ByteView(buf), arms);
}

int scenario_mixed() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 6, 0);
        put_varint(buf, zigzag64(static_cast<std::int64_t>(i) * ((i & 1) != 0 ? -7 : 7)));
        put_tag(buf, 15, 1);
        put_fixed64(buf, double_bits(static_cast<double>(i) * 0.5));
        put_tag(buf, 12, 2);
        put_varint(buf, 4);
        buf.append("wxyz", 4);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::i32, std::int32_t v) {
                     s += static_cast<std::uint32_t>(v);
                 },
                 [&](p2::stream::Scalars::s64, std::int64_t v) {
                     s += static_cast<std::uint64_t>(v);
                 },
                 [&](p2::stream::Scalars::db, double v) { s += double_bits(v); },
                 [&](p2::stream::Scalars::s, std::string_view v) { s += v.size(); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 1 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(v));
                 } else if (t.field_number == 6 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint64_t>(zigzag_decode_64(v));
                 } else if (t.field_number == 15 && t.wire_type == WireType::I64) {
                     std::uint64_t v = 0;
                     p = wire::read_fixed64(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += v;
                 } else if (t.field_number == 12 && t.wire_type == WireType::Len) {
                     ByteView v;
                     p = wire::read_length_delimited(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += v.size();
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 switch (r.tag()) {
                     case 1:
                         s += static_cast<std::uint32_t>(r.get_int32());
                         break;
                     case 6:
                         s += static_cast<std::uint64_t>(r.get_sint64());
                         break;
                     case 15:
                         s += double_bits(r.get_double());
                         break;
                     case 12:
                         s += r.get_view().size();
                         break;
                     default:
                         r.skip();
                         break;
                 }
             }
             return s;
         }},
#endif
    };
    return run("mixed", ByteView(buf), arms);
}

// --- multi-byte tags: field 18 (expanded_nums) -> tag is 2 bytes (0x90 0x01). Real schemas have
// field numbers >15 constantly; the 1-byte-tag micro-scenarios are an unrealistic best case. ------
int scenario_multibyte_tag() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 18, 0);  // (18<<3)|0 = 144 -> 0x90 0x01
        put_varint(buf, static_cast<std::uint64_t>(i) & 0x7FU);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::expanded_nums, std::int32_t v) {
                     s += static_cast<std::uint32_t>(v);
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 18 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(v));
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 18) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("multibyte-tag", ByteView(buf), arms);
}

// --- nested sub-messages: Container{ repeated Nested items = 5; Nested{ int32 x = 1; } }. Exercises
// the real streaming path -- read a LEN payload, construct a sub-decoder, recurse. -----------------
int scenario_nested() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        std::string nested;
        put_tag(nested, 1, 0);  // Nested::x
        put_varint(nested, static_cast<std::uint64_t>(i) & 0xFFFFU);
        put_tag(buf, 5, 2);  // Container::items (LEN)
        put_varint(buf, nested.size());
        buf += nested;
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Container{b}.decode(
                 [&](p2::stream::Container::items, p2::stream::Container::Nested sub) {
                     (void)sub.decode([&](p2::stream::Container::Nested::x, std::int32_t v) {
                         s += static_cast<std::uint32_t>(v);
                     });
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st0 = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st0);
                 if (st0 != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 5 && t.wire_type == WireType::Len) {
                     ByteView span;
                     p = wire::read_length_delimited(p, end, &span, &we);
                     if (p == nullptr) {
                         break;
                     }
                     const std::uint8_t* sp = wire::byte_ptr(span);
                     const std::uint8_t* const sbeg = sp;
                     const std::uint8_t* const send = sp + span.size();
                     while (true) {
                         Tag t2{};
                         wire::TagState st = wire::TagState::End;
                         sp = wire::read_tag_or_end(sp, send, &t2, &we, &st);
                         if (st != wire::TagState::Tag) {
                             break;
                         }
                         if (t2.field_number == 1 && t2.wire_type == WireType::Varint) {
                             std::uint64_t v = 0;
                             sp = wire::read_varint(sp, send, &v, &we);
                             if (sp == nullptr) {
                                 break;
                             }
                             s += static_cast<std::uint32_t>(varint_to_int32(v));
                         } else {
                             std::size_t fo = 0;
                             sp = wire::skip_value(sp, send, sbeg, t2, 0, &we, &fo);
                             if (sp == nullptr) {
                                 break;
                             }
                         }
                     }
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 5) {
                     auto sub = r.get_message();
                     while (sub.next()) {
                         if (sub.tag() == 1) {
                             s += static_cast<std::uint32_t>(sub.get_int32());
                         } else {
                             sub.skip();
                         }
                     }
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("nested-msg", ByteView(buf), arms);
}

// --- groups: WithGroup{ group MyGroup = 1 { int32 a = 2; } }. Exercises the recursive group-decode
// path (SGROUP..EGROUP, scan_group_end). protozero does NOT support groups (it throws on wire type
// 3/4), so this scenario has no protozero arm -- generated vs wire only. -------------------------
int scenario_groups() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 3);  // SGROUP, field 1 -> (1<<3)|3 = 0x0b
        put_tag(buf, 2, 0);  // MyGroup::a
        put_varint(buf, static_cast<std::uint64_t>(i) & 0xFFFFU);
        put_tag(buf, 1, 4);  // EGROUP, field 1 -> (1<<3)|4 = 0x0c
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::WithGroup{b}.decode(
                 [&](p2::stream::WithGroup::mygroup, p2::stream::WithGroup::MyGroup sub) {
                     (void)sub.decode([&](p2::stream::WithGroup::MyGroup::a, std::int32_t v) {
                         s += static_cast<std::uint32_t>(v);
                     });
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st0 = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st0);
                 if (st0 != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 1 && t.wire_type == WireType::SGroup) {
                     ByteView body;
                     std::size_t gfo = 0;
                     p = wire::read_group(p, end, beg, t.field_number, &body, &we, &gfo);
                     if (p == nullptr) {
                         break;
                     }
                     const std::uint8_t* sp = wire::byte_ptr(body);
                     const std::uint8_t* const sbeg = sp;
                     const std::uint8_t* const send = sp + body.size();
                     while (true) {
                         Tag t2{};
                         wire::TagState st = wire::TagState::End;
                         sp = wire::read_tag_or_end(sp, send, &t2, &we, &st);
                         if (st != wire::TagState::Tag) {
                             break;
                         }
                         if (t2.field_number == 2 && t2.wire_type == WireType::Varint) {
                             std::uint64_t v = 0;
                             sp = wire::read_varint(sp, send, &v, &we);
                             if (sp == nullptr) {
                                 break;
                             }
                             s += static_cast<std::uint32_t>(varint_to_int32(v));
                         } else {
                             std::size_t fo = 0;
                             sp = wire::skip_value(sp, send, sbeg, t2, 0, &we, &fo);
                             if (sp == nullptr) {
                                 break;
                             }
                         }
                     }
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
    };
    return run("groups", ByteView(buf), arms);
}

// --- sparse: each record has one handled field (i32) + three unhandled fields of different wire
// types (varint, fixed64, len), so the skip dispatch over wire types is genuinely hot. -----------
int scenario_sparse() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);  // i32 (handled)
        put_varint(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 2, 0);  // i64 (skipped, varint)
        put_varint(buf, static_cast<std::uint64_t>(i) * 3U);
        put_tag(buf, 8, 1);  // f64 (skipped, fixed64)
        put_fixed64(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 12, 2);  // s (skipped, len)
        put_varint(buf, 4);
        buf.append("abcd", 4);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             const std::uint8_t* p = wire::byte_ptr(b);
             const std::uint8_t* const beg = p;
             const std::uint8_t* const end = p + b.size();
             WireError we = WireError::None;
             while (true) {
                 Tag t{};
                 wire::TagState st = wire::TagState::End;
                 p = wire::read_tag_or_end(p, end, &t, &we, &st);
                 if (st != wire::TagState::Tag) {
                     break;
                 }
                 if (t.field_number == 1 && t.wire_type == WireType::Varint) {
                     std::uint64_t v = 0;
                     p = wire::read_varint(p, end, &v, &we);
                     if (p == nullptr) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(v));
                 } else {
                     std::size_t fo = 0;
                     p = wire::skip_value(p, end, beg, t, 0, &we, &fo);
                     if (p == nullptr) {
                         break;
                     }
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("sparse-skip", ByteView(buf), arms);
}

}  // namespace

// The comprehensive Scalars decode compiled IN CONTEXT -- amid this large bench TU's many decoders,
// so the compiler keeps read_varint & co. out-of-line and calls them per element. The identical
// decode compiled ALONE (rp_bench_decode_scalars_micro, bench_stream_isolated.cpp) inlines them.
RP_BENCH_DEFINE_SCALARS_DECODE(rp_bench_decode_scalars_ctx)
RP_BENCH_DEFINE_MBTAG_DECODE(rp_bench_decode_mbtag_ctx)
extern std::uint64_t rp_bench_decode_scalars_micro(rapidproto::ByteView);  // the isolated-TU twins
extern std::uint64_t rp_bench_decode_mbtag_micro(rapidproto::ByteView);

namespace {

// Two-tier: the SAME generated decode, isolated (micro, primitives inlined) vs in-context (large TU,
// primitives out-of-line). The ratio is the out-of-line-primitive penalty a real large program pays.
int scenario_two_tier() {
    int bad = 0;
    const std::string buf = rp_bench_scalars_buffer(kN);
    const Arm arms[] = {
        {"micro (own TU)", [](ByteView b) { return rp_bench_decode_scalars_micro(b); }},
        {"large TU", [](ByteView b) { return rp_bench_decode_scalars_ctx(b); }},
    };
    bad += run("two-tier", ByteView(buf), arms);
    // Same tiers for the multi-byte-tag path: does inlining the primitives help or hurt the tag read?
    const std::string mbuf = rp_bench_mbtag_buffer(kN);
    const Arm marms[] = {
        {"micro (own TU)", [](ByteView b) { return rp_bench_decode_mbtag_micro(b); }},
        {"large TU", [](ByteView b) { return rp_bench_decode_mbtag_ctx(b); }},
    };
    bad += run("two-tier-mbtag", ByteView(mbuf), marms);
    return bad;
}

}  // namespace

int main() {
    // pin + steady-state warmup + open the cycle/instruction counters, once
    const bool perf = rpbench::metric_fds().cyc >= 0;
    if (!rpbench::json_mode()) {  // human legend; JSON mode emits only NDJSON (tests/bench.py)
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        std::puts(
            "rapidproto decode bench (generated / wire / protozero). Each arm vs the baseline:");
#else
        std::puts(
            "rapidproto decode bench (generated / wire; protozero ABSENT). Each arm vs baseline:");
#endif
        std::printf(
            "  metric: %s; adaptive, self-pinned. '+X%%' = arm out-throughputs baseline;"
            " verdict SIG=real&>=0.5%% / flat=real&<0.5%% / noise=CI spans 0."
            " ins/B is placement-invariant: differs => real code change.\n",
            perf ? "wall-clock GB/s + CPU cycles/byte + retired instructions/byte (perf)"
                 : "wall-clock GB/s (perf unavailable: kernel.perf_event_paranoid > 2)");
    }
    int bad = 0;
    bad += scenario_varint_1byte();
    bad += scenario_varint_multibyte();
    bad += scenario_zigzag();
    bad += scenario_fixed32();
    bad += scenario_fixed64();
    bad += scenario_string();
    bad += scenario_packed();
    bad += scenario_skip_heavy();
    bad += scenario_mixed();
    bad += scenario_multibyte_tag();
    bad += scenario_nested();
    bad += scenario_groups();
    bad += scenario_sparse();
    bad += scenario_two_tier();
    if (bad != 0) {
        // A mismatch is also carried per-arm as "ok":false in JSON mode; keep the exit code either way.
        if (!rpbench::json_mode()) {
            std::printf("\nFAIL: %d checksum mismatch(es)\n", bad);
        }
        return 1;
    }
    if (!rpbench::json_mode()) {
        std::puts("\nall checksums agree.");
    }
    return 0;
}
