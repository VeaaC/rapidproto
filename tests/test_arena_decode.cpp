// Decode tests for the generated arena decoders. Two kinds of oracle: (1) protoc-encoded wire
// fixtures (tests/wire_fixtures/*.bin) decoded through the generated decoder, asserting accessor
// values; (2) hand-built buffers for the behaviors fixtures don't cover -- absent-field reads (implicit
// zero defaults, explicit nullopt), required-field validation, the recursion guard, malformed input,
// single-bool wrapper fields, unknown-field drop, and the oneof reader (last-wins, unset,
// ignore-unhandled, catch-all, same-typed routing).

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstring>  // std::memcpy: build little-endian fixed-width packed test bytes
#include <fstream>
#include <initializer_list>  // packed({...}) test-vector helper
#include <ios>
#include <optional>  // std::nullopt: absent explicit-presence reads
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>  // is_same_v: the inline-namespace transparency pin
#include <utility>
#include <variant>  // std::monostate: a oneof reader's unset handler

#include "arenagen_golden/arena_manyreq.rp.hpp"
#include "arenagen_golden/arena_modes.rp.hpp"    // field modes: raw payloads + dropped fields
#include "arenagen_golden/arena_naming.rp.hpp"   // same-typed oneof members (letters { a; A; })
#include "arenagen_golden/arena_unknown.rp.hpp"  // --unknown-present + a bool-wrapper field
#include "arenagen_golden/editions2023.rp.hpp"  // editions features: presence + DELIMITED, at runtime
#include "arenagen_golden/editions2024.rp.hpp"  // 2024: decode-relevant defaults match 2023
#include "arenagen_golden/main.rp.hpp"  // cross-file imports (pulls dep/forward/pub): runtime decode
#include "arenagen_golden/proto2.rp.hpp"
#include "arenagen_golden/proto3.rp.hpp"
#include "arenagen_golden/wire_all.rp.hpp"
#include "arenagen_golden/xref.rp.hpp"  // oneof member stored as a pointer (Def -> const-ref deref)
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"  // ByteView, WireError

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

// The schema-known enum bounds ride in the shared common header (one type for both models):
// rp_known_min/max span the DECLARED values -- negatives included, aliases collapsed -- distinct
// from the INT32 rp_non_exhaustive sentinels.
static_assert(p2::Color::rp_known_min == static_cast<p2::Color>(-2));
static_assert(p2::Color::rp_known_max == p2::Color::RED);  // CRIMSON aliases 1; max is still 1
static_assert(p3::State::rp_known_min == p3::State::UNKNOWN);
static_assert(p3::State::rp_known_max == p3::State::ON);

// A profiled header's inline namespace is transparent for qualified use (fm::Holder just works)
// while making the profile part of the type identity -- mixed-profile TUs hold distinct types.
// NOTE a namespace-shape change breaks this line against the stale checked-in golden BEFORE the
// in-test regen can run: lift it temporarily, regen '[arenagen]', restore (see test_arenagen.cpp).
static_assert(std::is_same_v<fm::Holder, fm::rp_modes_lean_4ba94f51::Holder>);

namespace {

std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
std::string fixture(const std::string& name) {
    return read_file(std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/" + name);
}

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
void put_len(std::string& b, std::uint32_t field, std::string_view bytes) {
    put_tag(b, field, 2);
    put_varint(b, bytes.size());
    b.append(bytes);
}

// p3.Msg.self (field 5) wrapped `depth` times around an empty innermost Msg.
std::string nest_self(int depth) {
    std::string buf;
    for (int i = 0; i < depth; ++i) {
        std::string outer;
        put_len(outer, 5, buf);
        buf = std::move(outer);
    }
    return buf;
}

}  // namespace

TEST_CASE("arena-decode: protoc scalar fixture", "[arena-decode]") {
    const std::string bin = fixture("scalars.bin");
    Arena arena;
    ArenaDecodeError err;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(bin), arena, &err);
    REQUIRE(m != nullptr);
    CHECK(m->i32() == -7);
    CHECK(m->i64() == 42);
    CHECK(m->u32() == 300);
    CHECK(m->u64() == 1000000);
    CHECK(m->s32() == -5);
    CHECK(m->s64() == -2);
    CHECK(m->f32() == 0x01020304U);
    CHECK(m->f64() == 0x0102030405060708ULL);
    CHECK(m->sf32() == -2);
    CHECK(m->sf64() == -3);
    CHECK(m->b() == true);
    CHECK(m->s() == "hi");
    CHECK(m->by() == std::string("\x00\x01\xff", 3));
    CHECK(m->fl() == Catch::Approx(1.5F));
    CHECK(m->db() == Catch::Approx(-2.25));
    CHECK(m->i64().has_value());  // present in the fixture
}

