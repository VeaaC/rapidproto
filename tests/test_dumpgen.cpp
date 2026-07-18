// Generated debug-dumper tests. Two kinds of oracle: (1) a golden test that regenerates each
// *.rp.dump.hpp and compares byte-for-byte to the checked-in copy; (2) runtime-output tests that
// decode real wire fixtures through the arena decoder and assert the EXACT dumped string from
// rp_dump_string. The compile-smoke below (#including every generated debug golden) also makes the
// generated dumpers -- and, transitively, the arena headers they include -- valid C++.
//
// Regenerate after an intentional generator change with: tests/regen_goldens.sh (the in-test
// RAPIDPROTO_REGEN_GOLDEN mode can't rebuild this binary when a change breaks the headers it
// #includes).

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>

#include "arena_modes_profile.hpp"
#include "rapidproto/arena_runtime.hpp"  // Arena: decode the fixtures for the runtime-output tests
#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/dumpgen/generator.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/runtime.hpp"  // ByteView
// Checked-in generated debug headers (compile-smoke: they -- and the arena headers they #include --
// must be valid C++). These also supply the rp_dump_string overloads the runtime tests call.
// IWYU pragma: begin_keep
#include "dumpgen_golden/arena_layout.rp.dump.hpp"
#include "dumpgen_golden/arena_manyreq.rp.dump.hpp"  // >64 required
#include "dumpgen_golden/arena_modes.rp.dump.hpp"    // field modes: dropped fields gone, raw as hex
#include "dumpgen_golden/arena_naming.rp.dump.hpp"   // identifier dedup: must compile
#include "dumpgen_golden/arena_unknown.rp.dump.hpp"  // --unknown-present: has_unknown_fields marker
#include "dumpgen_golden/editions2023.rp.dump.hpp"
#include "dumpgen_golden/editions2024.rp.dump.hpp"
#include "dumpgen_golden/main.rp.dump.hpp"  // cross-file imports + shared-enum dumper guard (see dep.proto)
#include "dumpgen_golden/prefixed/main.rp.dump.hpp"  // --namespace-prefix + imports
#include "dumpgen_golden/proto2.rp.dump.hpp"
#include "dumpgen_golden/proto3.rp.dump.hpp"
#include "dumpgen_golden/samepkg_a.rp.dump.hpp"  // same-package multi-file (pulls samepkg_b)
#include "dumpgen_golden/weakmain.rp.dump.hpp"   // weak import (pulls weakdep)
#include "dumpgen_golden/wire_all.rp.dump.hpp"   // group + packed (dumped as a group)
#include "dumpgen_golden/xref.rp.dump.hpp"
#include "dumpgen_golden/xref_prefixed/xref.rp.dump.hpp"  // --namespace-prefix=rp -> namespace rp::xr
// IWYU pragma: end_keep

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

