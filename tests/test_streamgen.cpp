// Generated-decoder tests. (1) A golden test that regenerates the streaming header for
// proto2.proto and compares it byte-for-byte to the checked-in copy. (2) A compile-and-run test
// that includes that checked-in generated header and decodes the `scalars.bin` wire fixture
// through the generated `p2::stream::Scalars` decoder, asserting every singular scalar.
//
// Regenerate the goldens after an intentional generator change with: tests/regen_goldens.sh
// (the in-test RAPIDPROTO_REGEN_GOLDEN mode can't rebuild this binary when a change breaks the
// checked-in headers it #includes. The script drives the CLI directly; see it for the details).

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/runtime.hpp"
#include "rapidproto/streamgen/generator.hpp"
#include "temp_dir.hpp"

#include "streamgen_golden/proto2.rp.stream.hpp"  // checked-in generated headers
#include "streamgen_golden/proto3.rp.stream.hpp"
// IWYU pragma: begin_keep
#include "streamgen_golden/main.rp.stream.hpp"    // cross-file: pulls dep/forward/pub via #include
#include "streamgen_golden/naming.rp.stream.hpp"  // identifier dedup + absolute names: must compile
#include "streamgen_golden/usewkt.rp.stream.hpp"  // WKT closure: pulls google/protobuf/* headers
#include "streamgen_golden/weakmain.rp.stream.hpp"  // weak import: pulls weakdep via #include
#include "streamgen_golden/xref.rp.stream.hpp"      // mutually-cyclic A<->B: must compile
#include "streamgen_golden/xref_prefixed/xref.rp.stream.hpp"  // --namespace-prefix=rp -> namespace rp::xr
// IWYU pragma: end_keep
#include "streamgen_golden/wire_all.rp.stream.hpp"  // group + packed coverage
#include "streamgen_golden/packed.rp.stream.hpp"    // packed fixed-width (I32/I64) element decode
#include "streamgen_golden/editions2023.rp.stream.hpp"  // editions DELIMITED + presence features
#include "streamgen_golden/editions2024.rp.stream.hpp"  // IWYU pragma: keep -- 2024 compile-smoke
#include "streamgen_golden/dep.rp.stream.hpp"  // cross-file decode target (dep::stream::Dep; also via main)
#include "streamgen_golden/pub.rp.stream.hpp"      // re-exported cross-file decode target
#include "streamgen_golden/forward.rp.stream.hpp"  // public-import cross-file decode target
#include "streamgen_golden/weakdep.rp.stream.hpp"  // weak-import decode target (also via weakmain)
#include "streamgen_golden/google/protobuf/timestamp.rp.stream.hpp"  // WKT decode target (also via usewkt)
#include "streamgen_golden/google/protobuf/duration.rp.stream.hpp"  // WKT decode target (also via usewkt)

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): proto path vs include dir, distinct roles
std::string generate_at(const std::string& proto_path, const std::string& include_dir) {
    ResolverConfig config;
    config.include_paths = {include_dir};
    auto resolved = resolve(proto_path, config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    REQUIRE(analyze(set).is_ok());
    return streamgen::generate_header(set.files.back(), set.files);
}

// --- hand-built wire-buffer builders for the decode tests below ---
void put_varint(std::string& b, std::uint64_t v) {
    while (v >= 0x80U) {
        b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
        v >>= 7U;
    }
    b.push_back(static_cast<char>(v));
}
void put_tag(std::string& b, std::uint32_t field, std::uint32_t wire) {
    put_varint(b, (static_cast<std::uint64_t>(field) << 3) | wire);
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): value bits vs byte count, distinct roles
void put_le(std::string& b, std::uint64_t bits, int bytes) {  // little-endian fixed width
    for (int i = 0; i < bytes; ++i) {
        b.push_back(static_cast<char>((bits >> (8 * i)) & 0xFFU));
    }
}
void put_double(std::string& b, double d) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    put_le(b, bits, 8);
}
void put_float(std::string& b, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    put_le(b, bits, 4);
}

}  // namespace

TEST_CASE("streamgen: generated headers match the goldens", "[streamgen]") {
    const std::string corpus = RAPIDPROTO_CORPUS_DIR;
    const std::string fixtures = RAPIDPROTO_WIRE_FIXTURE_DIR;
    struct Case {
        std::string name;
        std::string include;
    };
    const std::string imports = corpus + "/imports";
    const std::vector<Case> cases = {
        {"proto2", corpus},       {"proto3", corpus},     {"xref", corpus},
        {"naming", corpus},       {"dep", imports},  // cross-file: dependency, re-export, entry
        {"pub", imports},         {"forward", imports},   {"main", imports},
        {"weakdep", imports},  // weak import: type still usable, header still included
        {"weakmain", imports},    {"wire_all", fixtures}, {"packed", corpus},
        {"editions2023", corpus},   // packed fixed-width; editions features
        {"editions2024", corpus}};  // 2024 defaults (EXPLICIT presence, PACKED repeated)

    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    const bool regen = std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr;
    for (const Case& test : cases) {
        const std::string actual =
            generate_at(test.include + "/" + test.name + ".proto", test.include);
        const std::string golden =
            std::string(RAPIDPROTO_STREAMGEN_GOLDEN_DIR) + "/" + test.name + ".rp.stream.hpp";
        if (regen) {
            std::ofstream(golden, std::ios::binary) << actual;
            WARN("regenerated streamgen golden: " << test.name);
            continue;
        }
        INFO("golden: " << test.name);
        CHECK(actual == read_file(golden));
    }
}

// A `--namespace-prefix` nests every file's C++ namespace (and every absolute type reference) under
// the prefix, so the generated decoders coexist with protoc's headers (which use the bare package
// namespace) in one translation unit. The default (empty prefix) keeps protoc parity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): two independent generation checks
TEST_CASE("streamgen: namespace prefix nests the generated namespace", "[streamgen]") {
    ResolverConfig config;
    config.include_paths = {std::string(RAPIDPROTO_CORPUS_DIR)};

    // proto3 with a multi-component prefix: namespace and every absolute reference are nested.
    auto p3 = resolve(std::string(RAPIDPROTO_CORPUS_DIR) + "/proto3.proto", config);
    REQUIRE(p3.is_ok());
    ResolvedFileSet p3set = std::move(p3).value();
    REQUIRE(analyze(p3set).is_ok());
    const std::string prefixed =
        streamgen::generate_header(p3set.files.back(), p3set.files, "rp.dec");
    CHECK(prefixed.find("namespace rp::dec::p3::stream {") != std::string::npos);
    CHECK(prefixed.find("::rp::dec::p3::stream::Msg") !=
          std::string::npos);  // message refs: prefixed + nested
    // The shared enum is prefixed but not model-nested (it lives in the common header at package scope,
    // aliased into the model namespace) -- the load-bearing coexistence invariant.
    CHECK(prefixed.find("::rp::dec::p3::State") != std::string::npos);
    CHECK(prefixed.find("::rp::dec::p3::stream::State") == std::string::npos);
    const std::string plain = streamgen::generate_header(p3set.files.back(), p3set.files);
    CHECK(plain.find("namespace p3::stream {") !=
          std::string::npos);  // default (no prefix) still nests under stream
    CHECK(plain.find("rp::dec") == std::string::npos);

    // xref (mutually-cyclic) with prefix `rp`: byte-stable against the checked-in golden, which is
    // also #included above so it COMPILES -- proving prefixed output is valid C++.
    auto xr = resolve(std::string(RAPIDPROTO_CORPUS_DIR) + "/xref.proto", config);
    REQUIRE(xr.is_ok());
    ResolvedFileSet xrset = std::move(xr).value();
    REQUIRE(analyze(xrset).is_ok());
    const std::string xr_prefixed =
        streamgen::generate_header(xrset.files.back(), xrset.files, "rp");
    const std::string golden =
        std::string(RAPIDPROTO_STREAMGEN_GOLDEN_DIR) + "/xref_prefixed/xref.rp.stream.hpp";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    if (std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr) {
        std::ofstream(golden, std::ios::binary) << xr_prefixed;
    } else {
        CHECK(xr_prefixed == read_file(golden));
    }

    // Coexistence in one TU: the prefixed `rp::xr::stream::A` and the unprefixed `xr::stream::A` (both #included
    // above) are distinct, usable types -- the same shape as coexisting with a protoc header.
    static_assert(!std::is_same_v<rp::xr::stream::A, xr::stream::A>);
}

