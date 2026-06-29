#include "rapidproto/parser.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/combinators.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"
#include "rapidproto/scalar.hpp"

// The parser is combinator-centric: each grammar production is an expression built
// from the Range/Result combinators (alt/seq/opt/many/delimited/preceded/
// separated_list), with the semantic action carried by map(). Concrete functions
// appear only at the recursion points (value <-> message-literal/list <-> field),
// because a combinator value cannot reference itself; they build and run a
// combinator that names the others.
//
// Number parsing never fails on a value-range issue: an integer literal that exceeds 64-bit range
// can only be a double-typed option written in integer form, so it becomes a double; float
// over/underflow saturates to +/-inf or 0. Genuinely malformed literals are rejected by the lexer and
// never reach here. Out-of-range values matter only for correctness on invalid input; the
// conversion is always memory-safe.

namespace rapidproto {
namespace {

// --- recursion-depth guard --------------------------------------------------
// Schema parsing is recursive descent over nested messages/groups/extends and nested option-literal
// aggregates, both unbounded in the input. A pathologically nested hand-written schema would
// overflow the stack, so cap the recursion depth and fail cleanly instead. The safety floor is
// unconditional: the parser must never crash, even on input protoc would reject. The cap is well
// above any real schema (real nesting is single digits) yet kept low enough to stay within a small
// thread stack -- the combinator frames are heavy, so it is deliberately conservative.
constexpr int kMaxParseDepth = 50;

// Recursion depth for the active parse, as a function-local thread_local: kept out of namespace scope,
// and per-thread so concurrent parses on different threads don't interfere. Balanced by DepthGuard.
int& parse_depth() {
    thread_local int depth = 0;
    return depth;
}

// RAII depth counter: increments on construction, decrements on every return (success or failure), so
// the count tracks live nesting even across the combinators' backtracking.
class DepthGuard {
public:
    DepthGuard() : m_within(++parse_depth() <= kMaxParseDepth) {}
    ~DepthGuard() { --parse_depth(); }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
    DepthGuard(DepthGuard&&) = delete;
    DepthGuard& operator=(DepthGuard&&) = delete;
    [[nodiscard]] bool within_limit() const { return m_within; }

private:
    bool m_within;
};

// A committed ("fatal") too-deeply-nested parse error at the current position; fatal so the
// backtracking combinators propagate it instead of trying sibling alternatives.
Error too_deep(Range<Token> in) {
    Error error{in.empty() ? 0 : in.front().byte_offset, "maximum nesting depth exceeded"};
    error.fatal = true;
    return error;
}

// --- token matchers ---------------------------------------------------------

bool is_keyword(TokenKind k) {
    return k >= TokenKind::KwSyntax && k <= TokenKind::KwNan;
}

// A name position accepts an identifier or a keyword (proto allows keywords as
// names — e.g. a field named `message`).
bool is_name_token(const Token& t) {
    return t.kind == TokenKind::Identifier || is_keyword(t.kind);
}

// Match one token of the given kind; produces the Token.
auto kind(TokenKind k) {
    return one([k](const Token& t) { return t.kind == k; });
}

auto name_token() {
    return one([](const Token& t) { return is_name_token(t); });
}

auto sign_token() {
    return one(
        [](const Token& t) { return t.kind == TokenKind::Minus || t.kind == TokenKind::Plus; });
}

auto number_token() {
    return one([](const Token& t) {
        return t.kind == TokenKind::IntLiteral || t.kind == TokenKind::FloatLiteral ||
               t.kind == TokenKind::KwInf || t.kind == TokenKind::KwNan;
    });
}

// --- numeric value interpretation (infallible; see file header) ------------

constexpr int kDecimalBase = 10;
constexpr int kHexBase = 16;
constexpr int kOctalBase = 8;
constexpr unsigned kInt64SignBitShift = 63;

OptionValue make_float(std::string_view text, bool negative);

// Coarse base-10 exponent of |text| (a valid float literal; sign already stripped),
// used only to classify an out-of-range literal as overflow (>= 0) vs underflow.
long approx_decimal_exponent(std::string_view text) {
    long exp = 0;
    std::string_view mantissa = text;
    if (const auto e = text.find_first_of("eE"); e != std::string_view::npos) {
        std::size_t epos = e + 1;
        if (epos < text.size() && text[epos] == '+') {
            ++epos;  // std::from_chars rejects a leading '+'
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars range
        std::from_chars(text.data() + epos, text.data() + text.size(), exp);
        mantissa = text.substr(0, e);
    }
    const auto dot = mantissa.find('.');
    const std::string_view int_part =
        dot == std::string_view::npos ? mantissa : mantissa.substr(0, dot);
    const std::string_view frac_part =
        dot == std::string_view::npos ? std::string_view{} : mantissa.substr(dot + 1);

    std::size_t i = 0;
    while (i < int_part.size() && int_part[i] == '0') {
        ++i;
    }
    if (i < int_part.size()) {  // leading significant digit is in the integer part
        return static_cast<long>(int_part.size() - i) - 1 + exp;
    }
    std::size_t z = 0;
    while (z < frac_part.size() && frac_part[z] == '0') {
        ++z;
    }
    if (z < frac_part.size()) {  // leading significant digit is after the point
        return -static_cast<long>(z + 1) + exp;
    }
    return exp;  // all-zero mantissa (value is exactly 0)
}

// Split an integer literal into (base, digits without the 0x/0 prefix).
std::pair<int, std::string_view> split_int_literal(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return {kHexBase, text.substr(2)};
    }
    if (text.size() > 1 && text[0] == '0') {
        return {kOctalBase, text.substr(1)};
    }
    return {kDecimalBase, text};
}

OptionValue make_int(std::string_view text, bool negative) {
    const auto [base, digits] = split_int_literal(text);
    std::uint64_t mag = 0;
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): paired with size below
    const char* first = digits.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars needs a range
    const char* last = first + digits.size();
    const auto ec = std::from_chars(first, last, mag, base).ec;
    if (ec == std::errc::result_out_of_range) {
        return make_float(text, negative);  // beyond uint64 -> a double-typed option value
    }

