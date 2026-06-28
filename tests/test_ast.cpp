#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "rapidproto/ast.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

// Nodes must move cheaply so std::vector relocates by move, not copy.
static_assert(std::is_nothrow_move_constructible_v<MessageNode>);
static_assert(std::is_nothrow_move_constructible_v<OptionValue>);
static_assert(std::is_nothrow_move_constructible_v<FileNode>);

TEST_CASE("FileNode defaults and construction") {
    FileNode f;
    CHECK(f.syntax_level == SyntaxLevel::Proto2);  // absent syntax/edition => proto2
    CHECK(f.package.empty());

    f.package = "foo.bar";
    MessageNode m;
    m.name = "Msg";
    f.messages.push_back(std::move(m));
    REQUIRE(f.messages.size() == 1);
    CHECK(f.messages[0].name == "Msg");
}

TEST_CASE("MessageNode nests recursively") {
    MessageNode outer;
    outer.name = "Outer";
    MessageNode inner;
    inner.name = "Inner";
    FieldNode fld;
    fld.name = "x";
    fld.number = 1;
    inner.fields.push_back(std::move(fld));
    outer.nested_messages.push_back(std::move(inner));

    REQUIRE(outer.nested_messages.size() == 1);
    CHECK(outer.nested_messages[0].name == "Inner");
    CHECK(outer.nested_messages[0].fields[0].number == 1);
}

TEST_CASE("FieldNode normalized attributes default sensibly") {
    const FieldNode fld;
    CHECK(fld.presence == FieldPresence::Explicit);
    CHECK_FALSE(fld.is_repeated);
    CHECK(fld.repeated_encoding == RepeatedEncoding::Expanded);
    CHECK_FALSE(fld.is_group);
    CHECK(fld.message_encoding == MessageEncoding::LengthPrefixed);
    CHECK_FALSE(fld.default_value.has_value());
    CHECK(fld.resolved_type_fqn.empty());
    CHECK_FALSE(fld.is_message_type);
}

TEST_CASE("EnumNode openness and (possibly negative) values") {
    EnumNode e;
    e.name = "Color";
    e.openness = EnumOpenness::Closed;
    e.values.push_back(EnumValueNode{"RED", 0, {}, ""});
    e.values.push_back(EnumValueNode{"NEG", -1, {}, ""});

    REQUIRE(e.values.size() == 2);
    CHECK(e.values[1].number == -1);
    CHECK(e.openness == EnumOpenness::Closed);
}

TEST_CASE("OptionValue holds each scalar alternative; identifier != string") {
    const OptionValue b{true};
    CHECK(std::get<bool>(b.value));

    const OptionValue i{std::int64_t{-5}};
    CHECK(std::get<std::int64_t>(i.value) == -5);

    const OptionValue s{std::string("hi")};
    CHECK(std::get<std::string>(s.value) == "hi");

    const OptionValue id{Identifier{"SPEED"}};
    CHECK(std::get<Identifier>(id.value).name == "SPEED");

    CHECK(id.value.index() != s.value.index());  // Identifier is a distinct alternative
}

TEST_CASE("OptionValue is recursive: a message literal containing a list literal") {
    // { values: [1, 2], name: "x" }
    ListLiteral list;
    list.elements.push_back(OptionValue{std::int64_t{1}});
    list.elements.push_back(OptionValue{std::int64_t{2}});

    MessageLiteral lit;
    lit.fields.push_back(MessageLiteralField{MessageFieldNameKind::Simple, "values", "",
                                             OptionValue{std::move(list)}});
    lit.fields.push_back(MessageLiteralField{MessageFieldNameKind::Simple, "name", "",
                                             OptionValue{std::string("x")}});

    const OptionValue v{std::move(lit)};
    const auto& ml = std::get<MessageLiteral>(v.value);
    REQUIRE(ml.fields.size() == 2);
    CHECK(ml.fields[0].name == "values");

    const auto& inner = std::get<ListLiteral>(ml.fields[0].value.value);
    REQUIRE(inner.elements.size() == 2);
    CHECK(std::get<std::int64_t>(inner.elements[1].value) == 2);
    CHECK(std::get<std::string>(ml.fields[1].value.value) == "x");
}