// An unknown-field handler (a 1-arg `[](rapidproto::UnknownField){}`) receives fields whose number
// is not in the schema. Narrow semantics: a known field the caller didn't handle is NOT delivered
// to it (it is simply skipped). Only truly non-schema field numbers are.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one buffer build + one decode
TEST_CASE("streamgen: an unknown-field handler receives only non-schema fields", "[streamgen]") {
    const auto put_varint = [](std::string& b, std::uint32_t v) {
        while (v >= 0x80U) {
            b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
            v >>= 7U;
        }
        b.push_back(static_cast<char>(v));
    };
    std::string buf;
    buf.push_back('\x08');  // field 1 (implicit_i, varint): known, handled
    put_varint(buf, 7);
    put_varint(buf, (2U << 3) | 0U);  // field 2 (explicit_i, varint), known but not handled
    put_varint(buf, 20);
    put_varint(buf, (99U << 3) | 0U);  // field 99 (varint), an unknown number
    put_varint(buf, 42);
    put_varint(buf, (100U << 3) | 2U);  // field 100 (LEN), unknown number
    buf.push_back('\x02');
    buf += "hi";

    std::int32_t known = 0;
    std::vector<std::uint32_t> unknown_numbers;
    std::string unknown_len_bytes;
    const p3::stream::Msg msg{ByteView(buf)};
    const DecodeStatus s =
        msg.decode([&](p3::stream::Msg::implicit_i,
                       std::int32_t v) { known = v; },  // a known field (field 2 unhandled)
                   [&](UnknownField uf) {
                       unknown_numbers.push_back(uf.field_number);
                       if (uf.field_number == 100) {
                           CHECK(uf.wire_type == WireType::Len);
                           unknown_len_bytes =
                               std::string(uf.bytes);  // raw value bytes (length prefix + "hi")
                       }
                   });

    CHECK(s.ok());
    CHECK(known == 7);
    // Only the non-schema numbers reach the handler -- not field 2 (known but unhandled).
    CHECK(unknown_numbers == std::vector<std::uint32_t>{99, 100});
    CHECK(unknown_len_bytes == std::string("\x02hi", 3));

    // An unknown handler that aborts stops the walk (returns a non-ok, aborted status).
    const DecodeStatus aborted =
        msg.decode([](UnknownField) -> DecodeStatus { return DecodeStatus::abort(); });
    CHECK_FALSE(aborted.ok());
    CHECK(aborted.aborted);
}

// A generic field catch-all is known-fields-only: it does not absorb unknown (non-schema)
// fields; those reach only an explicit [](UnknownField) handler.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one buffer build + three decodes
TEST_CASE("streamgen: a field catch-all does not receive unknown fields", "[streamgen]") {
    const auto put_varint = [](std::string& b, std::uint32_t v) {
        while (v >= 0x80U) {
            b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
            v >>= 7U;
        }
        b.push_back(static_cast<char>(v));
    };
    std::string buf;
    buf.push_back('\x08');  // field 1 (implicit_i, varint): known
    put_varint(buf, 7);
    put_varint(buf, (99U << 3) | 0U);  // field 99 (varint): unknown number
    put_varint(buf, 42);
    const p3::stream::Msg msg{ByteView(buf)};

    // Catch-all only: it sees the known field but not the unknown one (which is skipped).
    int catchall_calls = 0;
    const DecodeStatus s1 = msg.decode([&](auto /*tag*/, auto&&... /*v*/) { ++catchall_calls; });
    CHECK(s1.ok());
    CHECK(catchall_calls == 1);  // only known field 1, not unknown field 99

    // Catch-all + explicit unknown handler: known -> catch-all, unknown -> UnknownField handler.
    int known_calls = 0;
    std::vector<std::uint32_t> unknown_numbers;
    const DecodeStatus s2 =
        msg.decode([&](auto /*tag*/, auto&&... /*v*/) { ++known_calls; },
                   [&](UnknownField uf) { unknown_numbers.push_back(uf.field_number); });
    CHECK(s2.ok());
    CHECK(known_calls == 1);
    CHECK(unknown_numbers == std::vector<std::uint32_t>{99});

    // A 1-arg generic [](auto) is also a valid unknown handler and is unambiguous next to a
    // variadic catch-all: known -> the (auto, auto&&...) catch-all, unknown -> the [](auto)
    // handler.
    int gen_known = 0;
    int gen_unknown = 0;
    const DecodeStatus s3 = msg.decode([&](auto /*tag*/, auto&&... /*v*/) { ++gen_known; },
                                       [&](auto /*uf*/) { ++gen_unknown; });
    CHECK(s3.ok());
    CHECK(gen_known == 1);
    CHECK(gen_unknown == 1);
}

// Packed FIXED-width repeated fields exercise the packed I32/I64 element path (read_fixed32/64 in
// the packed sub-reader). The varint-only packed fixtures never reach it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): builds three packed payloads
TEST_CASE("streamgen: packed fixed-width repeated fields decode per element", "[streamgen]") {
    std::string buf;
    put_tag(buf, 1, 2);  // pd (double, I64): LEN of two 8-byte doubles
    buf.push_back('\x10');
    put_double(buf, 1.5);
    put_double(buf, -2.25);
    put_tag(buf, 2, 2);  // psf (sfixed32, I32): LEN of two 4-byte values
    buf.push_back('\x08');
    put_le(buf, static_cast<std::uint32_t>(7), 4);
    put_le(buf, static_cast<std::uint32_t>(-3), 4);
    put_tag(buf, 3, 2);  // pf (float, I32): LEN of one 4-byte float
    buf.push_back('\x04');
    put_float(buf, 0.5F);

    std::vector<double> pd;
    std::vector<std::int32_t> psf;
    std::vector<float> pf;
    const pk::stream::Packed p{ByteView(buf)};
    const DecodeStatus s =
        p.decode([&](pk::stream::Packed::pd, double v) { pd.push_back(v); },
                 [&](pk::stream::Packed::psf, std::int32_t v) { psf.push_back(v); },
                 [&](pk::stream::Packed::pf, float v) { pf.push_back(v); });
    CHECK(s.ok());
    CHECK(pd == std::vector<double>{1.5, -2.25});
    CHECK(psf == std::vector<std::int32_t>{7, -3});
    CHECK(pf == std::vector<float>{0.5F});
}