    if (negative) {
        const std::uint64_t int64_min_magnitude = std::uint64_t{1} << kInt64SignBitShift;
        if (mag > int64_min_magnitude) {
            return make_float(text, negative);  // beyond int64 -> a double-typed option value
        }
        return OptionValue{mag == int64_min_magnitude ? INT64_MIN
                                                      : -static_cast<std::int64_t>(mag)};
    }
    return OptionValue{mag};
}

OptionValue make_float(std::string_view text, bool negative) {
    double v = 0;
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): paired with size below
    const char* first = text.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars needs a range
    const char* last = first + text.size();
    const auto ec = std::from_chars(first, last, v).ec;
    if (ec == std::errc::result_out_of_range) {
        // from_chars leaves v unmodified on out-of-range for both extremes; classify.
        v = approx_decimal_exponent(text) >= 0 ? HUGE_VAL : 0.0;
    }
    if (negative) {
        v = -v;
    }
    return OptionValue{v};
}

// ScalarValue = ["-"|"+"] (intLit | floatLit | inf | nan)  ->  OptionValue
OptionValue interpret_number(const std::optional<Token>& sign, const Token& num) {
    const bool negative = sign.has_value() && sign->kind == TokenKind::Minus;
    if (num.kind == TokenKind::IntLiteral) {
        return make_int(num.text, negative);
    }
    if (num.kind == TokenKind::FloatLiteral) {
        return make_float(num.text, negative);
    }
    if (num.kind == TokenKind::KwInf) {
        return OptionValue{negative ? -HUGE_VAL : HUGE_VAL};
    }
    const double nan_value = std::nan("");  // the only remaining kind is KwNan
    return OptionValue{negative ? -nan_value : nan_value};
}

// --- recursive grammar points (forward decls) ------------------------------

Result<Parsed<OptionValue, Token>> parse_scalar(Range<Token> in);
Result<Parsed<MessageLiteral, Token>> parse_message_literal(Range<Token> in);
Result<Parsed<ListLiteral, Token>> parse_list_literal(Range<Token> in);
Result<Parsed<MessageLiteralField, Token>> parse_message_field(Range<Token> in);
Result<Parsed<OptionValue, Token>> parse_field_value(Range<Token> in);
Result<Parsed<std::vector<OptionNameComponent>, Token>> parse_option_name(Range<Token> in);

// --- combinator helpers (non-recursive grammar fragments) ------------------

// QualifiedIdent = name { "." name }  ->  dotted string
auto qualified_ident() {
    return map(seq(name_token(), many(preceded(kind(TokenKind::Dot), name_token()))),
               [](auto parts) {
                   std::string out(std::get<0>(parts).text);
                   for (const Token& t : std::get<1>(parts)) {
                       out += '.';
                       out.append(t.text);
                   }
                   return out;
               });
}

// FieldName = ident | "[" QualifiedIdent "]" | "[" QualifiedIdent "/" QualifiedIdent "]"
// Produces a MessageLiteralField with only the name fields set.
auto field_name() {
    return alt(map(name_token(),
                   [](const Token& t) {
                       MessageLiteralField f;
                       f.name = std::string(t.text);  // name_kind defaults to Simple
                       return f;
                   }),
               map(seq(kind(TokenKind::LBracket), qualified_ident(),
                       opt(preceded(kind(TokenKind::Slash), qualified_ident())),
                       kind(TokenKind::RBracket)),
                   [](auto parts) {
                       MessageLiteralField f;
                       auto& type = std::get<2>(parts);
                       if (type.has_value()) {  // [domain "/" type] -> Any type-URL
                           f.name_kind = MessageFieldNameKind::AnyTypeUrl;
                           f.name = std::move(std::get<1>(parts));
                           f.any_type = std::move(*type);
                       } else {
                           f.name_kind = MessageFieldNameKind::Extension;
                           f.name = std::move(std::get<1>(parts));
                       }
                       return f;
                   }));
}

// OptionNameComponent = ident | "(" [ "." ] QualifiedIdent ")"
auto option_name_component() {
    return alt(map(name_token(),
                   [](const Token& t) { return OptionNameComponent{std::string(t.text), false}; }),
               map(seq(kind(TokenKind::LParen), opt(kind(TokenKind::Dot)), qualified_ident(),
                       kind(TokenKind::RParen)),
                   [](auto parts) {
                       std::string name;
                       if (std::get<1>(parts).has_value()) {
                           name += '.';  // fully-qualified extension name, retained verbatim
                       }
                       name += std::get<2>(parts);
                       return OptionNameComponent{std::move(name), true};
                   }));
}