// Build the arena LayoutSet the same way the CLI does (and test_arenagen.cpp does), then emit the
// debug header the dumpgen way. `modes` is inactive unless the caller resolved a selection.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): include dir, entry file, namespace prefix
std::string generate(const std::string& dir, const std::string& entry,
                     const std::string& prefix = {}) {
    ResolverConfig config;
    config.include_paths = {dir};
    auto resolved = resolve(dir + "/" + entry, config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    const codegen::CppNameTable names =
        codegen::build_cpp_names(set.files.front(), set.files, codegen::namespace_of(prefix));
    const arenagen::FieldModes modes;  // inactive: every field materializes
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    return dumpgen::generate_header(set.files.back(), names, layouts);
}
std::string generate_corpus(const std::string& entry, const std::string& prefix = {}) {
    return generate(RAPIDPROTO_CORPUS_DIR, entry, prefix);
}

// arena_modes under the shared `lean` profile (same selection the arena golden uses): the dumper
// walks the resulting accessors (dropped fields gone, raw payloads rendered as bytes).
std::string generate_modes_golden() {
    ResolverConfig config;
    config.include_paths = {RAPIDPROTO_CORPUS_DIR};
    auto resolved = resolve(std::string(RAPIDPROTO_CORPUS_DIR) + "/arena_modes.proto", config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    const arenagen::FieldModes modes = test::arena_modes_profile(set, symbols);
    const codegen::CppNameTable names = codegen::build_cpp_names(set.files.front(), set.files, "");
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    return dumpgen::generate_header(set.files.back(), names, layouts);
}

// arena_unknown under --unknown-present: every message reserves its has_unknown_fields() bit, so the
// dumper emits the "has_unknown_fields": true marker (bit-only -- no unknown data is retained).
std::string generate_unknown_present_golden() {
    ResolverConfig config;
    config.include_paths = {RAPIDPROTO_CORPUS_DIR};
    auto resolved = resolve(std::string(RAPIDPROTO_CORPUS_DIR) + "/arena_unknown.proto", config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();
    arenagen::FieldModesSpec spec;
    spec.unknown_all = true;
    auto resolved_modes = arenagen::resolve_field_modes(spec, set, symbols);
    REQUIRE(resolved_modes.is_ok());
    const arenagen::FieldModes modes = std::move(resolved_modes).value();
    const codegen::CppNameTable names = codegen::build_cpp_names(set.files.front(), set.files, "");
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    return dumpgen::generate_header(set.files.back(), names, layouts);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): expected vs actual, distinct roles
std::string first_difference(const std::string& expected, const std::string& actual) {
    std::istringstream exp(expected);
    std::istringstream act(actual);
    std::string exp_line;
    std::string act_line;
    int number = 1;
    while (true) {
        const bool exp_ok = static_cast<bool>(std::getline(exp, exp_line));
        const bool act_ok = static_cast<bool>(std::getline(act, act_line));
        if (!exp_ok && !act_ok) {
            return "(no line difference; trailing-newline mismatch?)";
        }
        if (exp_ok != act_ok || exp_line != act_line) {
            return "first diff at line " + std::to_string(number) +
                   ":\n  expected: " + (exp_ok ? exp_line : "<eof>") +
                   "\n  actual:   " + (act_ok ? act_line : "<eof>");
        }
        ++number;
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): golden name vs generated content
void check_golden(const std::string& name, const std::string& actual) {
    const std::string golden =
        std::string(RAPIDPROTO_DUMPGEN_GOLDEN_DIR) + "/" + name + ".rp.dump.hpp";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    if (std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr) {
        std::ofstream(golden, std::ios::binary) << actual;
        WARN("regenerated dumpgen golden: " << name);
        return;
    }
    INFO("golden: " << name);
    INFO(first_difference(read_file(golden), actual));
    CHECK(actual == read_file(golden));
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

}  // namespace

TEST_CASE("dumpgen: generated headers match the goldens", "[dumpgen]") {
    check_golden("arena_layout", generate_corpus("arena_layout.proto"));
    check_golden("arena_manyreq", generate_corpus("arena_manyreq.proto"));
    check_golden("arena_naming", generate_corpus("arena_naming.proto"));
    check_golden("proto2", generate_corpus("proto2.proto"));
    check_golden("proto3", generate_corpus("proto3.proto"));
    check_golden("editions2023", generate_corpus("editions2023.proto"));
    check_golden("editions2024", generate_corpus("editions2024.proto"));
    // field modes under the shared `lean` profile: dropped fields gone, raw payloads as bytes.
    check_golden("arena_modes", generate_modes_golden());
    // --unknown-present: the "has_unknown_fields": true marker is emitted per message.
    check_golden("arena_unknown", generate_unknown_present_golden());
    check_golden("xref", generate_corpus("xref.proto"));
    check_golden("xref_prefixed/xref", generate_corpus("xref.proto", "rp"));
    check_golden("wire_all", generate(RAPIDPROTO_WIRE_FIXTURE_DIR, "wire_all.proto"));
    // Cross-file imports (distinct packages): a debug header #includes only its own arena header
    // (which transitively pulls the deps' arena headers), so each file gets its own dumper.
    const std::string imports = std::string(RAPIDPROTO_CORPUS_DIR) + "/imports";
    check_golden("dep", generate(imports, "dep.proto"));
    check_golden("pub", generate(imports, "pub.proto"));
    check_golden("forward", generate(imports, "forward.proto"));
    check_golden("main", generate(imports, "main.proto"));
    check_golden("samepkg_b", generate(imports, "samepkg_b.proto"));
    check_golden("samepkg_a", generate(imports, "samepkg_a.proto"));
    check_golden("weakdep", generate(imports, "weakdep.proto"));
    check_golden("weakmain", generate(imports, "weakmain.proto"));
    // --namespace-prefix + imports: the oneof visit-tag structs the dumper references come from the
    // same deduped SynthNames the arena header declared, so the prefixed closure compiles.
    check_golden("prefixed/main", generate(imports, "main.proto", "rp"));
    check_golden("prefixed/dep", generate(imports, "dep.proto", "rp"));
}

// ── runtime output: decode a real fixture through the arena decoder, dump, assert the EXACT text ──

TEST_CASE("dumpgen: scalars fixture dumps every scalar kind (bytes->hex, enum by name)",
          "[dumpgen]") {
    const std::string bin = fixture("scalars.bin");
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    const std::string expected = R"({
  "i32": -7,
  "i64": 42,
  "u32": 300,
  "u64": 1000000,
  "s32": -5,
  "s64": -2,
  "f32": 16909060,
  "f64": 72623859790382856,
  "sf32": -2,
  "sf64": -3,
  "b": true,
  "s": "hi",
  "by": "0001ff",
  "fl": 1.5,
  "db": -2.25,
  "color": "RED",
  "packed_nums": [1, 2, 300],
  "expanded_nums": [3, 4]
})";
    CHECK(p2::rp_dump_string(*m) == expected);
}

TEST_CASE("dumpgen: msg fixture dumps nested msg, repeated, map, oneof (defaults omitted)",
          "[dumpgen]") {
    const std::string bin = fixture("msg.bin");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    // The empty innermost `self` collapses: only the fields present on the wire appear, and the
    // implicit-presence zero defaults are omitted.
    const std::string expected = R"({
  "implicit_i": 10,
  "explicit_i": 20,
  "name": "abc",
  "state": "ON",
  "self": {"implicit_i": 99},
  "nums": [1, 2, 3],
  "unpacked": [4, 5],
  "states": ["ON", "UNKNOWN"],
  "counts": {"x": 1, "y": 2},
  "a": 7
})";
    CHECK(p3::rp_dump_string(*m) == expected);
}