// The single-byte-tag fast path (fields 1..15) must behave identically to the general path (16+) on
// the boundary and on malformed input. Scalars spans it: i64=2 (fast) and color=16 / packed_nums=17.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): four independent buffers in one case
TEST_CASE("arena-decode: raw-byte fast path matches the general path across the 15/16 boundary",
          "[arena-decode]") {
    const auto pv = [](std::string& b, std::uint64_t v) {
        while (v >= 0x80U) {
            b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
            v >>= 7U;
        }
        b.push_back(static_cast<char>(v));
    };
    const auto tag = [](std::uint32_t f, std::uint32_t w) {
        return (static_cast<std::uint64_t>(f) << 3U) | w;
    };
    const auto with_req = [&](const std::string& extra) {  // valid required i32 (field 1) prefix
        std::string b;
        pv(b, tag(1, 0));
        pv(b, 7);
        b += extra;
        return b;
    };

    // (1) Boundary: i64 (field 2, <=15, fast) + packed_nums (field 17, >=16, general) both decode.
    {
        std::string e;
        pv(e, tag(2, 0));
        pv(e, 99);
        pv(e, tag(17, 2));
        pv(e, 2);
        pv(e, 5);
        pv(e, 6);  // packed_nums = [5, 6]
        const std::string buf = with_req(e);
        Arena arena;
        const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        CHECK(m->i32() == 7);
        CHECK(m->i64() == 99);  // optional == value: present and equal (no deref)
        REQUIRE(m->packed_nums().size() == 2);
        CHECK(m->packed_nums()[0] == 5);
        CHECK(m->packed_nums()[1] == 6);
    }

    // (2) A known field with the wrong wire type is skipped -- consistently for the fast field (i64)
    //     and the general field (color) -- and is not an error (i32 required still satisfied).
    {
        std::string e;
        pv(e, tag(2, 2));  // i64 (fast) as LEN -- wrong wire
        pv(e, 1);
        e += "x";
        pv(e, tag(16, 2));  // color (general) as LEN -- wrong wire
        pv(e, 1);
        e += "y";
        const std::string buf = with_req(e);
        Arena arena;
        const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        CHECK_FALSE(m->i64().has_value());
        CHECK_FALSE(m->color().has_value());
    }

    // (3) A reserved wire type (6) is rejected identically whether encoded as a 1-byte (<=15) or a
    //     2-byte (>=16) tag -- both miss the fast path and fail in the validating general read. The
    //     valid required-i32 prefix (a control that decodes on its own) ensures the nullptr comes from
    //     the wire rejection, not a missing required field.
    {
        Arena ctrl;
        REQUIRE(p2::Scalars::decode(ByteView(with_req("")), ctrl) != nullptr);  // baseline succeeds
        std::string one_e;
        pv(one_e, tag(2, 6));  // 1-byte reserved-wire tag
        std::string two_e;
        pv(two_e, tag(16, 6));  // 2-byte reserved-wire tag
        const std::string one = with_req(one_e);
        const std::string two = with_req(two_e);
        Arena a1;
        Arena a2;
        CHECK(p2::Scalars::decode(ByteView(one), a1) == nullptr);
        CHECK(p2::Scalars::decode(ByteView(two), a2) == nullptr);
    }

    // (4) A non-canonical (over-long) tag of a low field is skipped, not decoded (no conformant
    //     encoder emits one; the decode never crashes). field 2 (i64, varint) as 0x90 0x00 (== 16).
    {
        std::string e;
        e.push_back('\x90');
        e.push_back('\x00');  // over-long tag: field 2, Varint
        pv(e, 123);           // the value a canonical tag would have decoded
        const std::string buf = with_req(e);
        Arena arena;
        const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        CHECK_FALSE(m->i64().has_value());  // skipped, not decoded
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat list of accessor assertions
TEST_CASE("arena-decode: submessage, repeated, map, oneof fixture", "[arena-decode]") {
    const std::string bin = fixture("msg.bin");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    CHECK(m->implicit_i() == 10);
    CHECK(m->state() == p3::State::ON);
    REQUIRE(m->self());  // sub-message via arena pointer
    CHECK(m->self()->implicit_i() == 99);
    REQUIRE(m->nums().size() == 3);  // packed repeated
    CHECK(m->nums()[0] == 1);
    CHECK(m->nums()[2] == 3);
    REQUIRE(m->unpacked().size() == 2);  // expanded repeated
    CHECK(m->unpacked()[0] == 4);
    REQUIRE(m->states().size() == 2);  // repeated enum
    bool picked_a = false;  // oneof reader: the active member dispatches to its typed handler
    m->pick(
        [&](p3::Msg::Pick::a, std::int32_t v) {
            picked_a = true;
            CHECK(v == 7);
        },
        [](auto, auto) { FAIL("unexpected oneof member"); });
    CHECK(picked_a);
    REQUIRE(m->counts().size() == 2);  // map<string,int32>
    REQUIRE(m->counts().find(std::string_view("x")) != nullptr);
    CHECK(m->counts().find(std::string_view("x"))->value() == 1);
    CHECK(m->counts().find(std::string_view("y"))->value() == 2);
}

// Packed fixed-width scalars (double = I64, fixed32 = I32) exercise the decoder's bulk-copy path
// (the wire span is the array's little-endian byte image, filled in one memcpy) and its rejection of
// a malformed span whose length is not a whole multiple of the element width.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat list of accessor assertions
TEST_CASE("arena-decode: packed fixed-width scalars (bulk-copy path)", "[arena-decode]") {
    // append the low `width` bytes of `value`, little-endian
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a local test byte-emitter
    const auto le = [](std::string& b, std::uint64_t value, int width) {
        for (int i = 0; i < width; ++i) {
            b.push_back(static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU));
        }
    };
    std::string reals;  // three doubles, exactly representable so == is safe
    for (const double d : {1.5, -2.0, 3.25}) {
        std::uint64_t bitpat = 0;
        std::memcpy(&bitpat, &d, sizeof bitpat);
        le(reals, bitpat, 8);
    }
    std::string codes;  // four fixed32
    for (const std::uint32_t c : {10U, 20U, 30U, 40U}) {
        le(codes, c, 4);
    }
    std::string buf;
    put_len(buf, 12, reals);  // Msg.reals (packed double)
    put_len(buf, 13, codes);  // Msg.codes (packed fixed32)
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    const auto bits = [](double d) {
        std::uint64_t b = 0;
        std::memcpy(&b, &d, sizeof b);
        return b;
    };
    REQUIRE(m->reals().size() == 3);
    CHECK(bits(m->reals()[0]) == bits(1.5));  // exact byte image (avoids -Wfloat-equal)
    CHECK(bits(m->reals()[1]) == bits(-2.0));
    CHECK(bits(m->reals()[2]) == bits(3.25));
    REQUIRE(m->codes().size() == 4);
    CHECK(m->codes()[0] == 10);
    CHECK(m->codes()[3] == 40);

    // A packed fixed span whose length is not a multiple of the element width is malformed -> rejected.
    std::string bad;
    put_len(bad, 13, std::string_view("\x0a\x00\x00\x00\x63", 5));  // 5 bytes, not a multiple of 4
    Arena bad_arena;
    CHECK(p3::Msg::decode(ByteView(bad), bad_arena) == nullptr);
}

// Packed VARINT arrays take the pre-size-from-wire-length + trim (shrink_last) path, an upper-bound
// reserve because a varint element is 1..10 bytes. These pin its value output across the trim, a
// re-grow on a second occurrence of the field, and the two wire forms a proto3 packed field accepts.
// Field 6 (Msg.nums) is packed int32.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat list of accessor assertions
TEST_CASE("arena-decode: packed-varint fill (pre-size / trim / re-grow / mixed forms)",
          "[arena-decode]") {
    const auto packed = [](std::initializer_list<std::uint64_t> vs) {
        std::string p;
        for (const std::uint64_t v : vs) {
            put_varint(p, v);
        }
        return p;
    };

    SECTION("empty packed span decodes to an empty array") {
        std::string buf;
        put_len(buf, 6, std::string_view{});  // nums: a zero-length LEN payload
        Arena arena;
        const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        CHECK(m->nums().empty());
    }

    SECTION("two packed occurrences re-grow the array after the first trim") {
        std::string buf;
        put_len(buf, 6, packed({1, 2}));
        put_len(buf, 6, packed({3, 4, 5}));
        Arena arena;
        const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        REQUIRE(m->nums().size() == 5);
        CHECK(m->nums()[0] == 1);
        CHECK(m->nums()[4] == 5);
    }

    SECTION("packed then expanded elements of the same field accumulate") {
        std::string buf;
        put_len(buf, 6, packed({7, 8}));  // packed form (LEN)
        put_tag(buf, 6, 0);               // then an expanded element (wire type varint)
        put_varint(buf, 9);
        Arena arena;
        const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        REQUIRE(m->nums().size() == 3);
        CHECK(m->nums()[0] == 7);
        CHECK(m->nums()[2] == 9);
    }
}

TEST_CASE("arena-decode: message-value and enum-value maps fixture", "[arena-decode]") {
    const std::string bin = fixture("container.bin");
    Arena arena;
    const p2::Container* c = p2::Container::decode(ByteView(bin), arena);
    REQUIRE(c != nullptr);
    REQUIRE(c->by_name().size() == 2);
    const auto* alpha = c->by_name().find(std::string_view("alpha"));
    REQUIRE(alpha != nullptr);
    REQUIRE(alpha->value());  // map value is a sub-message
    CHECK(alpha->value()->x() == 11);
    CHECK(c->by_name().find(std::string_view("beta"))->value()->x() == 22);
    REQUIRE(c->by_id().size() == 2);
    CHECK(c->by_id().find(1)->value() == p2::Color::RED);
    CHECK(c->by_id().find(2)->value() == p2::Color::NEG);  // negative enum
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat list of accessor assertions
TEST_CASE("arena-decode: group (delimited) fixture", "[arena-decode]") {
    const std::string bin = fixture("all_wire.bin");
    Arena arena;
    const wire::AllWire* m = wire::AllWire::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    CHECK(m->zz() == -1234567890123LL);
    CHECK(m->fx() == 305419896U);
    CHECK(m->s() == "wire");
    REQUIRE(m->nested());
    CHECK(m->nested()->zz() == 7);
    REQUIRE(m->packed().size() == 3);
    REQUIRE(m->g());  // a group sub-message
    CHECK(m->g()->a() == 99);
    bool got_oi = false;
    m->pick(
        [&](wire::AllWire::Pick::oi, std::int32_t v) {
            got_oi = true;
            CHECK(v == 5);
        },
        [](auto, auto) { FAIL("unexpected oneof member"); });
    CHECK(got_oi);
}

// An absent explicit-presence field reads as std::nullopt: the schema default (proto2 `[default=...]`)
// is NOT read through the optional accessor -- a consumer applies it via value_or(...). Only field 1
// (required i32) is set here, so every optional field is absent.
TEST_CASE("arena-decode: absent explicit fields read as nullopt", "[arena-decode]") {
    std::string buf;
    put_tag(buf, 1, 0);  // i32 (required) = 5
    put_varint(buf, 5);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(m->i32() == 5);                 // required, present
    CHECK_FALSE(m->i64().has_value());    // absent -> nullopt (schema had [default = 42])
    CHECK_FALSE(m->b().has_value());      // absent -> nullopt ([default = true])
    CHECK_FALSE(m->fl().has_value());     // absent -> nullopt ([default = 1.5])
    CHECK_FALSE(m->color().has_value());  // absent -> nullopt ([default = COLOR_RED])
    CHECK_FALSE(m->s().has_value());      // absent -> nullopt
    CHECK_FALSE(m->u32().has_value());    // absent -> nullopt
}

// Implicit-presence fields (proto3 without `optional`) have no presence bit: absent, they read back the
// zero default -- 0 / "" / the first (zero) enum value -- as a bare value (no optional). Contrast the
// explicit field, which is std::nullopt when absent.
TEST_CASE("arena-decode: absent implicit fields read their zero default", "[arena-decode]") {
    Arena arena;
    const p3::Msg* m =
        p3::Msg::decode(ByteView(std::string()), arena);  // empty message: all absent
    REQUIRE(m != nullptr);
    CHECK(m->implicit_i() == 0);               // implicit int32 -> 0
    CHECK(m->name().empty());                  // implicit string -> ""
    CHECK(m->state() == p3::State::UNKNOWN);   // implicit enum  -> first (zero) value
    CHECK(m->nums().empty());                  // absent repeated -> empty view
    CHECK_FALSE(m->explicit_i().has_value());  // explicit int32 -> nullopt (no zero read-through)
}

// A PRESENT explicit enum field is an engaged std::optional carrying the decoded value (the engaged arm
// of the optional-enum accessor; the nullopt arm is covered above).
TEST_CASE("arena-decode: present explicit enum is an engaged optional", "[arena-decode]") {
    std::string buf;
    put_tag(buf, 1, 0);  // i32 (required) = 1
    put_varint(buf, 1);
    put_tag(buf, 16, 0);  // color (optional enum) = RED (1)
    put_varint(buf, 1);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    REQUIRE(m->color().has_value());      // engaged
    CHECK(m->color() == p2::Color::RED);  // ...with the decoded value
}

TEST_CASE("arena-decode: a missing required field fails the parse", "[arena-decode]") {
    std::string buf;
    put_tag(buf, 2, 0);  // i64 only; field 1 (required i32) is absent
    put_varint(buf, 1);
    Arena arena;
    ArenaDecodeError err;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena, &err);
    CHECK(m == nullptr);
    CHECK(err.code == ArenaDecodeError::Code::MissingRequired);
    CHECK(err.field_number == 1);
}

TEST_CASE("arena-decode: malformed input fails with a wire error", "[arena-decode]") {
    const std::string buf("\x08\x80", 2);  // field 1 varint, continuation bit set then truncated
    Arena arena;
    ArenaDecodeError err;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena, &err);
    CHECK(m == nullptr);
    CHECK(err.code == ArenaDecodeError::Code::Wire);
    CHECK(err.wire == WireError::TruncatedVarint);
}

