// Generated debug-dumper tests. Two kinds of oracle: (1) a golden test that regenerates each
// *.rp.dump.hpp and compares byte-for-byte to the checked-in copy; (2) runtime-output tests that
// decode real wire fixtures through the arena decoder and assert the EXACT dumped string from
// rapidproto::dump. The compile-smoke below (#including every generated debug golden) also makes the
// generated dumpers -- and, transitively, the arena headers they include -- valid C++.
//
// Regenerate after an intentional generator change with: tests/regen_goldens.sh (the in-test
// RAPIDPROTO_REGEN_GOLDEN mode can't rebuild this binary when a change breaks the headers it
// #includes).

#include <catch_amalgamated.hpp>

#include "golden_file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "arena_modes_profile.hpp"
#include "common_golden_sweep.hpp"
#include "rapidproto/arena_runtime.hpp"  // Arena: decode the fixtures for the runtime-output tests
#include "rapidproto/arenagen/generator.hpp"
#include "rapidproto/arenagen/layout.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/dump_runtime.hpp"  // dump_detail::Writer / DumpOptions: driven directly below
#include "rapidproto/dumpgen/generator.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/runtime.hpp"  // ByteView
// Checked-in generated debug headers (compile-smoke: they -- and the arena headers they #include --
// must be valid C++). These also supply the dumper<T> hooks the runtime tests dump through.
// IWYU pragma: begin_keep
#include "dumpgen_golden/arena_layout.rp.dump.hpp"
#include "dumpgen_golden/arena_manyreq.rp.dump.hpp"  // >64 required
#include "dumpgen_golden/arena_modes.rp.dump.hpp"    // field modes: dropped fields gone, raw as hex
#include "dumpgen_golden/arena_naming.rp.dump.hpp"   // identifier dedup: must compile
#include "dumpgen_golden/arena_unknown.rp.dump.hpp"  // --unknown-present: has_unknown_fields marker
#include "dumpgen_golden/editions2023.rp.dump.hpp"
#include "dumpgen_golden/editions2024.rp.dump.hpp"
#include "dumpgen_golden/main.rp.dump.hpp"  // cross-file imports + shared-enum dumper guard (see dep.proto)
// Enum values that sanitize alike stay distinct; also pulls in naming.rp.hpp, whose shadowing
// fixtures only compile if the out-of-line definitions name themselves absolutely.
#include "dumpgen_golden/naming.rp.dump.hpp"
#include "dumpgen_golden/prefixed/main.rp.dump.hpp"  // --namespace-prefix + imports
#include "dumpgen_golden/proto2.rp.dump.hpp"
#include "dumpgen_golden/proto3.rp.dump.hpp"
#include "dumpgen_golden/rppkg.rp.dump.hpp"  // package `rapidproto`: keeps its name under the roots
#include "dumpgen_golden/samepkg_a.rp.dump.hpp"  // same-package multi-file (pulls samepkg_b)
#include "dumpgen_golden/stdpkg.rp.dump.hpp"  // package `std` -> namespace std_, not namespace std
#include "dumpgen_golden/weakmain.rp.dump.hpp"  // weak import (pulls weakdep)
#include "dumpgen_golden/wire_all.rp.dump.hpp"  // group + packed (dumped as a group)
#include "dumpgen_golden/xref.rp.dump.hpp"
#include "dumpgen_golden/xref_prefixed/xref.rp.dump.hpp"  // --namespace-prefix=pfx -> namespace pfx::arena::xr

// The generated types live under one root per model; alias each package once so the
// bodies below read as they did before the roots existed. This file uses the arena model only.
namespace au = rp::arena::au;
namespace fm = rp::arena::fm;
namespace nm = rp::arena::nm;
namespace p2 = rp::arena::p2;
namespace p3 = rp::arena::p3;
// IWYU pragma: end_keep

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

using rapidproto::test::read_file;

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
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix(prefix),
                                 std::string(codegen::kArenaRoot));
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
    const codegen::CppNameTable names =
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kArenaRoot));
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
    const codegen::CppNameTable names =
        codegen::build_cpp_names(set.files.front(), set.files, codegen::effective_ns_prefix({}),
                                 std::string(codegen::kArenaRoot));
    arenagen::LayoutOptions options;
    options.modes = &modes;
    const arenagen::LayoutSet layouts = arenagen::plan_layouts(set, symbols, options);
    return dumpgen::generate_header(set.files.back(), names, layouts);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): golden name vs generated content
void check_golden(const std::string& name, const std::string& actual) {
    test::check_golden(std::string(RAPIDPROTO_DUMPGEN_GOLDEN_DIR) + "/" + name + ".rp.dump.hpp",
                       name, actual);
}