// MessageLiteral body: { MsgField [","|";"] }  ->  vector<MessageLiteralField>
auto message_body() {
    return map(many(seq(parse_message_field,
                        opt(alt(kind(TokenKind::Comma), kind(TokenKind::Semicolon))))),
               [](auto items) {
                   std::vector<MessageLiteralField> fields;
                   fields.reserve(items.size());
                   for (auto& item : items) {
                       fields.push_back(std::move(std::get<0>(item)));
                   }
                   return fields;
               });
}

// One compact option: Name "=" Value
auto compact_option() {
    return map(seq(parse_option_name, cut(kind(TokenKind::Equals)), cut(parse_value)),
               [](auto parts) {
                   Option opt;
                   opt.name = std::move(std::get<0>(parts));
                   opt.value = std::move(std::get<2>(parts));
                   return opt;
               });
}

// --- recursive grammar points (definitions) --------------------------------

// ScalarValue = signed number | StringLit | identifier
Result<Parsed<OptionValue, Token>> parse_scalar(Range<Token> in) {
    return alt(
        map(seq(opt(sign_token()), number_token()),
            [](auto parts) { return interpret_number(std::get<0>(parts), std::get<1>(parts)); }),
        map(kind(TokenKind::StringLiteral),
            [](Token t) { return OptionValue{std::move(t.str_value)}; }),
        map(name_token(),
            [](const Token& t) { return OptionValue{Identifier{std::string(t.text)}}; }))(in);
}

// OptionName = component { "." component }
Result<Parsed<std::vector<OptionNameComponent>, Token>> parse_option_name(Range<Token> in) {
    return map(
        seq(option_name_component(), many(preceded(kind(TokenKind::Dot), option_name_component()))),
        [](auto parts) {
            std::vector<OptionNameComponent> out;
            out.push_back(std::move(std::get<0>(parts)));
            for (auto& component : std::get<1>(parts)) {
                out.push_back(std::move(component));
            }
            return out;
        })(in);
}

// MsgField value: ":" Value | MessageLiteral | ListOfMessages (colon optional for the latter two)
Result<Parsed<OptionValue, Token>> parse_field_value(Range<Token> in) {
    return alt(
        preceded(kind(TokenKind::Colon), parse_value),
        map(parse_message_literal, [](MessageLiteral m) { return OptionValue{std::move(m)}; }),
        map(parse_list_literal, [](ListLiteral l) { return OptionValue{std::move(l)}; }))(in);
}

// MsgField = FieldName ( ":" Value | MessageValue )
Result<Parsed<MessageLiteralField, Token>> parse_message_field(Range<Token> in) {
    return map(seq(field_name(), parse_field_value), [](auto parts) {
        MessageLiteralField field = std::move(std::get<0>(parts));
        field.value = std::move(std::get<1>(parts));
        return field;
    })(in);
}

// MessageLiteral = "{" body "}" | "<" body ">"
Result<Parsed<MessageLiteral, Token>> parse_message_literal(Range<Token> in) {
    const DepthGuard guard;
    if (!guard.within_limit()) {
        return too_deep(in);
    }
    return alt(
        map(delimited(kind(TokenKind::LBrace), message_body(), cut(kind(TokenKind::RBrace))),
            [](std::vector<MessageLiteralField> f) { return MessageLiteral{std::move(f)}; }),
        map(delimited(kind(TokenKind::LAngle), message_body(), cut(kind(TokenKind::RAngle))),
            [](std::vector<MessageLiteralField> f) { return MessageLiteral{std::move(f)}; }))(in);
}

// ListLiteral = "[" [ Value { "," Value } ] "]"
Result<Parsed<ListLiteral, Token>> parse_list_literal(Range<Token> in) {
    const DepthGuard guard;
    if (!guard.within_limit()) {
        return too_deep(in);
    }
    return map(
        delimited(kind(TokenKind::LBracket), separated_list(parse_value, kind(TokenKind::Comma)),
                  cut(kind(TokenKind::RBracket))),
        [](std::vector<OptionValue> elems) { return ListLiteral{std::move(elems)}; })(in);
}

// --- enums ------------------------------------------------------------------

// Interpret a (sign-stripped) integer literal as an int32 enum/field number. Like the option
// helpers, this never fails: for valid input the literal is well-formed and in int32 range. An
// out-of-range magnitude (beyond uint64, or outside int32 once signed) silently yields an unspecified
// in-range value rather than erroring: a value-range concern for invalid input only, and
// memory-safe.
std::int32_t parse_int32(std::string_view text, bool negative) {
    const auto [base, digits] = split_int_literal(text);
    std::uint64_t mag = 0;
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): paired with size below
    const char* first = digits.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars needs a range
    const char* last = first + digits.size();
    std::from_chars(first, last, mag, base);
    const std::int64_t v =
        negative ? -static_cast<std::int64_t>(mag) : static_cast<std::int64_t>(mag);
    return static_cast<std::int32_t>(v);
}

template <typename T>
std::vector<T> prepend(T first, std::vector<T> rest) {
    std::vector<T> out;
    out.reserve(rest.size() + 1);
    out.push_back(std::move(first));
    for (auto& item : rest) {
        out.push_back(std::move(item));
    }
    return out;
}

// ["-"] intLit  ->  int32
auto signed_int() {
    return map(seq(opt(kind(TokenKind::Minus)), kind(TokenKind::IntLiteral)), [](auto parts) {
        return parse_int32(std::get<1>(parts).text, std::get<0>(parts).has_value());
    });
}