TEST_CASE("dumpgen: container fixture dumps map-of-messages and an array of objects", "[dumpgen]") {
    const std::string bin = fixture("container.bin");
    Arena arena;
    const p2::Container* c = p2::Container::decode(ByteView(bin), arena);
    REQUIRE(c != nullptr);
    // Fits within the default width -> single compact line.
    const std::string expected =
        R"({"by_name": {"alpha": {"x": 11}, "beta": {"x": 22}}, "by_id": {"1": "RED", "2": "NEG"}})";
    CHECK(p2::rp_dump_string(*c) == expected);
}

TEST_CASE("dumpgen: all_wire fixture dumps a group field (delimited sub-message)", "[dumpgen]") {
    const std::string bin = fixture("all_wire.bin");
    Arena arena;
    const ::wire::AllWire* m = ::wire::AllWire::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    // `g` is a group -- decoded through the identical nested-message accessor, dumped like any msg.
    const std::string expected = R"({
  "zz": -1234567890123,
  "db": 3.14159,
  "fx": 305419896,
  "s": "wire",
  "by": "deadbeef",
  "nested": {"zz": 7},
  "packed": [10, 20, 30],
  "g": {"a": 99},
  "oi": 5
})";
    CHECK(::wire::rp_dump_string(*m) == expected);
}

