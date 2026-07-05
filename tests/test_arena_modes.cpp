// Tests for the field-modes core (rapidproto/arenagen/modes.hpp): the profile-file parser and
// the name resolution/validation that turns entries into a per-field selection. Layout and
// codegen consumption of the resolved modes are covered by the arena layout/generator suites.

#include <catch_amalgamated.hpp>

#include <string>
#include <utility>
#include <vector>

#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/source.hpp"
#include "temp_dir.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

// A schema exercising every resolution rule: nested types, oneofs, maps, required, and a type
// used by several fields.
constexpr const char* kSchema = R"(
    syntax = "proto2";
    package m;

    message Blob { optional bytes data = 1; }

    message Msg {
      optional int32 keep = 1;
      optional int32 debug = 2;
      repeated Blob blobs = 3;
      optional Blob one_blob = 4;
      required int32 must = 5;
      map<string, Blob> by_name = 6;
      oneof pick { int32 a = 7; Blob pick_blob = 11; }
      message Nested { optional Blob deep_blob = 1; optional int32 inner = 2; }
      optional Nested nested = 8;
      required Blob req_blob = 9;
      optional Hue hue = 10;
    }

    enum Hue { HUE_RED = 0; }
)";

struct Analyzed {
    ResolvedFileSet set;
    SymbolTable symbols;
};

Analyzed analyze_schema() {
    auto lr = lex(kSchema);
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto parsed = parse_file(Range<Token>(lexed.tokens));
    REQUIRE(parsed.is_ok());
    Analyzed out;
    FileNode file = std::move(parsed.value().value);
    file.filename = "modes.proto";
    out.set.file_index["modes.proto"] = 0;
    out.set.files.push_back(std::move(file));
    auto table = analyze(out.set);
    REQUIRE(table.is_ok());
    out.symbols = std::move(table).value();
    return out;
}

arenagen::FieldModesSpec spec_of(std::vector<std::pair<arenagen::FieldMode, std::string>> entries) {
    arenagen::FieldModesSpec spec;
    for (auto& [mode, name] : entries) {
        spec.entries.push_back({mode, std::move(name), "test"});
    }
    return spec;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): message FQN vs field name, distinct roles