// Range = ["-"] intLit [ "to" ( ["-"] intLit | "max" ) ]; `max` -> the given sentinel.
auto reserved_range(std::int32_t max_sentinel) {
    auto bound =
        alt(map(kind(TokenKind::KwMax), [max_sentinel](const Token&) { return max_sentinel; }),
            signed_int());
    return map(seq(signed_int(), opt(preceded(kind(TokenKind::KwTo), bound))), [](auto parts) {
        NumberRange range;
        range.start = std::get<0>(parts);
        range.end = std::get<1>(parts).has_value() ? *std::get<1>(parts) : range.start;
        return range;
    });
}

// A reserved name is a string literal (proto2/proto3) or an identifier (editions).
auto reserved_name() {
    return alt(map(kind(TokenKind::StringLiteral), [](Token t) { return std::move(t.str_value); }),
               map(name_token(), [](const Token& t) { return std::string(t.text); }));
}

// ReservedDecl = "reserved" ( Range {"," Range} | Name {"," Name} ) ";"
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

// [ "export" | "local" ]  ->  optional<Visibility>
auto visibility_modifier() {
    return opt(alt(map(kind(TokenKind::KwExport), [](const Token&) { return Visibility::Export; }),
                   map(kind(TokenKind::KwLocal), [](const Token&) { return Visibility::Local; })));
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

// --- fields / maps / oneofs -------------------------------------------------

// TypeName = [ "." ] name { "." name }  (a scalar keyword or a message/enum reference),
// stored as written; resolved later by the type-resolution pass.
auto type_name() {
    return map(seq(opt(kind(TokenKind::Dot)), qualified_ident()), [](auto parts) {
        std::string out;
        if (std::get<0>(parts).has_value()) {
            out += '.';
        }
        out += std::get<1>(parts);
        return out;
    });
}

// A field/map number: a positive int literal (no sign).
auto field_number() {
    return map(kind(TokenKind::IntLiteral),
               [](const Token& t) { return parse_int32(t.text, false); });
}

enum class Cardinality : std::uint8_t { Required, Optional, Repeated };

auto cardinality() {
    return alt(
        map(kind(TokenKind::KwRequired), [](const Token&) { return Cardinality::Required; }),
        map(kind(TokenKind::KwOptional), [](const Token&) { return Cardinality::Optional; }),
        map(kind(TokenKind::KwRepeated), [](const Token&) { return Cardinality::Repeated; }));
}

RepeatedEncoding default_repeated_encoding(std::string_view type, SyntaxLevel syntax) {
    if (!is_packable_scalar(type)) {
        return RepeatedEncoding::Expanded;  // string/bytes/message; enums refined by type
                                            // resolution
    }
    // proto3 + editions default packed (editions refined by the feature pass); proto2 expanded.
    return syntax == SyntaxLevel::Proto2 ? RepeatedEncoding::Expanded : RepeatedEncoding::Packed;
}

// Build a FieldNode, normalizing presence/repeated-encoding from cardinality + syntax.
// Explicit `[packed]` overrides and editions feature refinement happen in later passes.
FieldNode build_field(std::optional<Cardinality> card, std::string type, std::string_view name,
                      std::int32_t number, std::optional<std::vector<Option>> options,
                      SyntaxLevel syntax) {
    FieldNode field;
    field.name = std::string(name);
    field.type_name = std::move(type);
    field.number = number;
    if (options.has_value()) {
        field.options = std::move(*options);
    }
    if (card == Cardinality::Repeated) {
        field.is_repeated = true;
        field.repeated_encoding = default_repeated_encoding(field.type_name, syntax);
    } else if (card == Cardinality::Required) {
        field.presence = FieldPresence::Required;
    } else if (!card.has_value() && syntax == SyntaxLevel::Proto3) {
        // proto3, no keyword: scalars/enums are implicit; message-typed fields are
        // promoted to Explicit by the type-resolution pass.
        field.presence = FieldPresence::Implicit;
    } else {
        // explicit `optional`; proto2 no-keyword; editions default (refined by the feature pass).
        field.presence = FieldPresence::Explicit;
    }
    return field;
}

// One oneof field: TypeName ident "=" intLit [CompactOptions] ";" (always explicit presence).
auto oneof_field() {
    return map(seq(type_name(), name_token(), cut(kind(TokenKind::Equals)), cut(field_number()),
                   opt(parse_compact_options), cut(kind(TokenKind::Semicolon))),
               [](auto parts) {
                   FieldNode field;
                   field.type_name = std::move(std::get<0>(parts));
                   field.name = std::string(std::get<1>(parts).text);
                   field.offset = std::get<1>(parts).byte_offset;  // the field name token
                   field.number = std::get<3>(parts);
                   field.presence = FieldPresence::Explicit;
                   if (std::get<4>(parts).has_value()) {
                       field.options = std::move(*std::get<4>(parts));
                   }
                   return field;
               });
}

using OneofElement = std::variant<FieldNode, Option, std::monostate>;

OneofNode assemble_oneof(std::string_view name, std::vector<OneofElement>& elements) {
    OneofNode node;
    node.name = std::string(name);
    for (auto& element : elements) {
        if (auto* field = std::get_if<FieldNode>(&element)) {
            node.fields.push_back(std::move(*field));
        } else if (auto* option = std::get_if<Option>(&element)) {
            node.options.push_back(std::move(*option));
        }
    }
    return node;
}

auto oneof_body() {
    return many(alt(map(parse_option_decl, [](Option o) { return OneofElement{std::move(o)}; }),
                    map(oneof_field(), [](FieldNode f) { return OneofElement{std::move(f)}; }),
                    map(kind(TokenKind::Semicolon),
                        [](const Token&) { return OneofElement{std::monostate{}}; })));
}

// --- messages / groups / extend / file --------------------------------------
// The parsers below are combinator-based recursive descent over nested messages / groups / extend
// bodies. Recursion depth is bounded by the DepthGuard at each body/aggregate entry (see
// kMaxParseDepth): a pathologically nested schema fails cleanly instead of overflowing the stack.

std::string to_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Overload-set helper for std::visit.
template <typename... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};
template <typename... Fs>
overloaded(Fs...) -> overloaded<Fs...>;