// An editions DELIMITED message field (ed23.M.delim) uses GROUP wire format (SGROUP/EGROUP), not
// length-prefix: the wire form is taken from message_encoding, not is_group. The length-prefixed
// `child` and the delimited `delim` both decode their inner field.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): builds a LEN and a group
// sub-message
TEST_CASE("streamgen: editions DELIMITED field decodes via group wire format", "[streamgen]") {
    std::string buf;
    put_tag(buf, 3, 2);  // child: LEN { implicit_scalar(1) = 42 }
    buf.push_back('\x02');
    buf.push_back('\x08');
    buf.push_back('\x2a');
    put_tag(buf, 6, 3);  // delim: SGROUP { implicit_scalar(1) = 7 } EGROUP
    buf.push_back('\x08');
    buf.push_back('\x07');
    put_tag(buf, 6, 4);  // EGROUP for field 6

    std::int32_t child_inner = 0;
    std::int32_t delim_inner = 0;
    const ed23::stream::M m{ByteView(buf)};
    const DecodeStatus s = m.decode(
        [&](ed23::stream::M::child, ed23::stream::M c) -> DecodeStatus {
            return c.decode(
                [&](ed23::stream::M::implicit_scalar, std::int32_t v) { child_inner = v; });
        },
        [&](ed23::stream::M::delim, ed23::stream::M d) -> DecodeStatus {
            return d.decode(
                [&](ed23::stream::M::implicit_scalar, std::int32_t v) { delim_inner = v; });
        });
    CHECK(s.ok());
    CHECK(child_inner == 42);
    CHECK(delim_inner == 7);  // would be 0 (field skipped) if delim were decoded as length-prefixed
}

// A oneof's members fire per occurrence in wire order; "last wins" is the caller's job (the decoder
// does not dedup). Two members of one oneof in a buffer both fire, in order.
TEST_CASE("streamgen: oneof members fire per occurrence in wire order", "[streamgen]") {
    std::string buf;  // p3.Msg oneof pick { int32 a=10; string b=11; }: a=1, b="hi", a=2
    put_tag(buf, 10, 0);
    put_varint(buf, 1);
    put_tag(buf, 11, 2);
    buf.push_back('\x02');
    buf += "hi";
    put_tag(buf, 10, 0);
    put_varint(buf, 2);

    std::vector<std::string> events;
    const p3::stream::Msg m{ByteView(buf)};
    const DecodeStatus s = m.decode(
        [&](p3::stream::Msg::a, std::int32_t v) { events.push_back("a=" + std::to_string(v)); },
        [&](p3::stream::Msg::b, std::string_view v) { events.push_back("b=" + std::string(v)); });
    CHECK(s.ok());
    CHECK(events == std::vector<std::string>{"a=1", "b=hi", "a=2"});  // wire order, no dedup
}

// A repeated MESSAGE field fires its callback once per element, each a sub-decoder to recurse into
// (the most common real-world shape; previously generated but never decoded at runtime).
TEST_CASE("streamgen: a repeated message field fires per element", "[streamgen]") {
    std::string
        buf;  // p2.Container { items: [Nested{x=11}, Nested{x=22}] } (field 5, repeated msg)
    put_tag(buf, 5, 2);
    buf.push_back('\x02');
    buf.push_back('\x08');
    buf.push_back('\x0b');
    put_tag(buf, 5, 2);
    buf.push_back('\x02');
    buf.push_back('\x08');
    buf.push_back('\x16');

    std::vector<std::int32_t> xs;
    const p2::stream::Container c{ByteView(buf)};
    const DecodeStatus s = c.decode([&](p2::stream::Container::items,
                                        p2::stream::Container::Nested n) -> DecodeStatus {
        return n.decode([&](p2::stream::Container::Nested::x, std::int32_t v) { xs.push_back(v); });
    });
    CHECK(s.ok());
    CHECK(xs == std::vector<std::int32_t>{11, 22});  // one callback per element, in wire order
}

// Malformed input through a generated MAP decoder and a generated GROUP decoder propagates the wire
// error (distinct paths from a scalar-field truncation).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): two independent malformed inputs
TEST_CASE("streamgen: map and group decoders report malformed input", "[streamgen]") {
    SECTION("truncated value inside a map entry") {
        // p2.Container by_id (field 2, map<int32,Color>): a VALID 2-byte entry envelope, but the
        // value varint inside the entry is truncated -- this exercises the map-entry sub-reader
        // (rp_entry_reader), not the outer map-field LEN read.
        std::string buf;
        put_tag(buf, 2, 2);     // field 2 (by_id), LEN
        buf.push_back('\x02');  // entry length 2 (envelope is valid)
        buf.push_back('\x10');  // entry field 2 (value), varint
        buf.push_back('\x80');  // continuation bit set, but the entry ends here -> truncated
        const p2::stream::Container c{ByteView(buf)};
        const DecodeStatus s = c.decode([&](auto, auto&&...) {});
        CHECK_FALSE(s.ok());
        CHECK(s.wire == WireError::TruncatedVarint);  // raised by the entry sub-reader
    }
    SECTION("unterminated group") {
        std::string buf;  // p2.WithGroup MyGroup (field 1, group): SGROUP with no matching EGROUP
        put_tag(buf, 1, 3);
        const p2::stream::WithGroup w{ByteView(buf)};
        const DecodeStatus s = w.decode([&](auto, auto&&...) {});
        CHECK_FALSE(s.ok());
        CHECK(s.wire == WireError::UnterminatedGroup);
    }
}

// An extension field (extend WithGroup { optional int32 ext_a = 100; }) is not a generated struct
// field, so it surfaces via the UnknownField handler, pinning that extensions are unsupported and
// land in the unknown channel.
TEST_CASE("streamgen: an extension field surfaces as an unknown field", "[streamgen]") {
    std::string buf;  // p2.WithGroup carrying field 100 (ext_a, varint) = 9
    put_tag(buf, 100, 0);
    put_varint(buf, 9);

    std::vector<std::uint32_t> unknown;
    const p2::stream::WithGroup w{ByteView(buf)};
    const DecodeStatus s = w.decode([&](UnknownField uf) { unknown.push_back(uf.field_number); });
    CHECK(s.ok());
    CHECK(unknown == std::vector<std::uint32_t>{100});  // the extension number is not in the schema
}