// Drive the runtime Writer the way generated array code does (group + entry_sep + a raw value), so
// the grid layout can be exercised over shapes no wire fixture supplies. `open`/`close` pick array
// vs object framing.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the bracket pair is written as a literal pair
std::string dump_entries(const std::vector<std::string>& cells, std::size_t width, char open = '[',
                         char close = ']') {
    std::ostringstream os;
    rapidproto::dump_detail::Writer w(os, width);
    w.group(open, close, [&] {
        bool first = true;
        for (const std::string& cell : cells) {
            w.entry_sep(first);
            w.os() << cell;
            if (w.overflowed()) {
                break;
            }
        }
    });
    return os.str();
}

// True if any line ends in a space -- grid padding must never leave trailing whitespace.
bool has_trailing_space(const std::string& s) {
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == ' ') {
            return true;
        }
    }
    return false;
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

// A float/double field body: the IEEE bits, little-endian (wire types I32 / I64). Assembled by
// shifting the bit pattern rather than memcpy-ing the object, so the bytes land in wire order on a
// big-endian host too.
template <class T>
void put_ieee(std::string& b, T value) {
    using Bits =
        std::conditional_t<sizeof(T) == sizeof(std::uint32_t), std::uint32_t, std::uint64_t>;
    Bits bits = 0;
    std::memcpy(&bits, &value, sizeof(T));
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        b.push_back(static_cast<char>((bits >> (i * 8U)) & 0xFFU));
    }
}

// Dump a p2::Scalars through a stream deliberately carrying a digit-grouping locale and
// hex/showpos/uppercase/showbase flags -- the state a dump must neither read nor disturb -- and
// return the text. The locale is built from a facet rather than a named system one, so the test
// needs nothing installed. Asserts here that the stream came back exactly as it was handed over.
std::string dump_through_hostile_stream(std::size_t width) {
    struct Grouping : std::numpunct<char> {
        char do_thousands_sep() const override { return '.'; }
        std::string do_grouping() const override { return "\3"; }
        char do_decimal_point() const override { return ','; }
    };
    const std::locale grouping(std::locale::classic(), new Grouping);  // the locale owns the facet

    std::string wire;
    put_tag(wire, 1, 0);  // i32 (required): Varint
    put_varint(wire, 1);
    put_tag(wire, 4, 0);  // u64: Varint -- large enough for grouping to show
    put_varint(wire, 1234567890);
    put_tag(wire, 15, 1);  // db: I64
    put_ieee(wire, 1234567.25);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(wire), arena);
    REQUIRE(m != nullptr);

    std::ostringstream os;
    os.imbue(grouping);
    os << std::hex << std::showpos << std::uppercase << std::showbase;
    const std::ios::fmtflags flags = os.flags();
    rapidproto::dump(os, *m, width);
    CHECK(os.getloc() == grouping);
    CHECK(os.flags() == flags);
    return os.str();
}

// The values dump_through_hostile_stream's message carries, as they must appear whatever the sink
// was set to: plain decimal digits, no `+`, no grouping separators, an unmangled double.
void check_hostile_stream_ignored(const std::string& text) {
    CHECK(text.find("1234567890") != std::string::npos);
    CHECK(text.find("499602D2") == std::string::npos);       // std::hex
    CHECK(text.find("+1234567890") == std::string::npos);    // std::showpos
    CHECK(text.find("1.234.567.890") == std::string::npos);  // the grouping locale
    CHECK(text.find("1234567.25") != std::string::npos);
}

}  // namespace

TEST_CASE("dumpgen: generated headers match the goldens", "[dumpgen]") {
    check_golden("arena_layout", generate_corpus("arena_layout.proto"));
    check_golden("arena_manyreq", generate_corpus("arena_manyreq.proto"));
    check_golden("arena_naming", generate_corpus("arena_naming.proto"));
    check_golden("naming", generate_corpus("naming.proto"));
    // Package shapes: `namespace std` would be UB that compiles silently, so the byte-compare
    // is the only thing that catches a regression; a package named `rapidproto` keeps its
    // spelling under the roots, clear of the runtime's own namespace.
    const std::string nsedge_dir = std::string(RAPIDPROTO_CORPUS_DIR) + "/nsedge";
    check_golden("stdpkg", generate(nsedge_dir, "stdpkg.proto"));
    check_golden("rppkg", generate(nsedge_dir, "rppkg.proto"));
    check_golden("proto2", generate_corpus("proto2.proto"));
    check_golden("proto3", generate_corpus("proto3.proto"));
    check_golden("editions2023", generate_corpus("editions2023.proto"));
    check_golden("editions2024", generate_corpus("editions2024.proto"));
    // field modes under the shared `lean` profile: dropped fields gone, raw payloads as bytes.
    check_golden("arena_modes", generate_modes_golden());
    // --unknown-present: the "has_unknown_fields": true marker is emitted per message.
    check_golden("arena_unknown", generate_unknown_present_golden());
    check_golden("xref", generate_corpus("xref.proto"));
    check_golden("xref_prefixed/xref", generate_corpus("xref.proto", "pfx"));
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
    check_golden("prefixed/main", generate(imports, "main.proto", "pfx"));
    check_golden("prefixed/dep", generate(imports, "dep.proto", "pfx"));
    check_golden("prefixed/pub", generate(imports, "pub.proto", "pfx"));
    check_golden("prefixed/forward", generate(imports, "forward.proto", "pfx"));
}

