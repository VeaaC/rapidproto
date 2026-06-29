// Decode tests for the generated arena decoders. Two kinds of oracle: (1) protoc-encoded wire
// fixtures (tests/wire_fixtures/*.bin) decoded through the generated decoder, asserting accessor
// values; (2) hand-built buffers for the behaviors fixtures don't cover -- default materialization,
// required-field validation, the recursion guard, malformed input, the single-bool-wrapper collapse,
// unknown-field drop, and oneof last-wins.

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "arenagen_golden/arena_manyreq.rp.hpp"
#include "arenagen_golden/arena_unknown.rp.hpp"  // --unknown-present + collapsed bool-wrapper
#include "arenagen_golden/main.rp.hpp"  // cross-file imports (pulls dep/forward/pub): runtime decode
#include "arenagen_golden/proto2.rp.hpp"
#include "arenagen_golden/proto3.rp.hpp"
#include "arenagen_golden/wire_all.rp.hpp"
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"  // ByteView, WireError

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

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
    CHECK(m->b());
    CHECK(m->s() == "hi");
    CHECK(m->by() == std::string("\x00\x01\xff", 3));
    CHECK(m->fl() == Catch::Approx(1.5F));
    CHECK(m->db() == Catch::Approx(-2.25));
    CHECK(m->has_i64());  // present in the fixture
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat list of accessor assertions
TEST_CASE("arena-decode: submessage, repeated, map, oneof fixture", "[arena-decode]") {
    const std::string bin = fixture("msg.bin");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    CHECK(m->implicit_i() == 10);
    CHECK(m->state() == p3::State::ON);
    REQUIRE(m->self() != nullptr);  // sub-message via arena pointer
    CHECK(m->self()->implicit_i() == 99);
    REQUIRE(m->nums().size() == 3);  // packed repeated
    CHECK(m->nums()[0] == 1);
    CHECK(m->nums()[2] == 3);
    REQUIRE(m->unpacked().size() == 2);  // expanded repeated
    CHECK(m->unpacked()[0] == 4);
    REQUIRE(m->states().size() == 2);  // repeated enum
    CHECK(m->pick_case() == p3::Msg::PickCase::kA);
    CHECK(m->a() == 7);
    REQUIRE(m->counts().size() == 2);  // map<string,int32>
    REQUIRE(m->counts().find(std::string_view("x")) != nullptr);
    CHECK(m->counts().find(std::string_view("x"))->value() == 1);
    CHECK(m->counts().find(std::string_view("y"))->value() == 2);
}

TEST_CASE("arena-decode: message-value and enum-value maps fixture", "[arena-decode]") {
    const std::string bin = fixture("container.bin");
    Arena arena;
    const p2::Container* c = p2::Container::decode(ByteView(bin), arena);
    REQUIRE(c != nullptr);
    REQUIRE(c->by_name().size() == 2);
    const auto* alpha = c->by_name().find(std::string_view("alpha"));
    REQUIRE(alpha != nullptr);
    REQUIRE(alpha->value() != nullptr);  // map value is a sub-message
    CHECK(alpha->value()->x() == 11);
    CHECK(c->by_name().find(std::string_view("beta"))->value()->x() == 22);
    REQUIRE(c->by_id().size() == 2);
    CHECK(c->by_id().find(1)->value() == p2::Color::RED);
    CHECK(c->by_id().find(2)->value() == p2::Color::NEG);  // negative enum
}

TEST_CASE("arena-decode: group (delimited) fixture", "[arena-decode]") {
    const std::string bin = fixture("all_wire.bin");
    Arena arena;
    const wire::AllWire* m = wire::AllWire::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    CHECK(m->zz() == -1234567890123LL);
    CHECK(m->fx() == 305419896U);
    CHECK(m->s() == "wire");
    REQUIRE(m->nested() != nullptr);
    CHECK(m->nested()->zz() == 7);
    REQUIRE(m->packed().size() == 3);
    REQUIRE(m->g() != nullptr);  // a group sub-message
    CHECK(m->g()->a() == 99);
    CHECK(m->oi() == 5);
}