// A group yields both a synthesized message and an is_group field referencing it.
struct GroupDecl {
    FieldNode field;
    MessageNode message;
};

// An extend yields the ExtendNode plus any synthesized messages for groups in its body
// (which belong to the enclosing scope, not the ExtendNode).
struct ExtendBundle {
    ExtendNode node;
    std::vector<MessageNode> group_messages;
};

using MessageElement =
    std::variant<FieldNode, MapFieldNode, OneofNode, EnumNode, MessageNode, ReservedNode,
                 ExtensionRangeNode, ExtendBundle, Option, GroupDecl, std::monostate>;

// recursion (message body <-> nested message / group / extend)
Result<Parsed<std::vector<MessageElement>, Token>> message_body(Range<Token> in,
                                                                const ParseContext& ctx);
Result<Parsed<GroupDecl, Token>> parse_group(Range<Token> in, const ParseContext& ctx);
Result<Parsed<ExtendBundle, Token>> parse_extend(Range<Token> in, const ParseContext& ctx);

// ExtensionRangeDecl = "extensions" Range { "," Range } [CompactOptions] ";"
auto extension_range() {
    auto ranges =
        map(seq(reserved_range(kMaxMessageFieldNumber),
                many(preceded(kind(TokenKind::Comma), reserved_range(kMaxMessageFieldNumber)))),
            [](auto parts) {
                return prepend(std::move(std::get<0>(parts)), std::move(std::get<1>(parts)));
            });
    return map(seq(preceded(kind(TokenKind::KwExtensions), ranges), opt(parse_compact_options),
                   cut(kind(TokenKind::Semicolon))),
               [](auto parts) {
                   ExtensionRangeNode node;
                   node.ranges = std::move(std::get<0>(parts));
                   if (std::get<1>(parts).has_value()) {
                       node.options = std::move(*std::get<1>(parts));
                   }
                   return node;
               });
}

MessageNode assemble_message(std::string_view name, std::vector<MessageElement>& elements) {
    MessageNode node;
    node.name = std::string(name);
    for (auto& element : elements) {
        std::visit(overloaded{[&](FieldNode& f) { node.fields.push_back(std::move(f)); },
                              [&](MapFieldNode& m) { node.map_fields.push_back(std::move(m)); },
                              [&](OneofNode& o) { node.oneofs.push_back(std::move(o)); },
                              [&](EnumNode& e) { node.enums.push_back(std::move(e)); },
                              [&](MessageNode& m) { node.nested_messages.push_back(std::move(m)); },
                              [&](ReservedNode& r) { node.reserved.push_back(std::move(r)); },
                              [&](ExtensionRangeNode& x) {
                                  node.extension_ranges.push_back(std::move(x));
                              },
                              [&](ExtendBundle& b) {
                                  node.extends.push_back(std::move(b.node));
                                  for (auto& gm : b.group_messages) {
                                      node.nested_messages.push_back(std::move(gm));
                                  }
                              },
                              [&](Option& o) { node.options.push_back(std::move(o)); },
                              [&](GroupDecl& g) {
                                  node.fields.push_back(std::move(g.field));
                                  node.nested_messages.push_back(std::move(g.message));
                              },
                              [](std::monostate) {}},
                   element);
    }
    return node;
}

// One message body element. Keyword-led declarations come before parse_field, and group before
// field, per the alt-ordering contract (parse_field fatally commits after a leading keyword type).
auto message_element(const ParseContext& ctx) {
    return alt(
        map(parse_option_decl, [](Option o) { return MessageElement{std::move(o)}; }),
        map(parse_map_field, [](MapFieldNode m) { return MessageElement{std::move(m)}; }),
        map(parse_oneof, [](OneofNode o) { return MessageElement{std::move(o)}; }),
        map([](Range<Token> i) { return parse_reserved(i, kMaxMessageFieldNumber); },
            [](ReservedNode r) { return MessageElement{std::move(r)}; }),
        map(extension_range(), [](ExtensionRangeNode x) { return MessageElement{std::move(x)}; }),
        map([ctx](Range<Token> i) { return parse_extend(i, ctx); },
            [](ExtendBundle b) { return MessageElement{std::move(b)}; }),
        map([ctx](Range<Token> i) { return parse_message(i, ctx); },
            [](MessageNode m) { return MessageElement{std::move(m)}; }),
        map([ctx](Range<Token> i) { return parse_enum(i, ctx); },
            [](EnumNode e) { return MessageElement{std::move(e)}; }),
        map([ctx](Range<Token> i) { return parse_group(i, ctx); },
            [](GroupDecl g) { return MessageElement{std::move(g)}; }),
        map([ctx](Range<Token> i) { return parse_field(i, ctx); },
            [](FieldNode f) { return MessageElement{std::move(f)}; }),
        map(kind(TokenKind::Semicolon),
            [](const Token&) { return MessageElement{std::monostate{}}; }));
}

