// End-to-end pipeline test: a self-contained multi-file
// schema spanning proto2 + proto3 + editions(2023), a dropped `service`, file-scope
// `extend`, a group, extension ranges, reserved ranges/names, a map, a oneof, and a
// full-fidelity custom option. It is written to a temp dir and driven through the real
// entry points: resolve() (disk -> lex -> parse -> topo) then analyze() (features ->
// FQNs -> type resolution -> option interpretation). Assertions spot-check the
// decode-relevant resolved set the AST must nail.

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"
#include "temp_dir.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

using rapidproto::test::TempDir;

// Find a top-level message by name in a file (returns nullptr if absent).
const MessageNode* find_message(const FileNode& file, const std::string& name) {
    for (const auto& msg : file.messages) {
        if (msg.name == name) {
            return &msg;
        }
    }
    return nullptr;
}

const FieldNode* find_field(const MessageNode& msg, const std::string& name) {
    for (const auto& field : msg.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

// Read a field's interpreted default value. The has_value guard keeps clang-tidy's
// optional-access model happy.
template <typename T>
T default_as(const FieldNode& field) {
    REQUIRE(field.default_value.has_value());
    if (!field.default_value.has_value()) {
        return T{};
    }
    return std::get<T>(field.default_value->value);
}

// proto2 (base): a group, file-scope extend with a default, extension ranges, reserved,
// and a service to confirm it is parsed-past and dropped (gRPC out of scope).
constexpr const char* kBaseProto = R"(
    syntax = "proto2";
    package base;

    message Base {
      optional group Inner = 1 {
        optional int32 v = 2;
      }
      extensions 100 to 200;
      reserved 5, 6 to 8;
      reserved "old_field";
    }

    extend Base {
      optional int32 ext = 100 [default = 7];
    }

    service Ignored {
      rpc Go(Base) returns (Base);
    }
)";

// proto3 (mid): cross-file message reference, explicit optional, an un-packed repeated,
// a map to a local enum, and a oneof.
constexpr const char* kMidProto = R"(
    syntax = "proto3";
    package mid;
    import "base.proto";

    enum Color {
      COLOR_UNKNOWN = 0;
      COLOR_RED = 1;
    }

    message Mid {
      base.Base b = 1;
      optional int32 maybe = 2;
      int32 plain = 3;
      repeated int32 nums = 4 [packed = false];
      map<string, Color> by_color = 5;
      oneof pick {
        int32 a = 6;
        string s = 7;
      }
    }
)";

// editions 2023 (top): file-level IMPLICIT presence feature, a message-typed field (must
// stay Explicit despite the feature), and a full-fidelity custom option (angle-bracket
// literal, list, retained raw).
constexpr const char* kTopProto = R"(
    edition = "2023";
    package top;
    import "mid.proto";

    option features.field_presence = IMPLICIT;

    message Top {
      option (custom.opt) = { a: 1 b: < c: 2 > nums: [1, 2, 3] };
      mid.Mid m = 1;
      int32 scalar = 2;
    }
)";

}  // namespace