// Every `<stem>.rp.common.hpp` beside the dump goldens, directory-driven (see
// common_golden_sweep.hpp for why not a hand-maintained case list).
TEST_CASE("dumpgen: the common headers beside the goldens match too", "[dumpgen]") {
    test::check_all_common_goldens(
        RAPIDPROTO_DUMPGEN_GOLDEN_DIR,
        {RAPIDPROTO_CORPUS_DIR, std::string(RAPIDPROTO_CORPUS_DIR) + "/imports",
         std::string(RAPIDPROTO_CORPUS_DIR) + "/nsedge", RAPIDPROTO_WIRE_FIXTURE_DIR});
}

// A MULTI-component prefix through the dump generator (every prefixed golden is
// single-component): the dotted spelling must split in the opening namespace and in the fully
// qualified core calls. streamgen and arenagen carry the twins of this check.
TEST_CASE("dumpgen: a dotted namespace prefix splits into nested namespaces", "[dumpgen]") {
    const std::string dotted = generate_corpus("xref.proto", "rp.dec");
    CHECK(dotted.find("namespace rp::dec::arena::xr {") != std::string::npos);
    CHECK(dotted.find("::rp::dec::arena::xr::rp_dump_detail::rp_dump_write") != std::string::npos);
}

// ── generated namespace layout ───────────────────────────────────────────────────────────────────

// Only the two PUBLIC entry points may sit in the message's namespace; the Writer-threaded core lives
// in <pkg>::rp_dump_detail and is reached by a FULLY QUALIFIED call (ADL cannot see into a
// sub-namespace). The namespace is read from CppNameTable::type_ns rather than reconstructed from the
// type FQN, so these assertions pin the shapes an FQN cannot express: a dotted package, and no package
// at all. No corpus file outside tests/corpus/nsedge/ has either, and these have no golden -- they
// assert on the generated TEXT, so they add no cross-generator golden churn.
TEST_CASE("dumpgen: generated internals live in sub-namespaces, not the public ones", "[dumpgen]") {
    const std::string nsedge = std::string(RAPIDPROTO_CORPUS_DIR) + "/nsedge";

    SECTION("a multi-component package qualifies against the FULL package namespace") {
        const std::string out = generate(nsedge, "deep.proto");
        CHECK(out.find("namespace rp::arena::com::example::deep {") != std::string::npos);
        CHECK(out.find("namespace rp_dump_detail {") != std::string::npos);
        // The nested Inner is a CLASS member, so its dumper is still qualified with the PACKAGE
        // namespace -- never `rp::arena::com::example::deep::Outer::rp_dump_detail`.
        CHECK(out.find("::rp::arena::com::example::deep::rp_dump_detail::rp_dump_write") !=
              std::string::npos);
        CHECK(out.find("Outer::rp_dump_detail") == std::string::npos);
    }
    SECTION("a file with NO package still opens the model root, and qualifies against it") {
        // The FULLY QUALIFIED spellings, both of them: the bare-substring checks this section used
        // to make (`namespace rp_dump_detail {`, `::rp_dump_detail::rp_dump_write`) are contained
        // in every dump header's `...arena::rp_dump_detail::...` and matched the pre-roots
        // global-scope layout exactly as well -- a section that could not fail.
        const std::string out = generate(nsedge, "nopkg.proto");
        CHECK(out.find("namespace rp::arena {") != std::string::npos);
        CHECK(out.find("::rp::arena::rp_dump_detail::rp_dump_write") != std::string::npos);
    }
    SECTION("a cross-file call is qualified with the CALLEE's namespace, not the caller's") {
        const std::string out = generate(nsedge, "xpkg.proto");
        CHECK(out.find("namespace rp::arena::other {") != std::string::npos);
        CHECK(out.find("::rp::arena::com::example::deep::rp_dump_detail::rp_dump_write") !=
              std::string::npos);
    }
    SECTION("--namespace-prefix is carried into the qualified call") {
        // A NON-DEFAULT prefix, or this asserts the same string the unprefixed sections above do
        // and says nothing about the flag.
        const std::string out = generate(nsedge, "xpkg.proto", "pfx");
        CHECK(out.find("::pfx::arena::com::example::deep::rp_dump_detail::rp_dump_write") !=
              std::string::npos);
        CHECK(out.find("::rp::arena::com::example::deep::") == std::string::npos);
    }
    SECTION("the dumper<T> hooks are emitted after every core they forward to") {
        const std::string out = generate(nsedge, "deep.proto");
        const auto detail_close = out.find("}  // namespace rp_dump_detail");
        const auto hook = out.find("struct dumper<");
        REQUIRE(detail_close != std::string::npos);
        REQUIRE(hook != std::string::npos);
        // Order is the whole reason this is a class template rather than an overload set: the hook
        // names the core in a qualified call, so the core must already be declared.
        CHECK(detail_close < hook);
        // deep.proto defines an enum, so a name table IS emitted -- into the runtime's internals
        // namespace. Generated code never opens `rapidproto` itself: that is the public surface.
        CHECK(out.find("namespace rapidproto::dump_detail {") != std::string::npos);
        CHECK(out.find("namespace rapidproto {") == std::string::npos);
        CHECK(out.find("::rapidproto::dump_detail::rp_dump_enum_name") != std::string::npos);
    }
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
    CHECK(rapidproto::dump(*m) == expected);
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
    CHECK(rapidproto::dump(*m) == expected);
}