TEST_CASE("arena-decode: excessive nesting hits the recursion guard", "[arena-decode]") {
    // p3.Msg.self is a Msg; nest it well past kMaxDecodeDepth (100).
    std::string buf;  // innermost empty Msg
    for (int i = 0; i < 150; ++i) {
        std::string outer;
        put_len(outer, 5, buf);  // field 5 (self) = the inner message
        buf = std::move(outer);
    }
    Arena arena;
    ArenaDecodeError err;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena, &err);
    CHECK(m == nullptr);
    CHECK(err.code == ArenaDecodeError::Code::RecursionTooDeep);
}

TEST_CASE("arena-decode: nesting exactly at the depth limit is accepted, one past is rejected",
          "[arena-decode]") {
    SECTION("exactly at kMaxDecodeDepth parses") {
        Arena arena;
        ArenaDecodeError err;
        const p3::Msg* m = p3::Msg::decode(ByteView(nest_self(100)), arena, &err);
        CHECK(m != nullptr);
        CHECK(err.code == ArenaDecodeError::Code::None);
    }
    SECTION("one past kMaxDecodeDepth is rejected") {
        Arena arena;
        ArenaDecodeError err;
        const p3::Msg* m = p3::Msg::decode(ByteView(nest_self(101)), arena, &err);
        CHECK(m == nullptr);
        CHECK(err.code == ArenaDecodeError::Code::RecursionTooDeep);
    }
}

