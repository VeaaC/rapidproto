// The enum / reserved-range corner of the schema grammar, split out of parser.cpp for BUILD COST
// rather than for design: see src/parser_internal.hpp. clang-tidy's matchers walk an AST whose size
// tracks combinator width, and parser.cpp alone was the gate's critical path at ~340s (its compile
// is only ~18s). Splitting the width across TUs lets the lint run in parallel.
//
// Self-contained by design: `signed_int`, the reserved-name parser and the enum body live only
// here. The three entry points parser.cpp still calls -- reserved_range, parse_reserved, parse_enum
// -- are declared in the internal header; everything else stays in the anonymous namespace.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "parser_internal.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/combinators.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {
using namespace parse_detail;  // NOLINT(google-build-using-namespace): the parser's own split
namespace parse_detail {

// ["-"] intLit  ->  int32
namespace {

auto signed_int() {
    return map(seq(opt(kind(TokenKind::Minus)), kind(TokenKind::IntLiteral)), [](auto parts) {
        return parse_int32(std::get<1>(parts).text, std::get<0>(parts).has_value());
    });
}

// Range = ["-"] intLit [ "to" ( ["-"] intLit | "max" ) ]; `max` -> the given sentinel.
// Function boundary: reserved_range appears twice inside both parse_reserved and
// extension_range, so its signed_int/kind chain otherwise re-spells into every enclosing name.
auto reserved_name() {
    return alt(map(kind(TokenKind::StringLiteral), [](Token t) { return std::move(t.str_value); }),
               map(name_token(), [](const Token& t) { return std::string(t.text); }));
}

// ReservedDecl = "reserved" ( Range {"," Range} | Name {"," Name} ) ";"
auto enum_value_decl() {
    return map(seq(name_token(), cut(kind(TokenKind::Equals)), cut(signed_int()),
                   opt(parse_compact_options), cut(kind(TokenKind::Semicolon))),
               [](auto parts) {
                   EnumValueNode value;
                   value.name = std::string(std::get<0>(parts).text);
                   value.number = std::get<2>(parts);
                   if (std::get<3>(parts).has_value()) {
                       value.options = std::move(*std::get<3>(parts));
                   }
                   return value;
               });
}

// One enum body element. `option`/`reserved` are matched before a bare value so those
// keywords aren't mistaken for value names. monostate represents an empty ";".
using EnumElement = std::variant<EnumValueNode, Option, ReservedNode, std::monostate>;

auto enum_body() {
    return many(
        alt(map(parse_option_decl, [](Option o) { return EnumElement{std::move(o)}; }),
            map([](Range<Token> i) { return parse_reserved(i, kMaxEnumNumber); },
                [](ReservedNode r) { return EnumElement{std::move(r)}; }),
            map(enum_value_decl(), [](EnumValueNode v) { return EnumElement{std::move(v)}; }),
            map(kind(TokenKind::Semicolon),
                [](const Token&) { return EnumElement{std::monostate{}}; })));
}

EnumNode assemble_enum(std::string_view name, std::vector<EnumElement>& elements,
                       SyntaxLevel syntax) {
    EnumNode node;
    node.name = std::string(name);
    // proto2 enums are closed; proto3 and editions default open (editions refined by
    // the feature pass).
    node.openness = syntax == SyntaxLevel::Proto2 ? EnumOpenness::Closed : EnumOpenness::Open;
    for (auto& element : elements) {
        if (auto* value = std::get_if<EnumValueNode>(&element)) {
            node.values.push_back(std::move(*value));
        } else if (auto* option = std::get_if<Option>(&element)) {
            node.options.push_back(std::move(*option));
        } else if (auto* reserved = std::get_if<ReservedNode>(&element)) {
            node.reserved.push_back(std::move(*reserved));
        }
    }
    return node;
}

}  // namespace

Result<Parsed<NumberRange, Token>> reserved_range(Range<Token> in, std::int32_t max_sentinel) {
    auto bound =
        alt(map(kind(TokenKind::KwMax), [max_sentinel](const Token&) { return max_sentinel; }),
            signed_int());
    return map(seq(signed_int(), opt(preceded(kind(TokenKind::KwTo), bound))), [](auto parts) {
        NumberRange range;
        range.start = std::get<0>(parts);
        range.end = std::get<1>(parts).has_value() ? *std::get<1>(parts) : range.start;
        return range;
    })(in);
}
// A reserved name is a string literal (proto2/proto3) or an identifier (editions).
Result<Parsed<ReservedNode, Token>> parse_reserved(Range<Token> in, std::int32_t max_sentinel) {
    auto ranges = map(seq(reserved_range(max_sentinel),
                          many(preceded(kind(TokenKind::Comma), reserved_range(max_sentinel)))),
                      [](auto parts) {
                          ReservedNode node;
                          node.ranges =
                              prepend(std::move(std::get<0>(parts)), std::move(std::get<1>(parts)));
                          return node;
                      });
    auto names = map(seq(reserved_name(), many(preceded(kind(TokenKind::Comma), reserved_name()))),
                     [](auto parts) {
                         ReservedNode node;
                         node.names =
                             prepend(std::move(std::get<0>(parts)), std::move(std::get<1>(parts)));
                         return node;
                     });
    return delimited(kind(TokenKind::KwReserved), alt(ranges, names),
                     cut(kind(TokenKind::Semicolon)))(in);
}

// EnumValueDecl = ident "=" ["-"] intLit [ CompactOptions ] ";"
}  // namespace parse_detail

// EnumDecl = [ "export" | "local" ] "enum" ident "{" { EnumElement } "}"
Result<Parsed<EnumNode, Token>> parse_enum(Range<Token> in, const ParseContext& ctx) {
    const SyntaxLevel syntax = ctx.syntax_level;
    return map(seq(visibility_modifier(), kind(TokenKind::KwEnum), cut(name_token()),
                   cut(kind(TokenKind::LBrace)), enum_body(), cut(kind(TokenKind::RBrace))),
               [syntax](auto parts) {
                   EnumNode node =
                       assemble_enum(std::get<2>(parts).text, std::get<4>(parts), syntax);
                   if (std::get<0>(parts).has_value()) {
                       node.visibility = *std::get<0>(parts);
                   }
                   return node;
               })(in);
}

}  // namespace rapidproto