TEST_CASE("dumpgen: container fixture dumps map-of-messages and an array of objects", "[dumpgen]") {
    const std::string bin = fixture("container.bin");
    Arena arena;
    const p2::Container* c = p2::Container::decode(ByteView(bin), arena);
    REQUIRE(c != nullptr);
    // Fits within the default width -> single compact line.
    const std::string expected =
        R"({"by_name": {"alpha": {"x": 11}, "beta": {"x": 22}}, "by_id": {"1": "RED", "2": "NEG"}})";
    CHECK(rapidproto::dump(*c) == expected);
}

TEST_CASE("dumpgen: all_wire fixture dumps a group field (delimited sub-message)", "[dumpgen]") {
    const std::string bin = fixture("all_wire.bin");
    Arena arena;
    const ::rp::arena::wire::AllWire* m = ::rp::arena::wire::AllWire::decode(ByteView(bin), arena);
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
    CHECK(rapidproto::dump(*m) == expected);
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
    "ON", "UNKNOWN"
  ],
  "counts": {
    "x": 1,
    "y": 2
  },
  "a": 7
})";
    CHECK(rapidproto::dump(*m, 20) == expected);
}

TEST_CASE("dumpgen: DumpOptions.skip omits fields by qualified path", "[dumpgen]") {
    const std::string bin = fixture("msg.bin");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(bin), arena);
    REQUIRE(m != nullptr);
    SECTION("skip across kinds (scalar, message, repeated, map) -> the rest stays") {
        rapidproto::DumpOptions opt;
        opt.skip = {"implicit_i", "name", "state", "self", "nums", "unpacked", "states", "counts"};
        CHECK(rapidproto::dump(*m, opt) == R"({"explicit_i": 20, "a": 7})");
    }
    SECTION("a qualified path skips a NESTED field (self.implicit_i) -> self dumps empty") {
        rapidproto::DumpOptions opt;
        opt.skip = {"implicit_i", "explicit_i", "name",   "state", "nums",
                    "unpacked",   "states",     "counts", "a",     "self.implicit_i"};
        CHECK(rapidproto::dump(*m, opt) == R"({"self": {}})");
    }
    SECTION("a leaf name skips it only at that path, not the same name nested elsewhere") {
        // "implicit_i" (top-level) is skipped, but "self.implicit_i" is a different path and stays.
        rapidproto::DumpOptions opt;
        opt.skip = {"implicit_i", "explicit_i", "name",   "state", "nums",
                    "unpacked",   "states",     "counts", "a"};
        CHECK(rapidproto::dump(*m, opt) == R"({"self": {"implicit_i": 99}})");
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
    rapidproto::DumpOptions opt;
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
        1, 2, 3
      ],
      "unpacked": [
        4, 5
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
    CHECK(rapidproto::dump(*m, opt) == expected);
}