TEST_CASE("arena-decode: a capacity-limited arena surfaces OutOfMemory", "[arena-decode]") {
    // A cap below the root message's storage makes the first arena allocation fail; the parse then
    // returns nullptr with OutOfMemory -- the chain a real host OOM would take, otherwise untestable.
    Arena arena;
    arena.set_capacity_limit(8);
    ArenaDecodeError err;
    const p3::Msg* m = p3::Msg::decode(ByteView(""), arena, &err);
    CHECK(m == nullptr);
    CHECK(err.code == ArenaDecodeError::Code::OutOfMemory);
}

TEST_CASE("arena-decode: an unknown field is dropped", "[arena-decode]") {
    std::string buf;
    put_tag(buf, 1, 0);  // i32 (required) = 9
    put_varint(buf, 9);
    put_tag(buf, 999, 0);  // unknown field number
    put_varint(buf, 12345);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);  // unknown field skipped, not an error
    CHECK(m->i32() == 9);
}

// Decode a message whose sub-message / repeated / map-value / oneof fields are IMPORTED types from
// other files (other namespaces) -- the cross-file path the ::rapidproto::arena_detail::decode_into
// forwarder handles. Previously only compile-checked; this runs it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of cross-file assertions
TEST_CASE("arena-decode: imported (cross-file) sub-messages decode through the forwarder",
          "[arena-decode]") {
    const auto dep = [](int v) {
        std::string d;
        put_tag(d, 1, 0);  // dep.Dep field 1 (v), varint
        put_varint(d, static_cast<std::uint64_t>(v));
        return d;
    };
    std::string buf;
    put_len(buf, 1, dep(42));  // Main.d    : singular imported dep.Dep
    put_len(buf, 5, dep(7));   // Main.ds[0]: repeated imported message
    put_len(buf, 5, dep(8));   // Main.ds[1]
    std::string entry;         // Main.dm[3] = dep.Dep{99} : map<int32, imported message>
    put_tag(entry, 1, 0);
    put_varint(entry, 3);
    put_len(entry, 2, dep(99));
    put_len(buf, 6, entry);
    put_len(buf, 7, dep(5));  // Main.choice.od: oneof imported message member

    Arena arena;
    ArenaDecodeError err;
    const main::Main* m = main::Main::decode(ByteView(buf), arena, &err);
    REQUIRE(m != nullptr);
    CHECK(err.code == ArenaDecodeError::Code::None);
    REQUIRE(m->d());
    CHECK(m->d()->v() == 42);  // singular cross-file
    REQUIRE(m->ds().size() == 2);
    CHECK(m->ds()[0].v() == 7);  // repeated cross-file
    CHECK(m->ds()[1].v() == 8);
    REQUIRE(m->dm().find(3) != nullptr);
    CHECK(m->dm().find(3)->value()->v() == 99);  // map-value cross-file
    bool chose_od =
        false;  // oneof reader: a sub-message member arrives by const-ref (no null-check)
    m->choice(
        [&](main::Main::Choice::od, const dep::Dep& d) {
            chose_od = true;
            CHECK(d.v() == 5);
        },
        [](auto, auto) { FAIL("unexpected choice member"); });
    CHECK(chose_od);
}