// An absent proto2 optional field with a [default = ...] reads back that default; a present field
// overrides it. Only field 1 (required i32) is set here, so every other field is absent.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat list of default-value assertions
TEST_CASE("arena-decode: absent fields read their schema default", "[arena-decode]") {
    std::string buf;
    put_tag(buf, 1, 0);  // i32 (required) = 5
    put_varint(buf, 5);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(m->i32() == 5);
    CHECK_FALSE(m->has_i64());               // absent
    CHECK(m->i64() == 42);                   // ...reads [default = 42]
    CHECK(m->b());                           // [default = true]
    CHECK(m->fl() == Catch::Approx(1.5F));   // [default = 1.5]
    CHECK(m->db() == Catch::Approx(-2.25));  // [default = -2.25]
    CHECK(m->color() == p2::Color::RED);     // [default = COLOR_RED]
    CHECK(m->s() == "hi\n\"there\"");        // [default = "hi\n\"there\""] (escapes survive)
    CHECK(m->u32() == 0);                    // no default -> zero
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
    REQUIRE(m->d() != nullptr);
    CHECK(m->d()->v() == 42);  // singular cross-file
    REQUIRE(m->ds().size() == 2);
    CHECK(m->ds()[0].v() == 7);  // repeated cross-file
    CHECK(m->ds()[1].v() == 8);
    REQUIRE(m->dm().find(3) != nullptr);
    CHECK(m->dm().find(3)->value()->v() == 99);  // map-value cross-file
    REQUIRE(m->choice_case() == main::Main::ChoiceCase::kOd);
    CHECK(m->od()->v() == 5);  // oneof cross-file
}

// A nested group whose message type is a single-bool wrapper collapses to presence+value bits in the
// parent, but its accessor still returns the wrapper type.
TEST_CASE("arena-decode: single-bool-wrapper group collapses to bits", "[arena-decode]") {
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
    REQUIRE(w->has_mygroup());
    CHECK(w->mygroup()->a() == 7);
    CHECK(w->mygroup()->has_inner());     // the wrapper was present
    CHECK(w->mygroup()->inner().flag());  // ...and its bool decoded to true
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
    CHECK(m->pick_case() == p3::Msg::PickCase::kA);  // last set member
    CHECK(m->a() == 2);
}

// A single-bool wrapper collapses to bits in its parent (no struct), so under --unknown-present its own
// has_unknown_fields() would be lost unless the parent carries it in an extra mask bit. Build a wrapper
// sub-message that contains an unknown field and confirm the reconstructed wrapper reports it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): three decode cases, flat assertions
TEST_CASE("arena-decode: collapsed bool-wrapper keeps has_unknown_fields (--unknown-present)",
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

    // (1) the wrapper carried an unknown field: the reconstructed wrapper reports it; Holder itself,
    // whose own fields were all known, does not.
    const std::string with = build_holder(/*wrapper_has_unknown=*/true);
    const au::Holder* m = au::Holder::decode(ByteView(with), arena);
    REQUIRE(m != nullptr);
    CHECK(m->has_flag());
    CHECK(m->flag().value());
    CHECK(m->flag().has_unknown_fields());  // survived the bit-collapse
    CHECK_FALSE(m->has_unknown_fields());   // the unknown was inside the wrapper, not Holder
    CHECK(m->n() == 7);

    // (2) a clean wrapper reports no unknown fields.
    const std::string without = build_holder(/*wrapper_has_unknown=*/false);
    const au::Holder* c = au::Holder::decode(ByteView(without), arena);
    REQUIRE(c != nullptr);
    CHECK(c->flag().value());
    CHECK_FALSE(c->flag().has_unknown_fields());

    // (3) an unknown field at the Holder level sets Holder's own flag (not the wrapper's).
    std::string top;
    put_tag(top, 9, 0);  // field 9: unknown to Holder
    put_varint(top, 1);
    const au::Holder* h = au::Holder::decode(ByteView(top), arena);
    REQUIRE(h != nullptr);
    CHECK(h->has_unknown_fields());
    CHECK_FALSE(h->has_flag());

    // (4) the wrapper's OWN field number with a WRONG wire type is a known number, not an unknown field
    // -- it is skipped, exactly as the wrapper's standalone decoder would, so has_unknown_fields() stays
    // false. (Guards the collapsed/oneof parity the bit exists to preserve.)
    std::string wrong;
    put_len(wrong, 1, std::string("\x01", 1));  // field 1 as length-delimited (bool expects varint)
    std::string buf;
    put_len(buf, 1, wrong);  // Holder.flag = <wrapper with field 1 mistyped>
    const au::Holder* w = au::Holder::decode(ByteView(buf), arena);
    REQUIRE(w != nullptr);
    CHECK(w->has_flag());
    CHECK_FALSE(w->flag().value());
    CHECK_FALSE(w->flag().has_unknown_fields());
}