// An array too wide for one line is laid out in as many aligned COLUMNS as fit, rather than one entry
// per line. Columns are sized independently (each to its own widest cell), row-major so index order
// still reads left-to-right; numeric cells right-align, everything else left-aligns.
TEST_CASE("dumpgen: a too-wide array is laid out in aligned columns", "[dumpgen]") {
    SECTION("numeric cells right-align, so digits line up under each other") {
        const std::string expected = R"([
  1,  2,  3,  4, 5, 6, 7, 8,
  9, 10, 11, 12
])";
        const std::string out =
            dump_entries({"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"}, 30);
        CHECK(out == expected);
        CHECK_FALSE(has_trailing_space(out));
    }
    SECTION("each column is sized to its OWN widest cell, not the array's") {
        // Column 3 holds only "4444", so it is 4 wide while column 0 (1, 66) stays 2.
        const std::string expected = R"([
   1, 22, 333, 4444, 5,
  66
])";
        CHECK(dump_entries({"1", "22", "333", "4444", "5", "66"}, 24) == expected);
    }
    SECTION("non-numeric cells left-align") {
        const std::string expected = R"([
  "al",  "bertrand", "cy",
  "dee"
])";
        const std::string out =
            dump_entries({R"("al")", R"("bertrand")", R"("cy")", R"("dee")"}, 30);
        CHECK(out == expected);
        CHECK_FALSE(has_trailing_space(out));
    }
}

// The grid is only ever an improvement on one-entry-per-line: everything it cannot lay out cleanly
// falls back to the previous behaviour, and a group that still fits on one line never reaches it.
TEST_CASE("dumpgen: a group the column grid can't handle falls back unchanged", "[dumpgen]") {
    SECTION("when not even two columns fit, fall back to one entry per line") {
        const std::string expected = R"([
  "averyverylongvalue",
  "anotherlongvalue"
])";
        CHECK(dump_entries({R"("averyverylongvalue")", R"("anotherlongvalue")"}, 20) == expected);
    }
    SECTION("an OBJECT never grids -- packing \"key\": value cells reads worse") {
        const std::string expected = R"({
  "a": 1,
  "b": 2,
  "c": 3
})";
        CHECK(dump_entries({R"("a": 1)", R"("b": 2)", R"("c": 3)"}, 20, '{', '}') == expected);
    }
    SECTION("a single entry too wide for the budget is just printed on its own line") {
        const std::string expected = R"([
  "averylongsinglevalue"
])";
        CHECK(dump_entries({R"("averylongsinglevalue")"}, 10) == expected);
    }
    SECTION("a group that still fits on one line is untouched") {
        CHECK(dump_entries({"1", "2", "3"}, 30) == "[1, 2, 3]");
    }
}

// Paths the flat cell-list helper can't express: a cell that is itself a group (which is what
// exercises suspending cell collection inside a nested group), a field following a grid, and the
// collection cap that guards against materializing an enormous array.
TEST_CASE("dumpgen: an array of objects grids them as whole cells", "[dumpgen]") {
    {
        std::ostringstream os;
        rapidproto::dump_detail::Writer w(os, 26);
        w.group('[', ']', [&] {
            bool first = true;
            for (int i = 1; i <= 4; ++i) {
                w.entry_sep(first);
                w.group('{', '}', [&] {
                    bool inner = true;
                    w.entry_sep(inner);
                    w.key("a");
                    w.os() << i;
                });
            }
        });
        const std::string expected = R"([
  {"a": 1}, {"a": 2},
  {"a": 3}, {"a": 4}
])";
        CHECK(os.str() == expected);
    }
}

TEST_CASE("dumpgen: a field after a column grid resumes at the enclosing indent", "[dumpgen]") {
    {
        std::ostringstream os;
        rapidproto::dump_detail::Writer w(os, 30);
        w.group('{', '}', [&] {
            bool first = true;
            w.entry_sep(first);
            w.key("nums");
            w.group('[', ']', [&] {
                bool inner = true;
                for (int i = 1; i <= 12; ++i) {
                    w.entry_sep(inner);
                    w.os() << i;
                }
            });
            w.entry_sep(first);
            w.key("a");
            w.os() << 7;
        });
        const std::string expected = R"({
  "nums": [
    1,  2,  3,  4, 5, 6, 7, 8,
    9, 10, 11, 12
  ],
  "a": 7
})";
        CHECK(os.str() == expected);
    }
}