// A nested group whose message type is a single-bool wrapper: decoded as a group, the wrapper inlined,
// reached through its const T* accessor (null when absent, the bool via ->).
TEST_CASE("arena-decode: single-bool-wrapper group field", "[arena-decode]") {
    std::string inner;  // Inner { flag (field 4, varint) = true }
    put_tag(inner, 4, 0);
    put_varint(inner, 1);
    std::string group;  // MyGroup { a (field 2) = 7; Inner (field 3, group) { ... } }
    put_tag(group, 2, 0);
    put_varint(group, 7);
    put_tag(group, 3, 3);  // SGROUP for Inner
    group += inner;
    put_tag(group, 3, 4);  // EGROUP for Inner
    std::string buf;       // WithGroup { MyGroup (field 1, group) { ... } }
    put_tag(buf, 1, 3);    // SGROUP for MyGroup
    buf += group;
    put_tag(buf, 1, 4);  // EGROUP for MyGroup

    Arena arena;
    const p2::WithGroup* w = p2::WithGroup::decode(ByteView(buf), arena);
    REQUIRE(w != nullptr);
    REQUIRE(w->mygroup());
    CHECK(w->mygroup()->a() == 7);
    CHECK(w->mygroup()->inner());          // the wrapper was present
    CHECK(w->mygroup()->inner()->flag());  // ...and its bool decoded to true
}

// >64 required fields exercise the multi-word transient required-presence mask: all present parses,
// a single omission (field 65, in the second mask word) fails.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a builder lambda + two sections
TEST_CASE("arena-decode: more than 64 required fields validate correctly", "[arena-decode]") {
    const auto build = [](int omit) {
        std::string buf;
        for (int f = 1; f <= 65; ++f) {
            if (f == omit) {
                continue;
            }
            put_tag(buf, static_cast<std::uint32_t>(f), 0);
            put_varint(buf, static_cast<std::uint64_t>(f));
        }
        return buf;
    };
    Arena arena;
    SECTION("all present") {
        const mr::ManyRequired* m = mr::ManyRequired::decode(ByteView(build(0)), arena);
        REQUIRE(m != nullptr);
        CHECK(m->f1() == 1);
        CHECK(m->f65() == 65);  // last field, second mask word
    }
    SECTION("field 65 missing") {
        ArenaDecodeError err;
        const mr::ManyRequired* m = mr::ManyRequired::decode(ByteView(build(65)), arena, &err);
        CHECK(m == nullptr);
        CHECK(err.code == ArenaDecodeError::Code::MissingRequired);
        CHECK(err.field_number == 65);
    }
}

// A singular sub-message occurring more than once is rejected (a read-only tree does not merge).
TEST_CASE("arena-decode: a repeated singular sub-message is rejected", "[arena-decode]") {
    std::string buf;
    put_len(buf, 5, std::string());  // p3.Msg.self (singular message) ...
    put_len(buf, 5, std::string());  // ...occurring twice
    Arena arena;
    ArenaDecodeError err;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena, &err);
    CHECK(m == nullptr);
    CHECK(err.code == ArenaDecodeError::Code::RepeatedSingularMessage);
    CHECK(err.field_number == 5);
}

TEST_CASE("arena-decode: oneof keeps the last member set", "[arena-decode]") {
    std::string buf;      // p3.Msg oneof pick { a=10 (int32); b=11 (string) }
    put_tag(buf, 10, 0);  // a = 1
    put_varint(buf, 1);
    put_len(buf, 11, "hi");  // b = "hi"
    put_tag(buf, 10, 0);     // a = 2  (last wins)
    put_varint(buf, 2);
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    bool picked_a = false;  // last set member wins
    m->pick(
        [&](p3::Msg::Pick::a, std::int32_t v) {
            picked_a = true;
            CHECK(v == 2);
        },
        [](auto, auto) { FAIL("unexpected oneof member"); });
    CHECK(picked_a);
}

TEST_CASE("arena-decode: oneof reader hands a pointer-stored sub-message over by const-ref",
          "[arena-decode]") {
    // xr.Nested.User.pick.chosen is a Def -- Def has a string, so it is not fixed-size and lands in the
    // union as a pointer; the reader dereferences it to a const-ref (distinct from inline storage).
    std::string def;
    put_len(def, 1, "x");  // Def.s = "x"
    put_tag(def, 2, 0);    // Def.n = 7
    put_varint(def, 7);
    std::string buf;
    put_len(buf, 4, def);  // User.pick.chosen = Def
    Arena arena;
    const xr::Nested::User* m = xr::Nested::User::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    bool chose = false;
    m->pick(
        [&](xr::Nested::User::Pick::chosen, const xr::Nested::Def& d) {
            chose = true;
            CHECK(d.s() == "x");
            CHECK(d.n() == 7);
        },
        [](auto, auto) { FAIL("unexpected oneof member"); });
    CHECK(chose);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): three decode cases, flat assertions