TEST_CASE("recursive OptionValue copies deeply and independently") {
    ListLiteral list;
    list.elements.push_back(OptionValue{std::int64_t{1}});
    MessageLiteral lit;
    lit.fields.push_back(
        MessageLiteralField{MessageFieldNameKind::Simple, "v", "", OptionValue{std::move(list)}});
    const OptionValue original{std::move(lit)};

    OptionValue copy = original;  // deep copy of the whole recursive tree
    auto& copy_leaf =
        std::get<ListLiteral>(std::get<MessageLiteral>(copy.value).fields[0].value.value)
            .elements[0]
            .value;
    copy_leaf = std::int64_t{99};  // mutate the copy's leaf

    const auto& orig_leaf =
        std::get<ListLiteral>(std::get<MessageLiteral>(original.value).fields[0].value.value)
            .elements[0]
            .value;
    CHECK(std::get<std::int64_t>(copy_leaf) == 99);
    CHECK(std::get<std::int64_t>(orig_leaf) == 1);  // original untouched -> deep, independent copy
}

TEST_CASE("AST nodes are copyable (deep value semantics)") {
    MessageNode m;
    m.name = "M";
    m.options.push_back(Option{{{"deprecated", false}}, OptionValue{true}});

    MessageNode copy = m;  // deep copy
    copy.name = "M2";

    CHECK(m.name == "M");
    CHECK(copy.name == "M2");
    REQUIRE(copy.options.size() == 1);
    CHECK(copy.options[0].name[0].name == "deprecated");
}

TEST_CASE("Option name components carry the extension flag") {
    Option opt;
    opt.name.push_back(OptionNameComponent{"my.pkg.ext", true});
    opt.name.push_back(OptionNameComponent{"field", false});
    opt.value = OptionValue{std::uint64_t{42}};

    CHECK(opt.name[0].is_extension);
    CHECK_FALSE(opt.name[1].is_extension);
    CHECK(std::get<std::uint64_t>(opt.value.value) == 42);
}

TEST_CASE("Reserved and extension ranges use the right max sentinel") {
    ReservedNode r;
    r.ranges.push_back(NumberRange{1, 1});
    r.ranges.push_back(NumberRange{10, kMaxMessageFieldNumber});
    r.names.emplace_back("foo");
    CHECK(r.ranges[1].end == 536870911);
    CHECK(r.names[0] == "foo");

    ExtensionRangeNode ext;
    ext.ranges.push_back(NumberRange{100, kMaxMessageFieldNumber});
    CHECK(ext.ranges[0].start == 100);
}

TEST_CASE("EnumNode reserved ranges allow negatives and the enum max sentinel") {
    EnumNode e;
    e.name = "E";
    ReservedNode r;
    r.ranges.push_back(NumberRange{-5, -1});  // enum values can be negative
    r.ranges.push_back(NumberRange{100, kMaxEnumNumber});
    r.names.emplace_back("OBSOLETE");
    e.reserved.push_back(std::move(r));

    REQUIRE(e.reserved.size() == 1);
    CHECK(e.reserved[0].ranges[0].start == -5);
    CHECK(e.reserved[0].ranges[1].end == kMaxEnumNumber);
    CHECK(e.reserved[0].ranges[1].end == 2147483647);
    CHECK(e.reserved[0].names[0] == "OBSOLETE");
}

TEST_CASE("ExtendNode at file scope") {
    FileNode f;
    f.syntax_level = SyntaxLevel::Proto2;
    ExtendNode ext;
    ext.extendee_type_name = ".google.protobuf.FileOptions";
    FieldNode fld;
    fld.name = "my_opt";
    fld.number = 1000;
    ext.fields.push_back(std::move(fld));
    f.extends.push_back(std::move(ext));

    REQUIRE(f.extends.size() == 1);
    CHECK(f.extends[0].extendee_type_name == ".google.protobuf.FileOptions");
    CHECK(f.extends[0].fields[0].number == 1000);
}

TEST_CASE("MapFieldNode") {
    MapFieldNode m;
    m.name = "labels";
    m.key_type = "string";
    m.value_type = "string";
    m.number = 1;
    CHECK(m.key_type == "string");
    CHECK_FALSE(m.value_is_message);
    CHECK_FALSE(m.value_is_enum);
    CHECK(m.resolved_value_type_fqn.empty());
}