TEST_CASE("dumpgen: an array past the cell cap falls back without dropping elements", "[dumpgen]") {
    {
        // The cap stops collection mid-array, so those cells are incomplete -- rendering them as a
        // grid would silently swallow entries. It must fall back to one per line instead.
        constexpr int kCount = 5000;
        std::ostringstream os;
        rapidproto::dump_detail::Writer w(os, 200);
        w.group('[', ']', [&] {
            bool first = true;
            for (int i = 0; i < kCount; ++i) {
                w.entry_sep(first);
                w.os() << 7;
                if (w.overflowed()) {
                    break;
                }
            }
        });
        const std::string out = os.str();
        CHECK(std::count(out.begin(), out.end(), '7') == kCount);       // every element survived
        CHECK(std::count(out.begin(), out.end(), '\n') == kCount + 1);  // one per line, not a grid
    }
}

TEST_CASE("dumpgen: a message with no set fields dumps as an empty object", "[dumpgen]") {
    Arena arena;
    // An empty p3::Msg: all fields absent / at their (omitted) implicit defaults -> {}.
    const p3::Msg* m = p3::Msg::decode(ByteView(std::string()), arena);
    REQUIRE(m != nullptr);
    CHECK(rapidproto::dump(*m) == "{}");
}

TEST_CASE("dumpgen: two enum values that sanitize alike still dump differently", "[dumpgen]") {
    // `enum E { decode = 0; decode_ = 1; }` -- protoc-valid. `decode` is reserved, so BOTH names
    // sanitize to `decode_`; the generated enum breaks the tie by suffixing, and the dumper prints
    // the enumerator it actually declared. Rendering the two values identically would make the dump
    // ambiguous about which one the wire carried.
    const auto dump_of = [](std::uint64_t value) {
        std::string buf;
        put_tag(buf, 5, 0);  // nm::M::e: Varint
        put_varint(buf, value);
        Arena arena;
        const nm::M* m = nm::M::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        return rapidproto::dump(*m);
    };
    CHECK(dump_of(0) == R"({"e": "decode_"})");
    CHECK(dump_of(1) == R"({"e": "decode__"})");
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
    const std::string dump = rapidproto::dump(*m);
    CHECK(dump.find("\"UNKNOWN(99)\"") != std::string::npos);
    CHECK(dump == R"j({"state": "UNKNOWN(99)"})j");
}

TEST_CASE("dumpgen: a bool renders true/false inside a COMPACT group", "[dumpgen]") {
    // A group that fits the width budget is rendered into the Writer's scratch buffer and spliced,
    // and that buffer carries its own formatting flags. write_bool spells the literal out, so the
    // compact and multi-line paths agree rather than depending on which sink is active.
    std::string inner;
    put_tag(inner, 4, 0);  // Inner.flag: Varint
    put_varint(inner, 1);
    std::string group;
    put_tag(group, 2, 0);  // MyGroup.a: Varint
    put_varint(group, 5);
    put_tag(group, 3, 3);  // MyGroup.Inner: SGROUP
    group += inner;
    put_tag(group, 3, 4);  // EGROUP
    std::string buf;
    put_tag(buf, 1, 3);  // WithGroup.MyGroup: SGROUP
    buf += group;
    put_tag(buf, 1, 4);  // EGROUP
    Arena arena;
    const p2::WithGroup* m = p2::WithGroup::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(rapidproto::dump(*m) == R"({"mygroup": {"a": 5, "inner": {"flag": true}}})");
    // The same value on the multi-line path (width 0 forces every group to expand) reads the same.
    CHECK(rapidproto::dump(*m, 0).find("\"flag\": true") != std::string::npos);
}

TEST_CASE("dumpgen: a bool map KEY renders as true/false, not 1/0", "[dumpgen]") {
    // A map key is rendered inside the JSON object's quotes, on a path separate from the value's.
    // proto allows `bool` there, so that path needs the same true/false literal the value side gets.
    const auto entry = [](bool key, std::string_view value) {
        std::string body;
        put_tag(body, 1, 0);  // MapEntry.key: Varint
        put_varint(body, key ? 1 : 0);
        put_tag(body, 2, 2);  // MapEntry.value: Len
        put_varint(body, value.size());
        body += value;
        std::string out;
        put_tag(out, 14, 2);  // Msg.flags: Len
        put_varint(out, body.size());
        out += body;
        return out;
    };
    const std::string buf = entry(true, "on") + entry(false, "off");
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(rapidproto::dump(*m) == R"({"flags": {"true": "on", "false": "off"}})");
    // Both layout paths agree: width 0 forces the map object onto multiple lines.
    const std::string wide = rapidproto::dump(*m, 0);
    CHECK(wide.find("\"true\": \"on\"") != std::string::npos);
    CHECK(wide.find("\"1\":") == std::string::npos);
}