TEST_CASE("arena-decode: oneof reader -- unset, ignore-unhandled, catch-all", "[arena-decode]") {
    Arena arena;

    // Unset: no oneof member on the wire -> the std::monostate handler fires.
    {
        const p3::Msg* m = p3::Msg::decode(ByteView{}, arena);
        REQUIRE(m != nullptr);
        bool unset = false;
        m->pick([](p3::Msg::Pick::a, std::int32_t) { FAIL("no member was set"); },
                [&](std::monostate) { unset = true; });
        CHECK(unset);
    }

    // Ignore-unhandled: member `a` is set but only a `b` handler is given -> nothing fires, no error.
    {
        std::string buf;
        put_tag(buf, 10, 0);  // pick.a = 5
        put_varint(buf, 5);
        const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        bool fired = false;
        m->pick(
            [&](p3::Msg::Pick::b, std::string_view) { fired = true; });  // `a` omitted -> ignored
        CHECK_FALSE(fired);
    }

    // Catch-all: an unnamed member routes to the (auto, auto) handler.
    {
        std::string buf;
        put_tag(buf, 10, 0);  // pick.a = 9
        put_varint(buf, 9);
        const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        bool caught = false;
        m->pick([&](auto, auto) { caught = true; });  // `a` not named -> catch-all
        CHECK(caught);
    }
}

TEST_CASE("arena-decode: oneof reader routes same-typed members by tag", "[arena-decode]") {
    // an.Collide.letters { int32 a = 7; int32 A = 8; } -- same value type, distinct tags.
    std::string buf;
    put_tag(buf, 8, 0);  // letters.A = 42
    put_varint(buf, 42);
    Arena arena;
    const an::Collide* m = an::Collide::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    bool got_upper_a = false;
    m->letters([](an::Collide::Letters::a, std::int32_t) { FAIL("routed to lowercase a"); },
               [&](an::Collide::Letters::A, std::int32_t v) {
                   got_upper_a = true;
                   CHECK(v == 42);
               });
    CHECK(got_upper_a);
}

// A single-bool wrapper field is a normal inlined sub-message, so under --unknown-present
// it carries its own has_unknown_fields() in its own inlined mask -- like any other message field. Build
// a wrapper sub-message that contains an unknown field and confirm the wrapper reports it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): three decode cases, flat assertions
TEST_CASE("arena-decode: a bool-wrapper field reports has_unknown_fields (--unknown-present)",
          "[arena-decode]") {
    const auto build_holder = [](bool wrapper_has_unknown) {
        std::string flag;
        put_tag(flag, 1, 0);  // Flag.value = true
        put_varint(flag, 1);
        if (wrapper_has_unknown) {  // a field the wrapper's schema doesn't know -> unknown-in-wrapper
            put_tag(flag, 5, 0);
            put_varint(flag, 99);
        }
        std::string buf;
        put_len(buf, 1, flag);  // Holder.flag = <wrapper bytes>
        put_tag(buf, 2, 0);     // Holder.n = 7
        put_varint(buf, 7);
        return buf;
    };
    Arena arena;

    // (1) the wrapper carried an unknown field: the wrapper reports it; Holder itself,
    // whose own fields were all known, does not.
    const std::string with = build_holder(/*wrapper_has_unknown=*/true);
    const au::Holder* m = au::Holder::decode(ByteView(with), arena);
    REQUIRE(m != nullptr);
    CHECK(m->flag());
    CHECK(m->flag()->value());
    CHECK(m->flag()->has_unknown_fields());  // carried by the inlined wrapper's own mask
    CHECK_FALSE(m->has_unknown_fields());    // the unknown was inside the wrapper, not Holder
    CHECK(m->n() == 7);

    // (2) a clean wrapper reports no unknown fields.
    const std::string without = build_holder(/*wrapper_has_unknown=*/false);
    const au::Holder* c = au::Holder::decode(ByteView(without), arena);
    REQUIRE(c != nullptr);
    CHECK(c->flag()->value());
    CHECK_FALSE(c->flag()->has_unknown_fields());

    // (3) an unknown field at the Holder level sets Holder's own flag (not the wrapper's).
    std::string top;
    put_tag(top, 9, 0);  // field 9: unknown to Holder
    put_varint(top, 1);
    const au::Holder* h = au::Holder::decode(ByteView(top), arena);
    REQUIRE(h != nullptr);
    CHECK(h->has_unknown_fields());
    CHECK_FALSE(h->flag());

    // (4) the wrapper's OWN field number with a WRONG wire type is a known number, not an unknown field
    // -- it is skipped (its `case` arm, not the `default:`), so has_unknown_fields() stays false.
    std::string wrong;
    put_len(wrong, 1, std::string("\x01", 1));  // field 1 as length-delimited (bool expects varint)
    std::string buf;
    put_len(buf, 1, wrong);  // Holder.flag = <wrapper with field 1 mistyped>
    const au::Holder* w = au::Holder::decode(ByteView(buf), arena);
    REQUIRE(w != nullptr);
    CHECK(w->flag());
    CHECK_FALSE(w->flag()->value());
    CHECK_FALSE(w->flag()->has_unknown_fields());
}