Result<Parsed<std::vector<MessageElement>, Token>> message_body(Range<Token> in,
                                                                const ParseContext& ctx) {
    const DepthGuard guard;
    if (!guard.within_limit()) {
        return too_deep(in);
    }
    return many(message_element(ctx))(in);
}

// GroupDecl = [Cardinality] "group" Ident "=" intLit [CompactOptions] "{" { MessageElement } "}".
// Synthesized into a nested message (named as written) + an is_group field (lowercased name).
Result<Parsed<GroupDecl, Token>> parse_group(Range<Token> in, const ParseContext& ctx) {
    const SyntaxLevel syntax = ctx.syntax_level;
    return map(
        seq(opt(cardinality()), preceded(kind(TokenKind::KwGroup), cut(name_token())),
            preceded(cut(kind(TokenKind::Equals)), cut(field_number())), opt(parse_compact_options),
            delimited(
                cut(kind(TokenKind::LBrace)),
                [ctx](Range<Token> i) { return message_body(i, ctx); },
                cut(kind(TokenKind::RBrace)))),
        [syntax](auto parts) {
            const std::string group_name(std::get<1>(parts).text);
            MessageNode message = assemble_message(group_name, std::get<4>(parts));
            FieldNode field =
                build_field(std::get<0>(parts), group_name, to_lower(group_name),
                            std::get<2>(parts), std::move(std::get<3>(parts)), syntax);
            field.offset = std::get<1>(parts).byte_offset;  // the group name token
            field.is_group = true;
            field.message_encoding = MessageEncoding::Delimited;
            return GroupDecl{std::move(field), std::move(message)};
        })(in);
}

// One extend body element: FieldDecl | GroupDecl | OptionDecl | ";".
using ExtendElement = std::variant<FieldNode, GroupDecl, Option, std::monostate>;

// Deliberate super-set of the published EBNF (which lists fields/options/empty only): proto2 protoc
// accepts group members inside `extend`, and we retain groups fully, so group is included.
auto extend_element(const ParseContext& ctx) {
    return alt(map(parse_option_decl, [](Option o) { return ExtendElement{std::move(o)}; }),
               map([ctx](Range<Token> i) { return parse_group(i, ctx); },
                   [](GroupDecl g) { return ExtendElement{std::move(g)}; }),
               map([ctx](Range<Token> i) { return parse_field(i, ctx); },
                   [](FieldNode f) { return ExtendElement{std::move(f)}; }),
               map(kind(TokenKind::Semicolon),
                   [](const Token&) { return ExtendElement{std::monostate{}}; }));
}

// ExtendDecl = "extend" TypeName "{" { FieldDecl | GroupDecl | OptionDecl | ";" } "}"
Result<Parsed<ExtendBundle, Token>> parse_extend(Range<Token> in, const ParseContext& ctx) {
    const std::size_t offset = in.empty() ? 0 : in.front().byte_offset;  // the `extend` keyword
    return map(
        seq(preceded(kind(TokenKind::KwExtend), cut(type_name())),
            delimited(cut(kind(TokenKind::LBrace)), many(extend_element(ctx)),
                      cut(kind(TokenKind::RBrace)))),
        [offset](auto parts) {
            ExtendBundle bundle;
            bundle.node.offset = offset;
            bundle.node.extendee_type_name = std::move(std::get<0>(parts));
            for (auto& element : std::get<1>(parts)) {
                std::visit(
                    overloaded{[&](FieldNode& f) { bundle.node.fields.push_back(std::move(f)); },
                               [&](GroupDecl& g) {
                                   bundle.node.fields.push_back(std::move(g.field));
                                   bundle.group_messages.push_back(std::move(g.message));
                               },
                               [&](Option& o) { bundle.node.options.push_back(std::move(o)); },
                               [](std::monostate) {}},
                    element);
            }
            return bundle;
        })(in);
}

// --- file-level declarations ------------------------------------------------

struct PackageName {
    std::string name;
};

struct SyntaxInfo {
    SyntaxLevel level = SyntaxLevel::Proto2;
    std::string edition;
};

// Consume a balanced { ... } block (tracking nesting); produce nothing. Used to skip `service`.
Result<Parsed<std::monostate, Token>> balanced_braces(Range<Token> in) {
    if (in.empty() || in.front().kind != TokenKind::LBrace) {
        return Error{0, "expected '{'"};
    }
    std::size_t depth = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i].kind == TokenKind::LBrace) {
            ++depth;
        } else if (in[i].kind == TokenKind::RBrace && --depth == 0) {
            return Parsed<std::monostate, Token>{in.subspan(i + 1), std::monostate{}};
        }
    }
    return Error{in.size(), "unterminated '{' block"};
}