// The width knob drives the adaptive compact/multi-line choice PER group. At width 20 the top object
// breaks (as always when it overflows), and the nested `self`/`states`/`counts` groups also break to
// multi-line, while the short `nums`/`unpacked` arrays stay compact. (The plan's width-40 renders
// identical to the default for p3::Msg -- nothing adaptive is exercised there -- so width 20 is used
// to actually pin the nested-breaking behavior.)
TEST_CASE("dumpgen: width knob drives the adaptive nested-group layout", "[dumpgen]") {
    const std::string bin = fixture("msg.bin");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    const std::string expected = R"({
  "implicit_i": 10,
  "explicit_i": 20,
  "name": "abc",
  "state": "ON",
  "self": {
    "implicit_i": 99
  },
  "nums": [1, 2, 3],
  "unpacked": [4, 5],
  "states": [
    "ON",
    "UNKNOWN"
  ],
  "counts": {
    "x": 1,
    "y": 2
  },
  "a": 7
})";
    CHECK(p3::rp_dump_string(*m, 20) == expected);
}

TEST_CASE("dumpgen: DumpOptions.skip omits fields by qualified path", "[dumpgen]") {
    const std::string bin = fixture("msg.bin");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    SECTION("skip across kinds (scalar, message, repeated, map) -> the rest stays") {
        rapidproto::dump::DumpOptions opt;
        opt.skip = {"implicit_i", "name", "state", "self", "nums", "unpacked", "states", "counts"};
        CHECK(p3::rp_dump_string(*m, opt) == R"({"explicit_i": 20, "a": 7})");
    }
    SECTION("a qualified path skips a NESTED field (self.implicit_i) -> self dumps empty") {
        rapidproto::dump::DumpOptions opt;
        opt.skip = {"implicit_i", "explicit_i", "name",   "state", "nums",
                    "unpacked",   "states",     "counts", "a",     "self.implicit_i"};
        CHECK(p3::rp_dump_string(*m, opt) == R"({"self": {}})");
    }
    SECTION("a leaf name skips it only at that path, not the same name nested elsewhere") {
        // "implicit_i" (top-level) is skipped, but "self.implicit_i" is a different path and stays.
        rapidproto::dump::DumpOptions opt;
        opt.skip = {"implicit_i", "explicit_i", "name",   "state", "nums",
                    "unpacked",   "states",     "counts", "a"};
        CHECK(p3::rp_dump_string(*m, opt) == R"({"self": {"implicit_i": 99}})");
    }
}

TEST_CASE("dumpgen: DumpOptions.indent starts the dump at a nesting level", "[dumpgen]") {
    const std::string bin = fixture("msg.bin");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    // Force multi-line (width 20) and start two levels deep: the opening brace sits at the caller's
    // cursor, every continuation line indents two extra levels (4 spaces), and the closing brace aligns
    // to the start level -- so the block drops cleanly under a surrounding `"key": ` at that indent.
    rapidproto::dump::DumpOptions opt;
    opt.width = 20;
    opt.indent = 2;
    const std::string expected = R"({
      "implicit_i": 10,
      "explicit_i": 20,
      "name": "abc",
      "state": "ON",
      "self": {
        "implicit_i": 99
      },
      "nums": [
        1,
        2,
        3
      ],
      "unpacked": [
        4,
        5
      ],
      "states": [
        "ON",
        "UNKNOWN"
      ],
      "counts": {
        "x": 1,
        "y": 2
      },
      "a": 7
    })";
    CHECK(p3::rp_dump_string(*m, opt) == expected);
}

TEST_CASE("dumpgen: a message with no set fields dumps as an empty object", "[dumpgen]") {
    Arena arena;
    // An empty p3::Msg: all fields absent / at their (omitted) implicit defaults -> {}.
    const p3::Msg* m = p3::Msg::decode(ByteView(std::string()), arena);
    REQUIRE(m != nullptr);
    CHECK(p3::rp_dump_string(*m) == "{}");
}