const FieldNode* field_named(const Analyzed& a, const std::string& msg_fqn,
                             const std::string& name) {
    const MessageNode* msg = a.symbols.messages.at(msg_fqn);
    for (const FieldNode& field : msg->fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("modes: file parser accepts directives, comments, and rejects malformed lines") {
    arenagen::FieldModesSpec spec;
    const auto ok = arenagen::parse_modes_file(R"(
        # a decode profile
        name lean          # trailing comment
        drop m.Msg.debug
        raw  m.Blob
    )",
                                               "p.txt", spec);
    REQUIRE(ok.is_ok());
    CHECK(spec.profile_name == "lean");
    REQUIRE(spec.entries.size() == 2);
    CHECK(spec.entries[0].mode == arenagen::FieldMode::Drop);
    CHECK(spec.entries[0].name == "m.Msg.debug");
    CHECK(spec.entries[0].origin == "p.txt:4");
    CHECK(spec.entries[1].mode == arenagen::FieldMode::Raw);

    arenagen::FieldModesSpec bad;
    CHECK(arenagen::parse_modes_file("shred m.Msg.debug\n", "p.txt", bad).is_err());
    CHECK(arenagen::parse_modes_file("drop\n", "p.txt", bad).is_err());
    CHECK(arenagen::parse_modes_file("drop a b\n", "p.txt", bad).is_err());
    arenagen::FieldModesSpec renamed;
    renamed.profile_name = "x";
    CHECK(arenagen::parse_modes_file("name y\n", "p.txt", renamed).is_err());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("modes: field and type entries resolve, with field-level precedence") {
    const Analyzed a = analyze_schema();
    const auto modes =
        arenagen::resolve_field_modes(spec_of({{arenagen::FieldMode::Drop, "m.Msg.debug"},
                                               {arenagen::FieldMode::Raw, "m.Blob"},
                                               // explicit field entry overrides the type-level raw:
                                               {arenagen::FieldMode::Drop, "m.Msg.one_blob"}}),
                                      a.set, a.symbols);
    REQUIRE(modes.is_ok());
    const arenagen::FieldModes& m = modes.value();
    CHECK(m.active());
    CHECK(m.fields.at(field_named(a, ".m.Msg", "debug")) == arenagen::FieldMode::Drop);
    CHECK(m.fields.at(field_named(a, ".m.Msg", "blobs")) == arenagen::FieldMode::Raw);  // via type
    CHECK(m.fields.at(field_named(a, ".m.Msg", "one_blob")) ==
          arenagen::FieldMode::Drop);  // field beats type
    CHECK(m.fields.at(field_named(a, ".m.Msg.Nested", "deep_blob")) ==
          arenagen::FieldMode::Raw);  // type entries reach any depth
    CHECK(m.fields.count(field_named(a, ".m.Msg", "keep")) == 0);
    CHECK(m.maps.empty());  // a type-level RAW silently leaves map<string, Blob> materialized
    CHECK(m.profile_id.size() == 16);  // content hash (no `name` given)
    // Type-level raw DOES apply to a required field (only drop is excluded there); the oneof
    // member is excluded for both modes.
    CHECK(m.fields.at(field_named(a, ".m.Msg", "req_blob")) == arenagen::FieldMode::Raw);
    bool oneof_member_selected = false;
    for (const auto& [node, mode] : m.fields) {
        oneof_member_selected |= node->name == "pick_blob";
    }
    CHECK_FALSE(oneof_member_selected);
}

TEST_CASE("modes: explicit map drops, enum type drops, and raw-required resolve") {
    const Analyzed a = analyze_schema();
    const auto modes = arenagen::resolve_field_modes(
        spec_of({{arenagen::FieldMode::Drop, "m.Msg.by_name"},    // explicit MAP field entry
                 {arenagen::FieldMode::Drop, "m.Hue"},            // enum TYPE entry
                 {arenagen::FieldMode::Raw, "m.Msg.req_blob"}}),  // raw on required is legal
        a.set, a.symbols);
    REQUIRE(modes.is_ok());
    const arenagen::FieldModes& m = modes.value();
    REQUIRE(m.maps.size() == 1);
    CHECK(m.maps.begin()->second == arenagen::FieldMode::Drop);
    CHECK(m.fields.at(field_named(a, ".m.Msg", "hue")) == arenagen::FieldMode::Drop);
    CHECK(m.fields.at(field_named(a, ".m.Msg", "req_blob")) == arenagen::FieldMode::Raw);
    // Type-level DROP silently skips the required field (no explicit entry to error on) while
    // still dropping the type's other, non-required uses.
    const auto drop_type = arenagen::resolve_field_modes(
        spec_of({{arenagen::FieldMode::Drop, "m.Blob"}}), a.set, a.symbols);
    REQUIRE(drop_type.is_ok());
    CHECK(drop_type.value().fields.count(field_named(a, ".m.Msg", "req_blob")) == 0);
    CHECK(drop_type.value().fields.at(field_named(a, ".m.Msg", "one_blob")) ==
          arenagen::FieldMode::Drop);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("modes: validation rejects unknown names, oneof members, required drops, conflicts") {
    const Analyzed a = analyze_schema();
    const auto err_of = [&](std::vector<std::pair<arenagen::FieldMode, std::string>> entries) {
        auto r = arenagen::resolve_field_modes(spec_of(std::move(entries)), a.set, a.symbols);
        REQUIRE(r.is_err());
        return r.error().message;
    };
    CHECK(err_of({{arenagen::FieldMode::Drop, "m.Msg.nope"}}).find("unknown") != std::string::npos);
    CHECK(err_of({{arenagen::FieldMode::Raw, "m.Msg.a"}}).find("oneof") != std::string::npos);
    CHECK(err_of({{arenagen::FieldMode::Drop, "m.Msg.must"}}).find("required") !=
          std::string::npos);
    CHECK(err_of({{arenagen::FieldMode::Drop, "m.Msg.one_blob"},
                  {arenagen::FieldMode::Raw, "m.Msg.one_blob"}})
              .find("already has mode") != std::string::npos);
    CHECK(err_of({{arenagen::FieldMode::Drop, "m.Blob"}, {arenagen::FieldMode::Raw, "m.Blob"}})
              .find("already has mode") != std::string::npos);
    // Raw applies to message-typed fields only: a payload is what the later decode() consumes.
    CHECK(err_of({{arenagen::FieldMode::Raw, "m.Msg.debug"}}).find("message-typed") !=
          std::string::npos);
    CHECK(err_of({{arenagen::FieldMode::Raw, "m.Msg.by_name"}}).find("message-typed") !=
          std::string::npos);
    CHECK(err_of({{arenagen::FieldMode::Raw, "m.Hue"}}).find("enum") != std::string::npos);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a linear list of assertions
TEST_CASE("modes: profile identity is stable, order-independent, and name-overridable") {
    const Analyzed a = analyze_schema();
    const auto id_of = [&](std::vector<std::pair<arenagen::FieldMode, std::string>> entries,
                           std::string name = {}) {
        auto spec = spec_of(std::move(entries));
        spec.profile_name = std::move(name);
        auto r = arenagen::resolve_field_modes(spec, a.set, a.symbols);
        REQUIRE(r.is_ok());
        return r.value().profile_id;
    };
    const std::string ab =
        id_of({{arenagen::FieldMode::Drop, "m.Msg.debug"}, {arenagen::FieldMode::Raw, "m.Blob"}});
    const std::string ba =
        id_of({{arenagen::FieldMode::Raw, "m.Blob"}, {arenagen::FieldMode::Drop, "m.Msg.debug"}});
    CHECK(ab == ba);  // normalization sorts, so entry order can't fork the identity
    // Leading-dot and bare spellings normalize together.
    CHECK(id_of({{arenagen::FieldMode::Drop, ".m.Msg.debug"},
                 {arenagen::FieldMode::Raw, "m.Blob"}}) == ab);
    CHECK(id_of({{arenagen::FieldMode::Drop, "m.Msg.debug"}}) != ab);
    // A `name` keeps a human-readable prefix but still carries the content hash: two DIFFERENT
    // selections sharing a name must not produce one identity (they would link silently).
    const std::string named = id_of({{arenagen::FieldMode::Drop, "m.Msg.debug"}}, "lean");
    CHECK(named.rfind("lean_", 0) == 0);
    CHECK(named.size() == 5 + 8);
    CHECK(id_of({{arenagen::FieldMode::Drop, "m.Msg.keep"}}, "lean") != named);
    // Duplicate identical entries dedup into the same identity.
    CHECK(id_of({{arenagen::FieldMode::Drop, "m.Msg.debug"},
                 {arenagen::FieldMode::Drop, "m.Msg.debug"}}) ==
          id_of({{arenagen::FieldMode::Drop, "m.Msg.debug"}}));
    // No entries at all: inactive, no identity, default output.
    auto empty = arenagen::resolve_field_modes({}, a.set, a.symbols);
    REQUIRE(empty.is_ok());
    CHECK_FALSE(empty.value().active());
    CHECK(empty.value().profile_id.empty());
}

TEST_CASE("modes: parser handles CRLF and a missing trailing newline; identifiers enforced") {
    arenagen::FieldModesSpec spec;
    REQUIRE(arenagen::parse_modes_file("drop m.Msg.debug\r\nraw m.Blob", "p.txt", spec).is_ok());
    REQUIRE(spec.entries.size() == 2);
    CHECK(spec.entries[0].name == "m.Msg.debug");  // \r trimmed
    CHECK(spec.entries[1].name == "m.Blob");       // last line without newline still parses
    arenagen::FieldModesSpec bad;
    CHECK(arenagen::parse_modes_file("name le-an\n", "p.txt", bad).is_err());
    CHECK(arenagen::parse_modes_file("name 9x\n", "p.txt", bad).is_err());
}

TEST_CASE("modes: one profile resolves across a batch of unrelated entry files") {
    // The global-profile workflow: entries that do NOT import each other, one profile naming
    // fields from both. Resolution runs against the batch's union symbols, so every name lands.
    const test::TempDir dir("modes_batch");
    dir.write("one.proto", R"(syntax = "proto3"; package one;
                              message Payload { bytes data = 1; }
                              message Holder { Payload p = 1; int32 skip_me = 2; })");
    dir.write("two.proto", R"(syntax = "proto3"; package two;
                              message Other { string note = 1; })");
    ResolverConfig config;
    config.include_paths = {dir.root()};
    SourceRegistry sources;
    auto resolved = resolve({dir.path("one.proto"), dir.path("two.proto")}, config, sources);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();
    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable symbols = std::move(analyzed).value();

    const auto modes =
        arenagen::resolve_field_modes(spec_of({{arenagen::FieldMode::Raw, "one.Payload"},
                                               {arenagen::FieldMode::Drop, "one.Holder.skip_me"},
                                               {arenagen::FieldMode::Drop, "two.Other.note"}}),
                                      set, symbols);
    REQUIRE(modes.is_ok());
    CHECK(modes.value().fields.size() == 3);  // p (via type), skip_me, note -- across both files
}