// ImportDecl = "import" [ "weak" | "public" | "option" ] StringLit ";"
auto import_decl() {
    return map(seq(preceded(kind(TokenKind::KwImport),
                            opt(alt(kind(TokenKind::KwWeak), kind(TokenKind::KwPublic),
                                    kind(TokenKind::KwOption)))),
                   cut(kind(TokenKind::StringLiteral)), cut(kind(TokenKind::Semicolon))),
               [](auto parts) {
                   ImportNode node;
                   node.path = std::get<1>(parts).str_value;
                   if (std::get<0>(parts).has_value()) {
                       switch (std::get<0>(parts)->kind) {
                           case TokenKind::KwWeak:
                               node.kind = ImportKind::Weak;
                               break;
                           case TokenKind::KwPublic:
                               node.kind = ImportKind::Public;
                               break;
                           case TokenKind::KwOption:
                               node.kind = ImportKind::Option;
                               break;
                           default:
                               break;
                       }
                   }
                   return node;
               });
}

// PackageDecl = "package" QualifiedIdent ";"
auto package_decl() {
    return map(seq(preceded(kind(TokenKind::KwPackage), cut(qualified_ident())),
                   cut(kind(TokenKind::Semicolon))),
               [](auto parts) { return PackageName{std::move(std::get<0>(parts))}; });
}

// SkippedServiceDecl = "service" ident BalancedBraces  (consumed and DROPPED)
auto service_decl() {
    return map(seq(kind(TokenKind::KwService), cut(name_token()), cut(balanced_braces)),
               [](const auto&) { return std::monostate{}; });
}

// SyntaxDecl = "syntax" "=" StringLit ";" | "edition" "=" StringLit ";"
auto syntax_decl() {
    return alt(map(seq(preceded(kind(TokenKind::KwSyntax), cut(kind(TokenKind::Equals))),
                       cut(kind(TokenKind::StringLiteral)), cut(kind(TokenKind::Semicolon))),
                   [](auto parts) {
                       SyntaxInfo info;
                       info.level = std::get<1>(parts).str_value == "proto3" ? SyntaxLevel::Proto3
                                                                             : SyntaxLevel::Proto2;
                       return info;
                   }),
               map(seq(preceded(kind(TokenKind::KwEdition), cut(kind(TokenKind::Equals))),
                       cut(kind(TokenKind::StringLiteral)), cut(kind(TokenKind::Semicolon))),
                   [](auto parts) {
                       SyntaxInfo info;
                       info.level = SyntaxLevel::Edition;
                       info.edition = std::get<1>(parts).str_value;
                       return info;
                   }));
}

using FileElement = std::variant<ImportNode, PackageName, Option, MessageNode, EnumNode,
                                 ExtendBundle, std::monostate>;

auto file_element(const ParseContext& ctx) {
    return alt(map(parse_option_decl, [](Option o) { return FileElement{std::move(o)}; }),
               map(import_decl(), [](ImportNode i) { return FileElement{std::move(i)}; }),
               map(package_decl(), [](PackageName p) { return FileElement{std::move(p)}; }),
               map(service_decl(), [](std::monostate) { return FileElement{std::monostate{}}; }),
               map([ctx](Range<Token> i) { return parse_extend(i, ctx); },
                   [](ExtendBundle b) { return FileElement{std::move(b)}; }),
               map([ctx](Range<Token> i) { return parse_message(i, ctx); },
                   [](MessageNode m) { return FileElement{std::move(m)}; }),
               map([ctx](Range<Token> i) { return parse_enum(i, ctx); },
                   [](EnumNode e) { return FileElement{std::move(e)}; }),
               map(kind(TokenKind::Semicolon),
                   [](const Token&) { return FileElement{std::monostate{}}; }));
}

void apply_file_element(FileNode& file, FileElement& element) {
    std::visit(overloaded{[&](ImportNode& i) { file.imports.push_back(std::move(i)); },
                          [&](PackageName& p) { file.package = std::move(p.name); },
                          [&](Option& o) { file.options.push_back(std::move(o)); },
                          [&](MessageNode& m) { file.messages.push_back(std::move(m)); },
                          [&](EnumNode& e) { file.enums.push_back(std::move(e)); },
                          [&](ExtendBundle& b) {
                              file.extends.push_back(std::move(b.node));
                              for (auto& gm : b.group_messages) {
                                  file.messages.push_back(std::move(gm));
                              }
                          },
                          [](std::monostate) {}},
               element);
}

}  // namespace

// OptionValue = ScalarValue | MessageLiteral | ListLiteral
Result<Parsed<OptionValue, Token>> parse_value(Range<Token> in) {
    return alt(
        map(parse_message_literal, [](MessageLiteral m) { return OptionValue{std::move(m)}; }),
        map(parse_list_literal, [](ListLiteral l) { return OptionValue{std::move(l)}; }),
        parse_scalar)(in);
}

// OptionDecl = "option" OptionName "=" OptionValue ";"
Result<Parsed<Option, Token>> parse_option_decl(Range<Token> in) {
    return map(seq(kind(TokenKind::KwOption), cut(parse_option_name), cut(kind(TokenKind::Equals)),
                   cut(parse_value), cut(kind(TokenKind::Semicolon))),
               [](auto parts) {
                   Option opt;
                   opt.name = std::move(std::get<1>(parts));
                   opt.value = std::move(std::get<3>(parts));
                   return opt;
               })(in);
}