TEST_CASE("dumpgen: an open-enum value outside the schema dumps as UNKNOWN(<n>)", "[dumpgen]") {
    // A hand-built p3::Msg with its open enum field `state` (field 4) set to 99, a value no
    // enumerator carries: rp_dump_enum_name returns nullptr, so the dumper renders the numeric
    // fallback UNKNOWN(99) rather than a name.
    std::string buf;
    put_tag(buf, 4, 0);  // state: Varint
    put_varint(buf, 99);
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    const std::string dump = p3::rp_dump_string(*m);
    CHECK(dump.find("\"UNKNOWN(99)\"") != std::string::npos);
    CHECK(dump == R"j({"state": "UNKNOWN(99)"})j");
}

TEST_CASE("dumpgen: a string field escapes JSON control/quote/backslash characters", "[dumpgen]") {
    // A hand-built p3::Msg whose `name` (field 3) carries a quote, backslash, newline, tab, and a
    // raw 0x01 control byte: the dumper escapes each per JSON -- the named escapes, and \u0001 for
    // the otherwise-unnamed control byte -- passing nothing through raw.
    std::string raw = "q\"b\\";  // q, quote, b, backslash
    raw += '\n';                 // newline -> \n
    raw += '\t';                 // tab -> \t
    raw += '\x01';               // control byte -> \u0001
    std::string buf;
    put_tag(buf, 3, 2);  // name: Len
    put_varint(buf, raw.size());
    buf += raw;
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    const std::string dump = p3::rp_dump_string(*m);
    CHECK(dump == R"({"name": "q\"b\\\n\t\u0001"})");
}

TEST_CASE("dumpgen: field-modes dump omits dropped fields and renders raw payloads as hex",
          "[dumpgen]") {
    // A hand-built fm::Holder decoded under the `lean` profile: the dropped field `debug` (field 2)
    // must not appear, and the raw message fields `blob` (7) / `req_blob` (13) surface as the hex of
    // their borrowed payloads (the sub-message body bytes), never as nested objects.
    const std::string body = [] {
        std::string b;
        put_tag(b, 1, 2);  // Blob.payload: Len
        const std::string p = "\x01\x02\xff";
        put_varint(b, p.size());
        b += p;
        return b;
    }();  // 0a 03 01 02 ff
    std::string buf;
    put_tag(buf, 1, 0);  // keep
    put_varint(buf, 7);
    put_tag(buf, 2, 0);  // debug: DROPPED by the profile
    put_varint(buf, 123);
    put_tag(buf, 5, 0);  // must (required)
    put_varint(buf, 9);
    put_tag(buf, 7, 2);  // blob (raw, singular optional): Len
    put_varint(buf, body.size());
    buf += body;
    put_tag(buf, 13, 2);  // req_blob (raw, required): Len
    put_varint(buf, body.size());
    buf += body;
    Arena arena;
    const fm::Holder* h = fm::Holder::decode(ByteView(buf), arena);
    REQUIRE(h != nullptr);
    const std::string dump = fm::rp_dump_string(*h);
    // The dropped field is absent; keep/must are materialized; raw payloads are the body's hex
    // (0a 03 01 02 ff = tag(1,Len) len=3 <01 02 ff>).
    CHECK(dump.find("\"debug\"") == std::string::npos);
    CHECK(dump == R"({"keep": 7, "must": 9, "blob": "0a030102ff", "req_blob": "0a030102ff"})");
}

TEST_CASE("dumpgen: --unknown-present message reports has_unknown_fields", "[dumpgen]") {
    // au::Holder built under --unknown-present, decoded from a buffer with a field unknown to it: the
    // dumper surfaces the reserved marker (bit-only; no unknown data is retained).
    std::string buf;
    put_tag(buf, 9, 0);  // field 9: unknown to Holder
    put_varint(buf, 1);
    Arena arena;
    const au::Holder* h = au::Holder::decode(ByteView(buf), arena);
    REQUIRE(h != nullptr);
    const std::string dump = au::rp_dump_string(*h);
    CHECK(dump.find("\"has_unknown_fields\": true") != std::string::npos);
    CHECK(dump == R"({"has_unknown_fields": true})");
}