TEST_CASE("dumpgen: bools and floats render through their writers in every container",
          "[dumpgen]") {
    // Repeated elements, map values and oneof members each reach the value writers by their own
    // emission path, so each needs its own coverage -- a bool or double is not rendered by the
    // singular-field path in any of them.
    std::string buf;
    {  // bools = [true, false] (field 17, packed by default in proto3)
        std::string packed;
        put_varint(packed, 1);
        put_varint(packed, 0);
        put_tag(buf, 17, 2);
        put_varint(buf, packed.size());
        buf += packed;
    }
    {  // toggles = {"t": true} (field 18)
        std::string entry;
        put_tag(entry, 1, 2);
        put_varint(entry, 1);
        entry += "t";
        put_tag(entry, 2, 0);
        put_varint(entry, 1);
        put_tag(buf, 18, 2);
        put_varint(buf, entry.size());
        buf += entry;
    }
    {  // ratios = {"r": 0.5} (field 19)
        std::string entry;
        put_tag(entry, 1, 2);
        put_varint(entry, 1);
        entry += "r";
        put_tag(entry, 2, 1);
        put_ieee(entry, 0.5);
        put_tag(buf, 19, 2);
        put_varint(buf, entry.size());
        buf += entry;
    }
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(rapidproto::dump(*m) ==
          R"({"bools": [true, false], "toggles": {"t": true}, "ratios": {"r": 0.5}})");
}

TEST_CASE("dumpgen: a bool/double oneof member renders through its writer", "[dumpgen]") {
    const auto dump_of = [](const std::string& buf) {
        Arena arena;
        const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        return rapidproto::dump(*m);
    };
    std::string pick_bool;
    put_tag(pick_bool, 15, 0);  // pick.c: Varint
    put_varint(pick_bool, 1);
    CHECK(dump_of(pick_bool) == R"({"c": true})");

    std::string pick_double;
    put_tag(pick_double, 16, 1);  // pick.d: I64
    put_ieee(pick_double, 2.5);
    CHECK(dump_of(pick_double) == R"({"d": 2.5})");
}

TEST_CASE("dumpgen: float/double edge values keep their exact bits", "[dumpgen]") {
    const auto dump_of = [](double db, float fl) {
        std::string buf;
        put_tag(buf, 1, 0);  // i32 (required)
        put_varint(buf, 1);
        put_tag(buf, 14, 5);  // fl: I32
        put_ieee(buf, fl);
        put_tag(buf, 15, 1);  // db: I64
        put_ieee(buf, db);
        Arena arena;
        const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        return rapidproto::dump(*m);
    };
    // A float needs up to 9 significant digits to read back exactly; this one needs all 9. Paired
    // with -0.0, whose sign survives only because the round-trip check compares bit patterns.
    CHECK(dump_of(-0.0, 3.1415927F) == R"({"i32": 1, "fl": 3.1415927, "db": -0})");
    // Subnormals round-trip too, though not in the fewest digits possible (see round_trip_text).
    CHECK(dump_of(std::numeric_limits<double>::denorm_min(),
                  std::numeric_limits<float>::denorm_min()) ==
          R"({"i32": 1, "fl": 1.4013e-45, "db": 4.94065645841247e-324})");
}

TEST_CASE("dumpgen: a double keeps its precision on the multi-line path too", "[dumpgen]") {
    // The compact and multi-line paths render through different sinks (the Writer's scratch buffer
    // vs the output stream), so a value's rendering is pinned on both rather than only the default.
    std::string packed;
    put_ieee(packed, 3.141592653589793);
    put_ieee(packed, 2.718281828459045);
    std::string buf;
    put_tag(buf, 12, 2);  // reals: Len (packed I64)
    put_varint(buf, packed.size());
    buf += packed;
    Arena arena;
    const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(rapidproto::dump(*m) == R"({"reals": [3.141592653589793, 2.718281828459045]})");
    const std::string wide = rapidproto::dump(*m, 0);
    CHECK(wide.find("3.141592653589793") != std::string::npos);
    CHECK(wide.find("2.718281828459045") != std::string::npos);
}

TEST_CASE("dumpgen: a double keeps every digit it needs to read back", "[dumpgen]") {
    // The stream default of 6 significant digits would render this as 3.14159 -- a DIFFERENT value.
    // write_float widens the precision until the text parses back to the identical double.
    std::string buf;
    put_tag(buf, 15, 1);  // Scalars.db: I64
    put_ieee(buf, 3.141592653589793);
    put_tag(buf, 1, 0);  // Scalars.i32 (required): Varint
    put_varint(buf, 1);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(rapidproto::dump(*m) == R"({"i32": 1, "db": 3.141592653589793})");
}