// CompactOptions = "[" Option { "," Option } "]"
Result<Parsed<std::vector<Option>, Token>> parse_compact_options(Range<Token> in) {
    return delimited(kind(TokenKind::LBracket),
                     separated_list(compact_option(), kind(TokenKind::Comma)),
                     cut(kind(TokenKind::RBracket)))(in);
}

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

// FieldDecl = [Cardinality] TypeName ident "=" intLit [CompactOptions] ";". The "= number"
// is folded into one tuple slot with preceded() so each element below is meaningful.
//
// ORDERING CONTRACT: parse_field consumes a leading keyword as a type name, then commits (cut)
// at "=". So in any enclosing alt it MUST come AFTER every keyword-led declaration (option, map,
// oneof, reserved, extensions, extend, message, enum, group). A `group` is byte-for-byte a field
// up to "= number", so group MUST be tried before field. (See enum_body for the same discipline.)
Result<Parsed<FieldNode, Token>> parse_field(Range<Token> in, const ParseContext& ctx) {
    const SyntaxLevel syntax = ctx.syntax_level;
    return map(seq(opt(cardinality()), type_name(), name_token(),
                   preceded(cut(kind(TokenKind::Equals)), cut(field_number())),
                   opt(parse_compact_options), cut(kind(TokenKind::Semicolon))),
               [syntax](auto parts) {
                   FieldNode field = build_field(std::get<0>(parts), std::move(std::get<1>(parts)),
                                                 std::get<2>(parts).text, std::get<3>(parts),
                                                 std::move(std::get<4>(parts)), syntax);
                   field.offset = std::get<2>(parts).byte_offset;  // the field name token
                   return field;
               })(in);
}

// MapFieldDecl = "map" "<" KeyType "," TypeName ">" ident "=" intLit [CompactOptions] ";". Each
// punctuation token is folded into the preceding meaningful element via preceded(). KeyType is
// parsed as an arbitrary type_name (not the spec's restricted integral/bool/string set); key-type
// validity is checked later in analysis (resolve_map), not here; the parser does no semantic
// checks.
Result<Parsed<MapFieldNode, Token>> parse_map_field(Range<Token> in) {
    return map(seq(preceded(seq(kind(TokenKind::KwMap), cut(kind(TokenKind::LAngle))),
                            cut(type_name())),                                   // key
                   preceded(cut(kind(TokenKind::Comma)), cut(type_name())),      // value
                   preceded(cut(kind(TokenKind::RAngle)), cut(name_token())),    // name
                   preceded(cut(kind(TokenKind::Equals)), cut(field_number())),  // number
                   opt(parse_compact_options), cut(kind(TokenKind::Semicolon))),
               [](auto parts) {
                   MapFieldNode node;
                   node.key_type = std::move(std::get<0>(parts));
                   node.value_type = std::move(std::get<1>(parts));
                   node.name = std::string(std::get<2>(parts).text);
                   node.offset = std::get<2>(parts).byte_offset;  // the field name token
                   node.number = std::get<3>(parts);
                   if (std::get<4>(parts).has_value()) {
                       node.options = std::move(*std::get<4>(parts));
                   }
                   return node;
               })(in);
}

// OneofDecl = "oneof" ident "{" { OneofElement } "}"
Result<Parsed<OneofNode, Token>> parse_oneof(Range<Token> in) {
    return map(
        seq(kind(TokenKind::KwOneof), cut(name_token()), cut(kind(TokenKind::LBrace)), oneof_body(),
            cut(kind(TokenKind::RBrace))),
        [](auto parts) { return assemble_oneof(std::get<1>(parts).text, std::get<3>(parts)); })(in);
}

// MessageDecl = [ "export"|"local" ] "message" ident "{" { MessageElement } "}"
Result<Parsed<MessageNode, Token>> parse_message(Range<Token> in, const ParseContext& ctx) {
    return map(seq(visibility_modifier(), kind(TokenKind::KwMessage), cut(name_token()),
                   delimited(
                       cut(kind(TokenKind::LBrace)),
                       [ctx](Range<Token> i) { return message_body(i, ctx); },
                       cut(kind(TokenKind::RBrace)))),
               [](auto parts) {
                   MessageNode node = assemble_message(std::get<2>(parts).text, std::get<3>(parts));
                   if (std::get<0>(parts).has_value()) {
                       node.visibility = *std::get<0>(parts);
                   }
                   return node;
               })(in);
}

// File = [ SyntaxDecl ] { FileElement }
Result<Parsed<FileNode, Token>> parse_file(Range<Token> in) {
    ParseContext ctx;  // absent syntax/edition decl => proto2
    std::string edition;
    Range<Token> rest = in;

    auto syntax = syntax_decl()(in);
    if (syntax.is_ok()) {
        ctx.syntax_level = syntax.value().value.level;
        edition = std::move(syntax.value().value.edition);
        rest = syntax.value().remaining;
    } else if (syntax.error().fatal) {
        return std::move(syntax.error());  // a malformed syntax/edition declaration
    }

    auto body = many(file_element(ctx))(rest);
    if (!body) {
        Error e = std::move(body.error());
        e.byte_offset += static_cast<std::size_t>(rest.data() - in.data());
        return e;
    }

    FileNode file;
    file.syntax_level = ctx.syntax_level;
    file.edition = std::move(edition);
    for (auto& element : body.value().value) {
        apply_file_element(file, element);
    }
    return Parsed<FileNode, Token>{body.value().remaining, std::move(file)};
}

}  // namespace rapidproto