TEST_CASE("arena-decode: editions 2023 presence features and DELIMITED encoding, from real bytes",
          "[arena-decode]") {
    // ed23::M: file-level IMPLICIT presence, per-field EXPLICIT override on explicit_scalar, and a
    // DELIMITED (group-framed) message field -- the editions constructs the compile-smoke alone
    // can't prove decode correctly.
    std::string buf;
    put_tag(buf, 1, 0);  // implicit_scalar = 7
    put_varint(buf, 7);
    put_tag(buf, 6, 3);  // delim: DELIMITED framing = SGROUP body EGROUP, not LEN
    put_tag(buf, 1, 0);  //   delim.implicit_scalar = 42
    put_varint(buf, 42);
    put_tag(buf, 6, 4);  // EGROUP
    Arena arena;
    const ed23::M* m = ed23::M::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(m->implicit_scalar() == 7);
    CHECK_FALSE(m->explicit_scalar().has_value());  // EXPLICIT override: absent reads nullopt
    CHECK(m->child() == nullptr);
    REQUIRE(m->delim() != nullptr);
    CHECK(m->delim()->implicit_scalar() == 42);

    std::string buf2;
    put_tag(buf2, 2, 0);  // explicit_scalar = 0: present-and-zero must differ from absent
    put_varint(buf2, 0);
    const ed23::M* m2 = ed23::M::decode(ByteView(buf2), arena);
    REQUIRE(m2 != nullptr);
    CHECK(m2->implicit_scalar() == 0);
    REQUIRE(m2->explicit_scalar().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(*m2->explicit_scalar() == 0);
}

TEST_CASE("arena-decode: editions 2024 defaults (EXPLICIT presence, PACKED repeated)",
          "[arena-decode]") {
    std::string buf;
    put_tag(buf, 1, 0);  // a = 7 (no features written: presence comes from the 2024 defaults)
    put_varint(buf, 7);
    std::string packed;
    put_varint(packed, 1);
    put_varint(packed, 2);
    put_varint(packed, 3);
    put_len(buf, 2, packed);  // b, packed by default
    Arena arena;
    const ed24::M* m = ed24::M::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    REQUIRE(m->a().has_value());  // 2024 default presence is EXPLICIT, like 2023
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(*m->a() == 7);
    REQUIRE(m->b().size() == 3);
    CHECK(m->b()[0] == 1);
    CHECK(m->b()[2] == 3);
    const ed24::M* empty = ed24::M::decode(ByteView(""), arena);
    REQUIRE(empty != nullptr);
    CHECK_FALSE(empty->a().has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of entry cases
TEST_CASE("arena-decode: map entry edge cases on the wire", "[arena-decode]") {
    // p3::Msg.counts (field 9): map<string, int32>, entry = { key = 1 (LEN), value = 2 (varint) }.
    std::string entry_dup1;
    put_len(entry_dup1, 1, "k");
    put_tag(entry_dup1, 2, 0);
    put_varint(entry_dup1, 1);
    std::string entry_dup2;
    put_len(entry_dup2, 1, "k");
    put_tag(entry_dup2, 2, 0);
    put_varint(entry_dup2, 2);
    std::string entry_no_value;  // value absent -> zero default
    put_len(entry_no_value, 1, "empty");
    std::string entry_no_key;  // key absent -> "" default
    put_tag(entry_no_key, 2, 0);
    put_varint(entry_no_key, 9);
    std::string entry_unknown;  // an unknown field inside the entry is skipped, entry still lands
    put_len(entry_unknown, 1, "u");
    put_tag(entry_unknown, 99, 0);
    put_varint(entry_unknown, 1);
    put_tag(entry_unknown, 2, 0);
    put_varint(entry_unknown, 5);

    std::string buf;
    for (const std::string* e :
         {&entry_dup1, &entry_dup2, &entry_no_value, &entry_no_key, &entry_unknown}) {
        put_len(buf, 9, *e);
    }
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    REQUIRE(m->counts().size() == 5);  // insertion order, duplicates kept
    REQUIRE(m->counts().find(std::string_view("k")) != nullptr);
    CHECK(m->counts().find(std::string_view("k"))->value() == 2);  // last wins
    REQUIRE(m->counts().find(std::string_view("empty")) != nullptr);
    CHECK(m->counts().find(std::string_view("empty"))->value() == 0);
    REQUIRE(m->counts().find(std::string_view("")) != nullptr);
    CHECK(m->counts().find(std::string_view(""))->value() == 9);
    REQUIRE(m->counts().find(std::string_view("u")) != nullptr);
    CHECK(m->counts().find(std::string_view("u"))->value() == 5);
}

// ── field modes (arena_modes golden, generated under the shared `lean` profile) ─────────────────
// fm::Holder fields: keep=1 debug=2(drop) extra=3(drop) old_ids=4(drop) must=5(required)
// big_num=6 blob=7(raw msg) blobs=8(raw repeated msg) samples=9 spread=10 by_name=11(map,
// materialized -- the type entry skips maps) level=12 req_blob=13(raw required msg)
// grp=14(raw group).

namespace {

// The minimal records every successful Holder decode needs: must=1 and one req_blob record.
// Returns req_blob's PAYLOAD (the Blob message bytes) so tests can assert the stored view.
std::string holder_base(std::string& buf) {
    put_tag(buf, 5, 0);  // must = 1
    put_varint(buf, 1);
    std::string inner;  // Blob{ payload: "r" }
    put_len(inner, 1, "r");
    put_len(buf, 13, inner);
    return inner;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of payload checks
TEST_CASE("arena-decode: raw fields hold their exact payloads, decodable directly",
          "[arena-decode]") {
    std::string buf;
    const std::string req_blob_payload = holder_base(buf);
    // blob: the singular raw message; its stored view is the Blob payload, tag/length gone.
    std::string blob_payload;  // Blob{ payload: "bb" }
    put_len(blob_payload, 1, "bb");
    put_len(buf, 7, blob_payload);
    // blobs: two elements -- one with content, one EMPTY (present zero-length payload).
    std::string b0;  // Blob{ payload: "x" }
    put_len(b0, 1, "x");
    put_len(buf, 8, b0);
    put_len(buf, 8, "");
    // grp: the raw group; the stored view is the BARE body (no SGROUP/EGROUP framing).
    std::string grp_body;
    put_tag(grp_body, 1, 0);  // g = 5
    put_varint(grp_body, 5);
    put_tag(buf, 14, 3);
    buf += grp_body;
    put_tag(buf, 14, 4);
    put_tag(buf, 2, 0);  // debug = 9: dropped -- skipped, must not disturb anything
    put_varint(buf, 9);
    std::string entry;  // by_name["k"] = Blob{}: the map stayed MATERIALIZED (type entry skips it)
    put_len(entry, 1, "k");
    put_len(entry, 2, "");
    put_len(buf, 11, entry);

    Arena arena;
    ArenaDecodeError err{};
    const fm::Holder* h = fm::Holder::decode(ByteView(buf), arena, &err);
    REQUIRE(h != nullptr);
    CHECK(h->keep() == std::nullopt);  // untouched explicit field, absent
    REQUIRE(h->blob().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(*h->blob() == ByteView(blob_payload));
    CHECK(h->req_blob() == ByteView(req_blob_payload));
    REQUIRE(h->grp().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(*h->grp() == ByteView(grp_body));
    REQUIRE(h->blobs().size() == 2);
    CHECK(h->blobs()[0] == ByteView(b0));
    CHECK(h->blobs()[1].empty());            // present element, empty payload
    CHECK(h->blobs()[1].data() != nullptr);  // ...still a real (arena-backed) view
    REQUIRE(h->by_name().find(std::string_view("k")) != nullptr);  // materialized map
    // The point of raw: each view decodes DIRECTLY through the field type's own decoder.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    const fm::Blob* blob = fm::Blob::decode(*h->blob(), arena);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->payload().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(*blob->payload() == "bb");
    const fm::Blob* elem = fm::Blob::decode(h->blobs()[1], arena);  // empty payload: valid Blob{}
    REQUIRE(elem != nullptr);
    CHECK_FALSE(elem->payload().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    const fm::Holder::Grp* grp = fm::Holder::Grp::decode(*h->grp(), arena);
    REQUIRE(grp != nullptr);
    REQUIRE(grp->g().has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(*grp->g() == 5);
}

TEST_CASE("arena-decode: raw absence and present-empty payloads are distinct", "[arena-decode]") {
    std::string buf;
    holder_base(buf);
    put_len(buf, 7, "");  // blob PRESENT with an empty payload (a legitimate Blob{})
    Arena arena;
    const fm::Holder* h = fm::Holder::decode(ByteView(buf), arena);
    REQUIRE(h != nullptr);
    REQUIRE(h->blob().has_value());  // present-empty != absent (no mask bit: non-null empty view)
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by the REQUIRE above
    CHECK(h->blob()->empty());
    CHECK_FALSE(h->grp().has_value());  // absent raw singular: nullopt
    CHECK(h->blobs().empty());          // absent raw repeated: empty array
}

TEST_CASE("arena-decode: raw singular message keeps stored-field semantics", "[arena-decode]") {
    // A second occurrence of a raw singular MESSAGE field is rejected exactly like its
    // materialized arm would be -- raw changes the representation, never the semantics. This
    // holds even when the FIRST record's payload was empty (present-empty is non-null "seen").
    std::string buf;
    holder_base(buf);
    put_len(buf, 7, "");  // blob, first record (empty payload): fine
    std::string inner;
    put_len(inner, 1, "x");
    put_len(buf, 7, inner);  // blob, second record: RepeatedSingularMessage
    Arena arena;
    ArenaDecodeError err{};
    CHECK(fm::Holder::decode(ByteView(buf), arena, &err) == nullptr);
    CHECK(err.code == ArenaDecodeError::Code::RepeatedSingularMessage);
    CHECK(err.field_number == 7);
    // A mismatched wire type falls to the validated skip, exactly like a materialized message
    // arm -- it is not an occurrence.
    std::string wrong;
    holder_base(wrong);
    put_tag(wrong, 7, 0);  // blob at VARINT wire type
    put_varint(wrong, 3);
    const fm::Holder* h = fm::Holder::decode(ByteView(wrong), arena);
    REQUIRE(h != nullptr);
    CHECK_FALSE(h->blob().has_value());
}

TEST_CASE("arena-decode: raw required and dropped fields keep decode guarantees",
          "[arena-decode]") {
    Arena arena;
    // A raw REQUIRED field that never appears fails MissingRequired, like a stored one.
    std::string no_req;
    put_tag(no_req, 5, 0);  // must only
    put_varint(no_req, 1);
    ArenaDecodeError err{};
    CHECK(fm::Holder::decode(ByteView(no_req), arena, &err) == nullptr);
    CHECK(err.code == ArenaDecodeError::Code::MissingRequired);
    CHECK(err.field_number == 13);
    // A DROPPED field's records are still wire-validated: a truncated record fails the decode.
    std::string bad;
    holder_base(bad);
    put_tag(bad, 4, 2);  // old_ids (dropped), LEN claiming 5 bytes...
    put_varint(bad, 5);
    bad += "ab";  // ...but only 2 present
    ArenaDecodeError err2{};
    CHECK(fm::Holder::decode(ByteView(bad), arena, &err2) == nullptr);
    CHECK(err2.code == ArenaDecodeError::Code::Wire);
}