TEST_CASE("pipeline: a multi-file proto2/proto3/editions schema resolves and analyzes") {
    const TempDir dir("e2e");
    dir.write("base.proto", kBaseProto);
    dir.write("mid.proto", kMidProto);
    dir.write("top.proto", kTopProto);

    ResolverConfig config;
    config.include_paths = {dir.root()};
    auto resolved = resolve(dir.path("top.proto"), config);
    REQUIRE(resolved.is_ok());
    ResolvedFileSet set = std::move(resolved).value();

    // Topological order: dependencies precede dependents.
    REQUIRE(set.files.size() == 3);
    CHECK(set.file_index.at("base.proto") < set.file_index.at("mid.proto"));
    CHECK(set.file_index.at("mid.proto") < set.file_index.at("top.proto"));

    auto analyzed = analyze(set);
    REQUIRE(analyzed.is_ok());
    const SymbolTable& symbols = analyzed.value();

    const FileNode& base = set.files[set.file_index.at("base.proto")];
    const FileNode& mid = set.files[set.file_index.at("mid.proto")];
    const FileNode& top = set.files[set.file_index.at("top.proto")];

    SECTION("the service is dropped, leaving the message intact") {
        // FileNode has no services field at all; the message still resolved.
        const MessageNode* base_msg = find_message(base, "Base");
        REQUIRE(base_msg != nullptr);
        CHECK(base_msg->fqn == ".base.Base");
    }

    SECTION("the group becomes a nested message plus a lowercased delimited field") {
        const MessageNode* base_msg = find_message(base, "Base");
        REQUIRE(base_msg != nullptr);
        REQUIRE(base_msg->nested_messages.size() == 1);
        CHECK(base_msg->nested_messages[0].name == "Inner");
        CHECK(base_msg->nested_messages[0].fqn == ".base.Base.Inner");

        const FieldNode* group = find_field(*base_msg, "inner");  // lowercased
        REQUIRE(group != nullptr);
        CHECK(group->is_group);
        CHECK(group->message_encoding == MessageEncoding::Delimited);
        CHECK(group->is_message_type);
        CHECK(group->resolved_type_fqn == ".base.Base.Inner");
    }

    SECTION("reserved ranges and names are retained") {
        const MessageNode* base_msg = find_message(base, "Base");
        REQUIRE(base_msg != nullptr);
        REQUIRE(base_msg->reserved.size() == 2);
        // 5, and 6 to 8.
        CHECK(base_msg->reserved[0].ranges.size() == 2);
        CHECK(base_msg->reserved[1].names.size() == 1);
        CHECK(base_msg->reserved[1].names[0] == "old_field");
        REQUIRE(base_msg->extension_ranges.size() == 1);
        REQUIRE(base_msg->extension_ranges[0].ranges.size() == 1);
        CHECK(base_msg->extension_ranges[0].ranges[0].start == 100);
        CHECK(base_msg->extension_ranges[0].ranges[0].end == 200);
    }

    SECTION("the extension field is registered with its proto2 default") {
        const auto it = symbols.extensions.find({".base.Base", 100});
        REQUIRE(it != symbols.extensions.end());
        const FieldNode* ext = it->second;
        REQUIRE(ext != nullptr);
        CHECK(ext->name == "ext");
        CHECK(ext->fqn == ".base.ext");               // declaration scope, not the extendee
        CHECK(default_as<std::uint64_t>(*ext) == 7);  // non-negative int literal -> uint64
    }

    SECTION("cross-file message references resolve and keep explicit presence") {
        const MessageNode* mid_msg = find_message(mid, "Mid");
        REQUIRE(mid_msg != nullptr);
        const FieldNode* b = find_field(*mid_msg, "b");
        REQUIRE(b != nullptr);
        CHECK(b->resolved_type_fqn == ".base.Base");
        CHECK(b->is_message_type);
        CHECK(b->presence == FieldPresence::Explicit);  // message-typed singular
    }

    SECTION("proto3 presence: explicit optional vs implicit bare scalar") {
        const MessageNode* mid_msg = find_message(mid, "Mid");
        REQUIRE(mid_msg != nullptr);
        CHECK(find_field(*mid_msg, "maybe")->presence == FieldPresence::Explicit);
        CHECK(find_field(*mid_msg, "plain")->presence == FieldPresence::Implicit);
    }

    SECTION("an un-packed repeated scalar is expanded") {
        const MessageNode* mid_msg = find_message(mid, "Mid");
        REQUIRE(mid_msg != nullptr);
        const FieldNode* nums = find_field(*mid_msg, "nums");
        REQUIRE(nums != nullptr);
        CHECK(nums->is_repeated);
        CHECK(nums->repeated_encoding == RepeatedEncoding::Expanded);
    }

    SECTION("a map to a local enum resolves its value type") {
        const MessageNode* mid_msg = find_message(mid, "Mid");
        REQUIRE(mid_msg != nullptr);
        REQUIRE(mid_msg->map_fields.size() == 1);
        const MapFieldNode& by_color = mid_msg->map_fields[0];
        CHECK(by_color.key_type == "string");
        CHECK(by_color.resolved_value_type_fqn == ".mid.Color");
        CHECK(by_color.value_is_enum);
    }

    SECTION("editions IMPLICIT applies to scalars but message fields stay explicit") {
        const MessageNode* top_msg = find_message(top, "Top");
        REQUIRE(top_msg != nullptr);
        CHECK(find_field(*top_msg, "scalar")->presence == FieldPresence::Implicit);

        const FieldNode* m = find_field(*top_msg, "m");
        REQUIRE(m != nullptr);
        CHECK(m->resolved_type_fqn == ".mid.Mid");
        CHECK(m->presence == FieldPresence::Explicit);  // message-typed fixup wins over IMPLICIT
    }

    SECTION("a full-fidelity custom option is retained raw") {
        const MessageNode* top_msg = find_message(top, "Top");
        REQUIRE(top_msg != nullptr);
        REQUIRE(top_msg->options.size() == 1);
        const Option& opt = top_msg->options[0];
        REQUIRE(opt.name.size() == 1);
        CHECK(opt.name[0].is_extension);
        CHECK(opt.name[0].name == "custom.opt");
        CHECK(std::holds_alternative<MessageLiteral>(opt.value.value));
    }

    SECTION("the symbol table holds every named type with the right kind") {
        CHECK(symbols.symbols.at(".base.Base") == SymbolKind::Message);
        CHECK(symbols.symbols.at(".base.Base.Inner") == SymbolKind::Message);
        CHECK(symbols.symbols.at(".mid.Mid") == SymbolKind::Message);
        CHECK(symbols.symbols.at(".mid.Color") == SymbolKind::Enum);
        CHECK(symbols.symbols.at(".top.Top") == SymbolKind::Message);
    }

    SECTION("proto3 enums are open") {
        REQUIRE(mid.enums.size() == 1);
        CHECK(mid.enums[0].openness == EnumOpenness::Open);
    }
}