TEST_CASE("dumpgen: a float/double renders no more digits than it carries", "[dumpgen]") {
    // The flip side of the test above: precision is widened only when the shorter form fails to
    // read back, so 0.1 stays "0.1" rather than padding out to 0.10000000000000001.
    std::string buf;
    put_tag(buf, 1, 0);  // i32 (required)
    put_varint(buf, 1);
    put_tag(buf, 14, 5);  // fl: I32
    put_ieee(buf, 0.1F);
    put_tag(buf, 15, 1);  // db: I64
    put_ieee(buf, 0.1);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    CHECK(rapidproto::dump(*m) == R"({"i32": 1, "fl": 0.1, "db": 0.1})");
}

TEST_CASE("dumpgen: an implicit-presence -0.0 is printed, not omitted as a default", "[dumpgen]") {
    // Implicit presence omits a field equal to its zero default. For a float that test must read the
    // BIT PATTERN: -0.0 compares equal to 0.0, so a plain comparison drops it -- but protobuf sends
    // it (it serializes the field, and its JSON prints -0.0), so omitting it reports a value the
    // sender never wrote and loses the sign the dumper otherwise takes care to render.
    const auto dump_of = [](double ratio, float scale) {
        std::string buf;
        put_tag(buf, 20, 1);  // p3.Msg.ratio: I64
        put_ieee(buf, ratio);
        put_tag(buf, 21, 5);  // p3.Msg.scale: I32
        put_ieee(buf, scale);
        Arena arena;
        const p3::Msg* m = p3::Msg::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        return rapidproto::dump(*m);
    };
    CHECK(dump_of(-0.0, -0.0F) == R"({"ratio": -0, "scale": -0})");
    CHECK(dump_of(0.0, 0.0F) == "{}");  // positive zero IS the default -- still omitted
    CHECK(dump_of(1.5, -2.5F) == R"({"ratio": 1.5, "scale": -2.5})");
}

TEST_CASE("dumpgen: non-finite floats render as JSON strings, not bare nan/inf", "[dumpgen]") {
    // NaN and the infinities have no JSON number form -- streaming them raw emits `nan` / `-inf`,
    // which no JSON parser accepts. They take protobuf's JSON convention instead.
    const auto dump_of = [](double db, float fl) {
        std::string buf;
        put_tag(buf, 1, 0);  // i32 (required)
        put_varint(buf, 1);
        put_tag(buf, 14, 5);  // fl: I32
        put_ieee(buf, fl);
        put_tag(buf, 15, 1);  // db: I64
        put_ieee(buf, db);
        Arena arena;
        const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
        REQUIRE(m != nullptr);
        return rapidproto::dump(*m);
    };
    CHECK(dump_of(std::numeric_limits<double>::quiet_NaN(),
                  -std::numeric_limits<float>::infinity()) ==
          R"({"i32": 1, "fl": "-Infinity", "db": "NaN"})");
    CHECK(
        dump_of(std::numeric_limits<double>::infinity(), std::numeric_limits<float>::infinity()) ==
        R"({"i32": 1, "fl": "Infinity", "db": "Infinity"})");
}

TEST_CASE("dumpgen: the sink's locale and format flags do not reach the dump", "[dumpgen]") {
    // Both layout paths: a group that fits renders through the Writer's own scratch buffer, while
    // width 0 opens every group so the values land straight on the caller's stream.
    check_hostile_stream_ignored(dump_through_hostile_stream(120));
    check_hostile_stream_ignored(dump_through_hostile_stream(0));
}

TEST_CASE("dumpgen: dumping does not change the caller's stream formatting", "[dumpgen]") {
    // Dumping sets no stream flags, so a caller's ostream keeps whatever formatting it carried --
    // a dump is not allowed to change how that stream prints anything afterwards.
    std::string buf;
    put_tag(buf, 1, 0);  // i32 (required)
    put_varint(buf, 1);
    Arena arena;
    const p2::Scalars* m = p2::Scalars::decode(ByteView(buf), arena);
    REQUIRE(m != nullptr);
    std::ostringstream os;
    const std::ios::fmtflags before = os.flags();
    rapidproto::dump(os, *m);
    CHECK(os.flags() == before);
    os.str(std::string());
    os << true;  // the stream's own default numeric rendering, not `true`
    CHECK(os.str() == "1");
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
    const std::string dump = rapidproto::dump(*m);
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
    const std::string dump = rapidproto::dump(*h);
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
    const std::string dump = rapidproto::dump(*h);
    CHECK(dump.find("\"has_unknown_fields\": true") != std::string::npos);
    CHECK(dump == R"({"has_unknown_fields": true})");
}