// Backstop the well-known-type and cross-file goldens with a RUNTIME decode (they were compiled but
// never decoded): the WKT Timestamp/Duration, the cross-file main.Main -> dep/pub/fwd recursion,
// and the weak-import weakmain.WMain -> weakdep.WDep -- covering every never-run golden.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): three independent decode groups
TEST_CASE("streamgen: well-known-type and cross-file decoders run", "[streamgen]") {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): field number vs inner value, distinct
    const auto sub_i32 = [](std::string& b, std::uint32_t field, std::int32_t inner) {
        put_tag(b, field, 2);  // a LEN sub-message holding { field 1 (int32) = inner }
        b.push_back('\x02');
        b.push_back('\x08');
        b.push_back(static_cast<char>(inner));
    };

    SECTION("WKT Timestamp and Duration") {
        std::string ts;  // google.protobuf.Timestamp { seconds=5, nanos=7 }
        put_tag(ts, 1, 0);
        put_varint(ts, 5);
        put_tag(ts, 2, 0);
        put_varint(ts, 7);
        std::int64_t tsec = 0;
        std::int32_t tnanos = 0;
        CHECK(
            google::protobuf::stream::Timestamp{ByteView(ts)}
                .decode(
                    [&](google::protobuf::stream::Timestamp::seconds, std::int64_t v) { tsec = v; },
                    [&](google::protobuf::stream::Timestamp::nanos, std::int32_t v) { tnanos = v; })
                .ok());
        CHECK(tsec == 5);
        CHECK(tnanos == 7);

        std::string du;  // google.protobuf.Duration { seconds=3 }
        put_tag(du, 1, 0);
        put_varint(du, 3);
        std::int64_t dsec = 0;
        CHECK(google::protobuf::stream::Duration{ByteView(du)}
                  .decode([&](google::protobuf::stream::Duration::seconds, std::int64_t v) {
                      dsec = v;
                  })
                  .ok());
        CHECK(dsec == 3);
    }
    SECTION("cross-file main.Main -> dep.Dep / pub.Pub / fwd.Fwd") {
        std::string buf;  // main.Main { d: dep.Dep{v=9}, p: pub.Pub{w=8}, f: fwd.Fwd{z=7} }
        sub_i32(buf, 1, 9);
        sub_i32(buf, 2, 8);
        sub_i32(buf, 3, 7);
        std::int32_t dv = 0;
        std::int32_t pw = 0;
        std::int32_t fz = 0;
        const main::stream::Main mn{ByteView(buf)};
        const DecodeStatus s = mn.decode(
            [&](main::stream::Main::d, dep::stream::Dep d) -> DecodeStatus {
                return d.decode([&](dep::stream::Dep::v, std::int32_t v) { dv = v; });
            },
            [&](main::stream::Main::p, pub::stream::Pub p) -> DecodeStatus {
                return p.decode([&](pub::stream::Pub::w, std::int32_t v) { pw = v; });
            },
            [&](main::stream::Main::f, fwd::stream::Fwd f) -> DecodeStatus {
                return f.decode([&](fwd::stream::Fwd::z, std::int32_t v) { fz = v; });
            });
        CHECK(s.ok());
        CHECK(dv == 9);
        CHECK(pw == 8);
        CHECK(fz == 7);
    }
    SECTION("weak import weakmain.WMain -> weakdep.WDep") {
        std::string buf;  // wm.WMain { d: wd.WDep{v=5} }
        sub_i32(buf, 1, 5);
        std::int32_t v = 0;
        const wm::stream::WMain w{ByteView(buf)};
        const DecodeStatus s =
            w.decode([&](wm::stream::WMain::d, wd::stream::WDep d) -> DecodeStatus {
                return d.decode([&](wd::stream::WDep::v, std::int32_t inner) { v = inner; });
            });
        CHECK(s.ok());
        CHECK(v == 5);
    }
}

// The dispatch gate is an exact value-type match. A correct callback still decodes; a
// generic `[](auto, auto)` catch-all matches every field; and mixing a catch-all with a specific
// callback routes each field to the most specialized handler (no spurious "duplicate" error).
// (Wrong-value-type and duplicate-callback cases are now compile errors; the compile-fail harness
// in tests/streamgen_compile_fail.sh asserts that.)
TEST_CASE("streamgen: exact-match dispatch routes catch-all and specific callbacks",
          "[streamgen]") {
    const std::string bin = read_file(std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/msg.bin");
    if (bin.empty()) {
        SUCCEED("fixture msg.bin not present; skipping");
        return;
    }

    std::int32_t a = 0;    // routed to the specific callback
    int other_fields = 0;  // routed to the catch-all
    const p3::stream::Msg msg{ByteView(bin)};
    const DecodeStatus status = msg.decode(
        [&](p3::stream::Msg::a, std::int32_t v) { a = v; },           // specific
        [&](auto /*tag*/, auto&&... /*value*/) { ++other_fields; });  // generic catch-all

    CHECK(status.ok());
    CHECK(a == 7);            // the specific callback won for field `a`
    CHECK(other_fields > 0);  // the catch-all handled the remaining fields
}

// A catch-all may read the tag's compile-time identity (kName / kNumber) -- the documented logging
// pattern -- and still be a valid catch-all. (Regression: this previously failed to compile because
// the dispatch probed the catch-all's tag slot with a member-less type.)
TEST_CASE("streamgen: a catch-all can introspect the tag's kName/kNumber", "[streamgen]") {
    const std::string bin = read_file(std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/msg.bin");
    if (bin.empty()) {
        SUCCEED("fixture msg.bin not present; skipping");
        return;
    }
    int fields = 0;
    std::string a_name;
    const p3::stream::Msg msg{ByteView(bin)};
    const DecodeStatus status = msg.decode([&](auto tag, auto&& /*value*/) {
        ++fields;
        if (tag.kNumber == 10) {
            a_name = std::string(tag.kName);  // field `a` (#10) is in the fixture
        }
    });

    CHECK(status.ok());
    CHECK(fields >= 1);    // the catch-all fired, reading tag.kNumber
    CHECK(a_name == "a");  // ... and tag.kName gave field 10's real proto name
}

namespace {
struct CbExactA {
    void operator()(p3::stream::Msg::a /*tag*/, std::int32_t /*v*/) const {}
};
struct CbWrongA {
    void operator()(p3::stream::Msg::a /*tag*/, double /*v*/) const {
    }  // convertible-but-wrong type
};
struct CbOptionalA {
    void operator()(p3::stream::Msg::a /*tag*/, std::optional<std::int32_t> /*v*/) const {
    }  // wrapper of V
};
struct CbCatchAll {
    template <class T, class V>
    void operator()(T /*tag*/, V /*v*/) const {}
};
struct CbPartialValue {  // concrete tag, generic value -> partially generic (rejected)
    template <class V>
    void operator()(p3::stream::Msg::a /*tag*/, V /*v*/) const {}
};
struct CbPartialTag {  // generic tag, concrete value -> partially generic (rejected)
    template <class T>
    void operator()(T /*tag*/, std::int32_t /*v*/) const {}
};
struct CbVariadicCatchAll {  // generic tag + value pack -> a catch-all (also handles map arity)
    template <class T, class... V>
    void operator()(T /*tag*/, V&&... /*v*/) const {}
};
struct CbUnknown {  // the explicit unknown-field handler
    void operator()(rapidproto::UnknownField /*uf*/) const {}
};
struct
    CbIntrospectingCatchAll {  // a fixed-arity catch-all that READS the tag identity (kName/kNumber)
    template <class T, class V>
    void operator()(T tag, V&& /*v*/) const {
        (void)tag.kNumber;
        (void)tag.kName;
    }
};
}  // namespace

// The exact-match gate primitives (which the generated static_asserts are built on)
// accept and reject the right callback types. Checked at compile time; the end-to-end "this snippet
// must fail to compile" cases live in tests/streamgen_compile_fail.sh (run by check.sh).
TEST_CASE("streamgen: the exact-match dispatch gate accepts and rejects the right types",
          "[streamgen]") {
    using A = p3::stream::Msg::a;
    using V = p3::stream::Msg::a::Value;  // std::int32_t
    // An exact callback handles the field; a convertible-but-wrong one and a wrapper-of-V one do
    // not (they would silently narrow / silently wrap):
    static_assert(handles_one<CbExactA, A, V>);
    static_assert(!handles_one<CbWrongA, A, V>);
    static_assert(!handles_one<CbOptionalA, A, V>);
    // A generic catch-all handles every field; it is a catch-all, not a specific handler:
    static_assert(handles_one<CbCatchAll, A, V>);
    static_assert(is_catch_all<CbCatchAll, A, V>);
    static_assert(!is_catch_all<CbExactA, A, V>);
    // Duplicate detection counts only SPECIFIC handlers, so a catch-all is exempt:
    static_assert(specifically_handles<CbExactA, A, V>);
    static_assert(!specifically_handles<CbCatchAll, A, V>);
    static_assert(!specifically_handles<CbOptionalA, A, V>);
    // A wrong-type or wrapper callback still TARGETS the field -> a wrong-type error, not a skip:
    static_assert(targets<CbWrongA, A, V>);
    static_assert(targets<CbOptionalA, A, V>);
    // Partial-generic callbacks (auto in exactly one position) are rejected; neither a concrete
    // handler nor a full catch-all (including a variadic one) is partial-generic:
    static_assert(is_partial_generic<CbPartialValue, A, V>);
    static_assert(is_partial_generic<CbPartialTag, A, V>);
    static_assert(!is_partial_generic<CbExactA, A, V>);
    static_assert(!is_partial_generic<CbCatchAll, A, V>);
    static_assert(!is_partial_generic<CbVariadicCatchAll, A, V>);
    // The unknown handler must specifically take UnknownField. A field catch-all -- even a variadic
    // one that happens to accept a lone UnknownField -- is known-fields-only, NOT the unknown
    // handler:
    static_assert(specifically_handles_unknown<CbUnknown>);
    static_assert(!specifically_handles_unknown<CbCatchAll>);
    static_assert(!specifically_handles_unknown<CbVariadicCatchAll>);
    static_assert(!specifically_handles_unknown<CbExactA>);
    static_assert(is_catch_all<CbVariadicCatchAll, A, V>);  // it IS a (known-field) catch-all
    // A fixed-arity catch-all may READ the tag's identity (kName/kNumber) and still classify as a
    // catch-all, not partial-generic -- the tag slot is probed with a tag-shaped type (tag_probe).
    static_assert(is_catch_all<CbIntrospectingCatchAll, A, V>);
    static_assert(!is_partial_generic<CbIntrospectingCatchAll, A, V>);
    static_assert(handles_one<CbIntrospectingCatchAll, A, V>);
    static_assert(
        !specifically_handles_unknown<CbIntrospectingCatchAll>);  // a field catch-all, not unknown
    SUCCEED("exact-match gate trait values verified at compile time");
}

// A generated decoder propagates a wire-level failure (the value read fails) and a
// callback abort, both as a non-ok DecodeStatus, and the callback does not fire on a failed read.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): two independent sections
TEST_CASE("streamgen: a generated decoder reports malformed input and callback aborts",
          "[streamgen]") {
    SECTION("truncated value -> wire error") {
        // field 1 (implicit_i, varint), then a varint byte with the continuation bit set and no
        // continuation: the value read fails with TruncatedVarint.
        const std::string truncated("\x08\x80", 2);
        const p3::stream::Msg msg{ByteView(truncated)};
        bool fired = false;
        const DecodeStatus s =
            msg.decode([&](p3::stream::Msg::implicit_i, std::int32_t) { fired = true; });
        CHECK_FALSE(s.ok());
        CHECK(s.wire == WireError::TruncatedVarint);
        CHECK_FALSE(fired);  // a failed read never invokes the callback
    }
    SECTION("callback abort -> aborted status") {
        const std::string valid("\x08\x0a", 2);  // field 1 (implicit_i) = 10
        const p3::stream::Msg msg{ByteView(valid)};
        const DecodeStatus s =
            msg.decode([&](p3::stream::Msg::implicit_i, std::int32_t) -> DecodeStatus {
                return DecodeStatus::abort();
            });
        CHECK_FALSE(s.ok());
        CHECK(s.aborted);
        CHECK(s.wire == WireError::None);  // an abort is distinct from a wire error
    }
}

// The CLI generates the whole resolved closure (entry + transitive imports + well-known
// types), so a schema using google.protobuf.* compiles standalone. This regenerates every file in
// usewkt.proto's closure (including the embedded WKT sources) and golden-checks each.
TEST_CASE("streamgen: well-known-type closure generates self-contained headers", "[streamgen]") {
    ResolverConfig config;
    config.include_paths = {std::string(RAPIDPROTO_CORPUS_DIR)};
    auto resolved = resolve(std::string(RAPIDPROTO_CORPUS_DIR) + "/usewkt.proto", config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    REQUIRE(analyze(set).is_ok());

    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    const bool regen = std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr;
    for (const FileNode& file : set.files) {
        std::filesystem::path stem = file.filename;
        stem.replace_extension();  // "google/protobuf/timestamp.proto" -> ".../timestamp"
        const std::filesystem::path golden =
            std::filesystem::path(RAPIDPROTO_STREAMGEN_GOLDEN_DIR) /
            (stem.string() + ".rp.stream.hpp");
        const std::string actual = streamgen::generate_header(file, set.files);
        if (regen) {
            std::filesystem::create_directories(golden.parent_path());
            std::ofstream(golden, std::ios::binary) << actual;
            WARN("regenerated streamgen golden: " << stem.string());
            continue;
        }
        INFO("golden: " << stem.string());
        CHECK(actual == read_file(golden.string()));
    }
}

// Distinct proto names that sanitize alike are de-duplicated per scope (no struct/
// enumerator redefinition), tags keep the real proto name in kName, and type references are
// absolute. These bindings are checked at compile time; reaching this body means they held.
TEST_CASE("streamgen: colliding identifiers are de-duplicated and types bind absolutely",
          "[streamgen]") {
    // Field tag `decode` -> decode_, sibling `decode_` -> decode__, oneof `decode__` -> decode___.
    static_assert(nm::stream::M::decode_::kNumber == 1);
    static_assert(nm::stream::M::decode__::kNumber == 2);
    static_assert(nm::stream::M::decode___::kNumber == 6);
    CHECK(nm::stream::M::decode_::kName == "decode");  // identifier deduped, wire name preserved
    CHECK(nm::stream::M::decode__::kName == "decode_");
    CHECK(nm::stream::M::decode___::kName == "decode__");
    // Nested type `int` keeps `int_` (indexed first); field `int_` becomes `int__`.
    static_assert(std::is_same_v<nm::stream::M::int_field::Value, nm::stream::M::int_>);
    static_assert(nm::stream::M::int__::kName[0] ==
                  'i');  // the field, distinct from the nested type
    // Enum value `decode` -> decode_, `decode_` -> decode__; type reference is fully ::-rooted.
    static_assert(nm::stream::E::decode_ == static_cast<nm::stream::E>(0));
    static_assert(nm::stream::E::decode__ == static_cast<nm::stream::E>(1));
    static_assert(std::is_same_v<nm::stream::M::e::Value, nm::stream::E>);
    SUCCEED("dedup + absolute-binding bindings compiled");
}

// A field whose type comes from an imported .proto resolves because the generated header
// #includes the import's header. `main` pulls dep + forward; forward re-exports pub (public
// import). Reaching this body means the whole include chain compiled with the right cross-file
// bindings.
TEST_CASE("streamgen: cross-file type references resolve through emitted includes", "[streamgen]") {
    // dep/fwd/pub are reached transitively via main.rp.stream.hpp's emitted includes -- that chain
    // is exactly what this test exercises, so the indirect references are intentional.
    // NOLINTBEGIN(misc-include-cleaner)
    static_assert(
        std::is_same_v<main::stream::Main::d::Value, ::dep::stream::Dep>);  // standard import
    static_assert(
        std::is_same_v<main::stream::Main::e::Value, ::dep::stream::DepEnum>);  // imported enum
    static_assert(
        std::is_same_v<main::stream::Main::f::Value, ::fwd::stream::Fwd>);  // forward.proto
    static_assert(
        std::is_same_v<main::stream::Main::p::Value, ::pub::stream::Pub>);  // public re-export
    static_assert(std::is_same_v<wm::stream::WMain::d::Value,
                                 ::wd::stream::WDep>);  // weak import, type still usable
    static_assert(std::is_same_v<uw::stream::Event::at::Value,
                                 ::google::protobuf::stream::Timestamp>);  // WKT
    static_assert(
        std::is_same_v<uw::stream::Event::took::Value, ::google::protobuf::stream::Duration>);
    // NOLINTEND(misc-include-cleaner)
    SUCCEED("cross-file include chain compiled");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one callback per scalar field
TEST_CASE("streamgen: a generated decoder decodes a wire fixture", "[streamgen]") {
    const std::string bin = read_file(std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/scalars.bin");
    if (bin.empty()) {
        SUCCEED("fixture scalars.bin not present; skipping");
        return;
    }

    std::int32_t i32 = 0;
    std::int64_t i64 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    std::int32_t s32 = 0;
    std::int64_t s64 = 0;
    std::uint32_t f32 = 0;
    std::uint64_t f64 = 0;
    std::int32_t sf32 = 0;
    std::int64_t sf64 = 0;
    bool b = false;
    std::string s;
    std::string by;
    float fl = 0;
    double db = 0;

    const p2::stream::Scalars decoder{ByteView(bin)};
    const DecodeStatus status =
        decoder.decode([&](p2::stream::Scalars::i32, std::int32_t v) { i32 = v; },
                       [&](p2::stream::Scalars::i64, std::int64_t v) { i64 = v; },
                       [&](p2::stream::Scalars::u32, std::uint32_t v) { u32 = v; },
                       [&](p2::stream::Scalars::u64, std::uint64_t v) { u64 = v; },
                       [&](p2::stream::Scalars::s32, std::int32_t v) { s32 = v; },
                       [&](p2::stream::Scalars::s64, std::int64_t v) { s64 = v; },
                       [&](p2::stream::Scalars::f32, std::uint32_t v) { f32 = v; },
                       [&](p2::stream::Scalars::f64, std::uint64_t v) { f64 = v; },
                       [&](p2::stream::Scalars::sf32, std::int32_t v) { sf32 = v; },
                       [&](p2::stream::Scalars::sf64, std::int64_t v) { sf64 = v; },
                       [&](p2::stream::Scalars::b, bool v) { b = v; },
                       [&](p2::stream::Scalars::s, std::string_view v) { s = std::string(v); },
                       [&](p2::stream::Scalars::by, std::string_view v) { by = std::string(v); },
                       [&](p2::stream::Scalars::fl, float v) { fl = v; },
                       [&](p2::stream::Scalars::db, double v) { db = v; });

    CHECK(status.ok());  // color (enum) + repeated fields are skipped, not errors
    CHECK(i32 == -7);
    CHECK(i64 == 42);
    CHECK(u32 == 300);
    CHECK(u64 == 1000000);
    CHECK(s32 == -5);
    CHECK(s64 == -2);
    CHECK(f32 == 0x01020304U);
    CHECK(f64 == 0x0102030405060708ULL);
    CHECK(sf32 == -2);
    CHECK(sf64 == -3);
    CHECK(b);
    CHECK(s == "hi");
    CHECK(by == std::string("\x00\x01\xff", 3));
    CHECK(fl == Catch::Approx(1.5F));
    CHECK(db == Catch::Approx(-2.25));
}

// The single-byte-tag fast path (fields 1..15) must behave identically to the general path (fields
// >=16) on the boundary and on malformed input. Scalars spans it: i32=1 (fast) and color=16 (general).
// NOLINTNEXTLINE(readability-function-cognitive-complexity): three independent buffers in one case
TEST_CASE("streamgen: raw-byte fast path matches the general path across the 15/16 boundary",
          "[streamgen]") {
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

    // (1) Boundary: a <=15 field (i32, 1-byte tag, fast path) and a >=16 field (color, 2-byte tag,
    //     general path) both decode in one message.
    {
        std::string buf;
        pv(buf, tag(1, 0));
        pv(buf, 123);  // i32
        pv(buf, tag(16, 0));
        pv(buf, 2);  // color (enum), 2-byte tag 0x80 0x01
        std::int32_t i32 = 0;
        int color = -1;
        const DecodeStatus s = p2::stream::Scalars{ByteView(buf)}.decode(
            [&](p2::stream::Scalars::i32, std::int32_t v) { i32 = v; },
            [&](p2::stream::Scalars::color, ::p2::Color v) { color = static_cast<int>(v); });
        CHECK(s.ok());
        CHECK(i32 == 123);
        CHECK(color == 2);
    }

    // (2) A KNOWN field with the wrong wire type is skipped -- consistently for the fast field (i32)
    //     and the general field (color) -- and NEVER delivered to an unknown-field handler (which is
    //     for non-schema numbers only). The wrong wire type misses the fast switch and is skipped by
    //     the field arm's wire-type guard on the general path.
    {
        std::string buf;
        pv(buf, tag(1, 2));  // i32 (fast) as LEN -- wrong wire
        pv(buf, 1);
        buf += "x";
        pv(buf, tag(16, 2));  // color (general) as LEN -- wrong wire
        pv(buf, 1);
        buf += "y";
        std::int32_t i32 = -1;
        int color = -1;
        std::vector<std::uint32_t> unknown;
        const DecodeStatus s = p2::stream::Scalars{ByteView(buf)}.decode(
            [&](p2::stream::Scalars::i32, std::int32_t v) { i32 = v; },
            [&](p2::stream::Scalars::color, ::p2::Color v) { color = static_cast<int>(v); },
            [&](UnknownField uf) { unknown.push_back(uf.field_number); });
        CHECK(s.ok());
        CHECK(i32 == -1);        // wrong-wire fast field skipped, callback not fired
        CHECK(color == -1);      // wrong-wire general field skipped, callback not fired
        CHECK(unknown.empty());  // neither known field leaked to the unknown handler
    }

    // (3) An invalid tag is rejected identically whether encoded in one byte (fast-path range) or two
    //     (general): a reserved wire type (6) never matches a fast case, so both go through the same
    //     validating read_tag_or_end and fail.
    {
        std::string one;
        pv(one, tag(1, 6));  // 1-byte reserved-wire tag
        std::string two;
        pv(two, tag(16, 6));  // 2-byte reserved-wire tag
        const DecodeStatus s1 = p2::stream::Scalars{ByteView(one)}.decode(
            [](p2::stream::Scalars::i32, std::int32_t) {});
        const DecodeStatus s2 = p2::stream::Scalars{ByteView(two)}.decode(
            [](p2::stream::Scalars::i32, std::int32_t) {});
        CHECK_FALSE(s1.ok());
        CHECK_FALSE(s2.ok());
    }

    // (4) A NON-MINIMAL (over-long) tag of a low field is valid protobuf -- the wire spec is silent on
    //     varint minimality and the reference parsers accept it. It misses the 1-byte fast path
    //     (continuation bit set) and takes the general path, where it DECODES (identically to a canonical
    //     tag), delivering the value to its callback. field 1 (i32, Varint) as 0x88 0x00 (== the tag 8,
    //     non-minimal), + value 55.
    {
        std::string buf;
        buf.push_back('\x88');
        buf.push_back('\x00');  // over-long tag: field 1, Varint
        pv(buf, 55);
        std::int32_t i32 = -1;
        std::vector<std::uint32_t> unknown;
        const DecodeStatus s = p2::stream::Scalars{ByteView(buf)}.decode(
            [&](p2::stream::Scalars::i32, std::int32_t v) { i32 = v; },
            [&](UnknownField uf) { unknown.push_back(uf.field_number); });
        CHECK(s.ok());
        CHECK(i32 == 55);  // value delivered on the general path
        CHECK(unknown
                  .empty());  // a known field -> delivered to its callback, not surfaced as unknown
    }
}

// A non-minimal (over-long) tag must decode identically to the canonical 1-byte tag for every fast
// field kind in the streaming decoder too -- including the string, sub-message, and oneof members that
// are fast in streamgen (number <=15) and run the length/recursion/oneof-dispatch logic on the general
// arm. over2() re-encodes a <=15 field's one-byte tag as a 2-byte non-minimal varint.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one independent parity block per kind
TEST_CASE("streamgen: non-minimal tags decode identically across fast field kinds", "[streamgen]") {
    const auto pv = [](std::string& b, std::uint64_t v) {
        while (v >= 0x80U) {
            b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
            v >>= 7U;
        }
        b.push_back(static_cast<char>(v));
    };
    const auto tg = [](std::uint32_t f, std::uint32_t w) {
        return (static_cast<std::uint64_t>(f) << 3U) | w;
    };
    const auto over2 = [&](std::string& b, std::uint32_t f, std::uint32_t w) {
        const std::uint64_t t = tg(f, w);  // a <=15 field's canonical tag is one byte (< 128)
        b.push_back(static_cast<char>(0x80U | (t & 0x7FU)));
        b.push_back(static_cast<char>(t >> 7U));  // 0x00 for t < 128
    };

    // string s (Scalars field 12, wire LEN): the over-long tag delivers the same value to the callback.
    {
        std::string over;
        over2(over, 12, 2);
        pv(over, 5);
        over += "hello";
        std::string got_over;
        std::vector<std::uint32_t> unknown;
        const DecodeStatus so = p2::stream::Scalars{ByteView(over)}.decode(
            [&](p2::stream::Scalars::s, std::string_view v) { got_over = std::string(v); },
            [&](UnknownField uf) { unknown.push_back(uf.field_number); });
        std::string canon;
        pv(canon, tg(12, 2));
        pv(canon, 5);
        canon += "hello";
        std::string got_canon;
        const DecodeStatus sc = p2::stream::Scalars{ByteView(canon)}.decode(
            [&](p2::stream::Scalars::s, std::string_view v) { got_canon = std::string(v); });
        CHECK(so.ok());
        CHECK(sc.ok());
        CHECK(got_over == got_canon);
        CHECK(got_over == "hello");
        CHECK(unknown.empty());
    }

    // oneof sub-message cn (Container field 4, wire LEN): the general arm runs oneof dispatch + message
    // recursion; the over-long tag fires the sub-decoder with the same nested value.
    {
        std::string nested;  // Nested { x = 42 }
        pv(nested, tg(1, 0));
        pv(nested, 42);
        std::string over;
        over2(over, 4, 2);
        pv(over, nested.size());
        over += nested;
        std::int32_t cn_x = 0;
        const DecodeStatus s = p2::stream::Container{ByteView(over)}.decode(
            [&](p2::stream::Container::cn, p2::stream::Container::Nested v) -> DecodeStatus {
                return v.decode(
                    [&](p2::stream::Container::Nested::x, std::int32_t xv) { cn_x = xv; });
            });
        CHECK(s.ok());
        CHECK(cn_x == 42);  // over-long oneof-submessage tag decoded through the sub-decoder
    }

    // repeated sub-message items (Container field 5, wire LEN): an over-long element tag fires the
    // element callback with the same value as a canonical one, both in a single decode.
    {
        const auto nested = [&](std::int32_t x) {
            std::string n;
            pv(n, tg(1, 0));
            pv(n, static_cast<std::uint64_t>(x));
            return n;
        };
        const std::string n1 = nested(7);
        const std::string n2 = nested(8);
        std::string buf;
        over2(buf, 5, 2);  // over-long element tag
        pv(buf, n1.size());
        buf += n1;
        pv(buf, tg(5, 2));  // canonical element tag
        pv(buf, n2.size());
        buf += n2;
        std::vector<std::int32_t> xs;
        const DecodeStatus s = p2::stream::Container{ByteView(buf)}.decode(
            [&](p2::stream::Container::items, p2::stream::Container::Nested v) -> DecodeStatus {
                std::int32_t x = 0;
                const DecodeStatus sub =
                    v.decode([&](p2::stream::Container::Nested::x, std::int32_t xv) { x = xv; });
                xs.push_back(x);
                return sub;
            });
        CHECK(s.ok());
        REQUIRE(xs.size() == 2);  // over-long and canonical element tags both fire the callback
        CHECK(xs[0] == 7);
        CHECK(xs[1] == 8);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one callback per field
TEST_CASE("streamgen: a generated decoder decodes enum, sub-message, and oneof fields",
          "[streamgen]") {
    const std::string bin = read_file(std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/msg.bin");
    if (bin.empty()) {
        SUCCEED("fixture msg.bin not present; skipping");
        return;
    }

    std::int32_t implicit_i = 0;
    std::int32_t a = 0;
    std::int32_t self_implicit = 0;
    p3::stream::State state = p3::stream::State::UNKNOWN;
    bool saw_self = false;
    std::vector<std::int32_t> nums;         // repeated int32 (packed on the wire)
    std::vector<std::int32_t> unpacked;     // repeated int32 [packed=false] (expanded)
    std::vector<p3::stream::State> states;  // repeated enum (packed)
    std::vector<std::pair<std::string, std::int32_t>> counts;  // map<string,int32>, per entry

    const p3::stream::Msg msg{ByteView(bin)};
    const DecodeStatus status = msg.decode(
        [&](p3::stream::Msg::implicit_i, std::int32_t v) { implicit_i = v; },
        [&](p3::stream::Msg::state, p3::stream::State v) { state = v; },  // open enum value
        [&](p3::stream::Msg::self,
            p3::stream::Msg sub) -> DecodeStatus {  // sub-decoder, read recursively
            saw_self = true;
            return sub.decode(
                [&](p3::stream::Msg::implicit_i, std::int32_t v) { self_implicit = v; });
        },
        [&](p3::stream::Msg::nums, std::int32_t v) { nums.push_back(v); },  // fires per element
        [&](p3::stream::Msg::unpacked, std::int32_t v) {
            unpacked.push_back(v);
        },  // expanded, per element
        [&](p3::stream::Msg::states, p3::stream::State v) { states.push_back(v); },
        [&](p3::stream::Msg::counts, std::string_view k,
            std::int32_t v) {  // map entry: (Tag, Key, Value)
            counts.emplace_back(std::string(k), v);
        },
        [&](p3::stream::Msg::a, std::int32_t v) { a = v; });

    CHECK(status.ok());
    CHECK(implicit_i == 10);
    CHECK(state == p3::stream::State::ON);
    CHECK(a == 7);  // the oneof member that is set
    CHECK(saw_self);
    CHECK(self_implicit == 99);  // nested message decoded through the sub-decoder
    CHECK(nums == std::vector<std::int32_t>{1, 2, 3});   // packed scalar -> per element
    CHECK(unpacked == std::vector<std::int32_t>{4, 5});  // expanded scalar -> per element
    CHECK(states ==
          std::vector<p3::stream::State>{p3::stream::State::ON, p3::stream::State::UNKNOWN});
    CHECK(counts == std::vector<std::pair<std::string, std::int32_t>>{{"x", 1}, {"y", 2}});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one callback per map field
TEST_CASE("streamgen: a generated decoder decodes message-value and enum-value maps",
          "[streamgen]") {
    const std::string bin = read_file(std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/container.bin");
    if (bin.empty()) {
        SUCCEED("fixture container.bin not present; skipping");
        return;
    }

    std::vector<std::pair<std::string, std::int32_t>> by_name;  // map<string, Nested> -> (key, x)
    std::vector<std::pair<std::int32_t, p2::stream::Color>> by_id;  // map<int32, Color>
    DecodeStatus nested_status = DecodeStatus::success();

    const p2::stream::Container container{ByteView(bin)};
    const DecodeStatus status = container.decode(
        // Message-value map: the value arrives as a Nested sub-decoder, read recursively.
        [&](p2::stream::Container::by_name, std::string_view k,
            p2::stream::Container::Nested v) -> DecodeStatus {
            std::int32_t x = 0;
            const DecodeStatus sub =
                v.decode([&](p2::stream::Container::Nested::x, std::int32_t xv) { x = xv; });
            if (!sub.ok()) {
                nested_status = sub;
            }
            by_name.emplace_back(std::string(k), x);
            return DecodeStatus::success();
        },
        // Enum-value map: open enum, COLOR_NEG is a negative (10-byte) varint on the wire.
        [&](p2::stream::Container::by_id, std::int32_t k, p2::stream::Color v) {
            by_id.emplace_back(k, v);
        });

    CHECK(status.ok());
    CHECK(nested_status.ok());
    CHECK(by_name ==
          std::vector<std::pair<std::string, std::int32_t>>{{"alpha", 11}, {"beta", 22}});
    CHECK(by_id == std::vector<std::pair<std::int32_t, p2::stream::Color>>{
                       {1, p2::stream::Color::RED}, {2, p2::stream::Color::NEG}});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one callback per field
TEST_CASE("streamgen: a generated decoder decodes packed-repeated and group fields",
          "[streamgen]") {
    const std::string bin = read_file(std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/all_wire.bin");
    if (bin.empty()) {
        SUCCEED("fixture all_wire.bin not present; skipping");
        return;
    }

    std::int64_t zz = 0;
    std::uint32_t fx = 0;
    std::string s;
    std::int64_t nested_zz = 0;  // nested AllWire.zz is sint64
    std::int32_t group_a = 0;
    std::int32_t oi = 0;
    std::vector<std::int32_t> packed;
    bool saw_group = false;

    const ::wire::stream::AllWire all{ByteView(bin)};
    const DecodeStatus status = all.decode(
        [&](::wire::stream::AllWire::zz, std::int64_t v) { zz = v; },  // sint64 (zigzag)
        [&](::wire::stream::AllWire::fx, std::uint32_t v) { fx = v; },
        [&](::wire::stream::AllWire::s, std::string_view v) { s = std::string(v); },
        [&](::wire::stream::AllWire::nested, ::wire::stream::AllWire sub) -> DecodeStatus {
            return sub.decode([&](::wire::stream::AllWire::zz, std::int64_t v) { nested_zz = v; });
        },
        [&](::wire::stream::AllWire::packed, std::int32_t v) {
            packed.push_back(v);
        },  // packed repeated
        [&](::wire::stream::AllWire::g,
            ::wire::stream::AllWire::G grp) -> DecodeStatus {  // group sub-decoder
            saw_group = true;
            return grp.decode([&](::wire::stream::AllWire::G::a, std::int32_t v) { group_a = v; });
        },
        [&](::wire::stream::AllWire::oi, std::int32_t v) { oi = v; });

    CHECK(status.ok());
    CHECK(zz == -1234567890123);
    CHECK(fx == 305419896);
    CHECK(s == "wire");
    CHECK(nested_zz == 7);                                   // recursed sub-message
    CHECK(packed == std::vector<std::int32_t>{10, 20, 30});  // packed repeated -> per element
    CHECK(saw_group);
    CHECK(group_a == 99);  // recursed group body
    CHECK(oi == 5);
}

TEST_CASE("streamgen: a long sibling dependency chain is emitted in dependency order",
          "[streamgen]") {
    // The streamgen twin of the arenagen long-chain test: each M(k) holds a field of M(k-1).Inner,
    // so naming the nested type requires M(k-1)'s definition first -- one long must-precede path,
    // unbounded in a protoc-valid schema (why ordered_siblings walks it iteratively; the ordering
    // contract is what's observable at test-friendly sizes).
    constexpr int kChainLen = 300;
    std::string schema = "syntax = \"proto3\";\npackage chain;\n";
    for (int i = kChainLen; i >= 2; --i) {
        schema += "message M" + std::to_string(i) + " { M" + std::to_string(i - 1) +
                  ".Inner f = 1; message Inner { int32 v = 1; } }\n";
    }
    schema += "message M1 { message Inner { int32 v = 1; } }\n";
    const test::TempDir dir("streamgen_chain");
    dir.write("chain.proto", schema);

    const std::string header = generate_at(dir.path("chain.proto"), dir.root());
    // Dependencies force definition order M1 < M2 < ... despite the reversed declaration order
    // ("struct Mx {" is exact; a missing struct fails loudly because npos < npos is false).
    for (int i = 2; i <= kChainLen; ++i) {
        CHECK(header.find("struct M" + std::to_string(i - 1) + " {") <
              header.find("struct M" + std::to_string(i) + " {"));
    }
}

TEST_CASE("streamgen: rp_bytes() exposes the exact undecoded span", "[streamgen]") {
    // The hybrid seam (stream the outer message, arena-decode a chosen sub-message): a callback's
    // sub-decoder must expose EXACTLY the sub-message's field bytes -- a LEN payload without its
    // length prefix, and a group/DELIMITED body without its SGROUP/EGROUP framing -- because the
    // arena model's decode() consumes a plain field sequence. The end-to-end handoff runs in
    // examples/consumer; this pins the span itself.
    std::string sub;
    put_tag(sub, 1, 0);  // child.implicit_scalar = 42
    put_varint(sub, 42);

    std::string len_buf;  // child (field 3): LEN framing
    put_tag(len_buf, 3, 2);
    put_varint(len_buf, sub.size());
    len_buf += sub;

    std::string group_buf;  // delim (field 6): SGROUP body EGROUP framing
    put_tag(group_buf, 6, 3);
    group_buf += sub;
    put_tag(group_buf, 6, 4);

    rapidproto::ByteView len_span;
    rapidproto::ByteView group_span;
    REQUIRE(ed23::stream::M{rapidproto::ByteView(len_buf)}
                .decode([&](ed23::stream::M::child, ed23::stream::M inner) {
                    len_span = inner.rp_bytes();
                })
                .ok());
    REQUIRE(ed23::stream::M{rapidproto::ByteView(group_buf)}
                .decode([&](ed23::stream::M::delim, ed23::stream::M inner) {
                    group_span = inner.rp_bytes();
                })
                .ok());
    CHECK(len_span == rapidproto::ByteView(sub));    // payload only, no length prefix
    CHECK(group_span == rapidproto::ByteView(sub));  // body only, framing excluded
    // Pointer identity: the span aliases the input buffer (zero-copy), not a match elsewhere.
    CHECK(len_span.data() == len_buf.data() + 2);      // after the 1-byte tag + 1-byte length
    CHECK(group_span.data() == group_buf.data() + 1);  // after the 1-byte SGROUP tag
    // The root decoder's span is the whole input.
    CHECK(ed23::stream::M{rapidproto::ByteView(len_buf)}.rp_bytes() ==
          rapidproto::ByteView(len_buf));
}
