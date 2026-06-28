#include "rapidproto/lexer.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rapidproto/combinators.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

// The tokenizer is one combinator: many(alt(<one parser per token kind>)). Each
// token parser is a near-verbatim transcription of the corresponding production in
// the protobuf.com lexical grammar (the EBNF is quoted above each one), so the
// recognition layer can be read and validated line-by-line against the spec.
//
// `cut` is used once a construct is unambiguously entered (after an opening quote /
// "/*"), so a later failure (missing close quote, bad escape, missing "*/") becomes
// a precise, propagating error instead of a silent backtrack.
//
// Everything that is NOT grammar -- filtering whitespace/comments, classifying an
// identifier as a keyword, classifying a numeric token as int vs float, decoding
// string escape values, merging adjacent string literals -- is plain code in a
// post-pass over the raw token stream (build_tokens), rather than combinator grammar.

namespace rapidproto {
namespace {

using rapidproto::alt;
using rapidproto::many;
using rapidproto::map;
using rapidproto::one;
using rapidproto::opt;
using rapidproto::recognize;
using rapidproto::seq;
using rapidproto::tag;
using rapidproto::take_till;
using rapidproto::take_until;
using rapidproto::take_while;
using rapidproto::take_while1;

// --- character classes (spec) ----------------------------------------------

bool is_whitespace(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
}
bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_decimal_digit(char c) {
    return c >= '0' && c <= '9';
}
bool is_octal_digit(char c) {
    return c >= '0' && c <= '7';
}
bool is_hex_digit(char c) {
    return is_decimal_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_letter_or_digit(char c) {
    return is_letter(c) || is_decimal_digit(c);
}
bool is_nonzero_digit(char c) {
    return c >= '1' && c <= '9';
}
bool is_dot(char c) {
    return c == '.';
}
bool is_exp_marker(char c) {
    return c == 'e' || c == 'E';
}
bool is_sign(char c) {
    return c == '+' || c == '-';
}
bool is_x_marker(char c) {
    return c == 'x' || c == 'X';
}
bool is_eol_or_nul(char c) {
    return c == '\n' || c == '\0';
}
bool is_simple_escape_char(char c) {
    return c == 'a' || c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't' || c == 'v' ||
           c == '\\' || c == '\'' || c == '"' || c == '?';
}
int hex_value(char c) {
    constexpr int kDecimalToLetterGap = 10;
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + kDecimalToLetterGap;
    }
    return c - 'A' + kDecimalToLetterGap;
}

// --- keyword table & punctuation -------------------------------------------

TokenKind keyword_or_identifier(std::string_view text) {
    static const std::unordered_map<std::string_view, TokenKind> kKeywords = {
        {"syntax", TokenKind::KwSyntax},     {"edition", TokenKind::KwEdition},
        {"package", TokenKind::KwPackage},   {"import", TokenKind::KwImport},
        {"weak", TokenKind::KwWeak},         {"public", TokenKind::KwPublic},
        {"option", TokenKind::KwOption},     {"export", TokenKind::KwExport},
        {"local", TokenKind::KwLocal},       {"message", TokenKind::KwMessage},
        {"enum", TokenKind::KwEnum},         {"service", TokenKind::KwService},
        {"extend", TokenKind::KwExtend},     {"extensions", TokenKind::KwExtensions},
        {"rpc", TokenKind::KwRpc},           {"returns", TokenKind::KwReturns},
        {"stream", TokenKind::KwStream},     {"group", TokenKind::KwGroup},
        {"oneof", TokenKind::KwOneof},       {"required", TokenKind::KwRequired},
        {"optional", TokenKind::KwOptional}, {"repeated", TokenKind::KwRepeated},
        {"int32", TokenKind::KwInt32},       {"int64", TokenKind::KwInt64},
        {"uint32", TokenKind::KwUint32},     {"uint64", TokenKind::KwUint64},
        {"sint32", TokenKind::KwSint32},     {"sint64", TokenKind::KwSint64},
        {"fixed32", TokenKind::KwFixed32},   {"fixed64", TokenKind::KwFixed64},
        {"sfixed32", TokenKind::KwSfixed32}, {"sfixed64", TokenKind::KwSfixed64},
        {"float", TokenKind::KwFloat},       {"double", TokenKind::KwDouble},
        {"bool", TokenKind::KwBool},         {"string", TokenKind::KwString},
        {"bytes", TokenKind::KwBytes},       {"reserved", TokenKind::KwReserved},
        {"map", TokenKind::KwMap},           {"to", TokenKind::KwTo},
        {"max", TokenKind::KwMax},           {"inf", TokenKind::KwInf},
        {"nan", TokenKind::KwNan},
    };
    const auto it = kKeywords.find(text);
    return it == kKeywords.end() ? TokenKind::Identifier : it->second;
}

std::optional<TokenKind> symbol_kind(char c) {
    switch (c) {
        case ';':
            return TokenKind::Semicolon;
        case ',':
            return TokenKind::Comma;
        case '.':
            return TokenKind::Dot;
        case ':':
            return TokenKind::Colon;
        case '/':
            return TokenKind::Slash;
        case '=':
            return TokenKind::Equals;
        case '-':
            return TokenKind::Minus;
        case '+':
            return TokenKind::Plus;
        case '<':
            return TokenKind::LAngle;
        case '>':
            return TokenKind::RAngle;
        case '(':
            return TokenKind::LParen;
        case ')':
            return TokenKind::RParen;
        case '[':
            return TokenKind::LBracket;
        case ']':
            return TokenKind::RBracket;
        case '{':
            return TokenKind::LBrace;
        case '}':
            return TokenKind::RBrace;
        default:
            return std::nullopt;
    }
}
bool is_symbol_char(char c) {
    return symbol_kind(c).has_value();
}

// Render a byte for an error message: printable ASCII as itself, else \xHH.
std::string quote_byte(char c) {
    constexpr unsigned kPrintableLo = 0x20;
    constexpr unsigned kPrintableHi = 0x7E;
    constexpr unsigned kLowNibble = 0x0F;
    constexpr unsigned kNibbleBits = 4;
    const auto uc = static_cast<unsigned>(static_cast<unsigned char>(c));
    if (uc >= kPrintableLo && uc <= kPrintableHi) {
        // NOLINTNEXTLINE(modernize-return-braced-init-list): the (count, char) ctor
        return std::string(1, c);
    }
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out = "\\x";
    out.push_back(kHex[uc >> kNibbleBits]);
    out.push_back(kHex[uc & kLowNibble]);
    return out;
}

// --- the lexical grammar, as combinators (protobuf.com EBNF in comments) ----

// identifier = letter { letter | decimal_digit }
auto identifier() {
    return recognize(seq(one(is_letter), take_while(is_letter_or_digit)));
}

// digit_point_or_exp = "." | decimal_digit | ( "e" | "E" ) [ "+" | "-" ] | letter
auto digit_point_or_exp() {
    return alt(recognize(one(is_decimal_digit)), recognize(one(is_dot)),
               recognize(seq(one(is_exp_marker), opt(one(is_sign)))), recognize(one(is_letter)));
}

// numeric_literal = [ "." ] decimal_digit { digit_point_or_exp }   (the greedy munch;
// it is then classified as int/float in the post-pass, so "0.0.0"/"1to3" are single
// invalid tokens per the spec)
auto numeric_literal() {
    return recognize(seq(opt(tag(".")), one(is_decimal_digit), many(digit_point_or_exp())));
}

// simple_escape_seq  = `\` ( "a"|"b"|"f"|"n"|"r"|"t"|"v"|`\`|"'"|`"`|"?" )
auto simple_escape_seq() {
    return recognize(seq(tag("\\"), one(is_simple_escape_char)));
}
// hex_escape_seq     = `\` ( "x"|"X" ) hex_digit [ hex_digit ]
auto hex_escape_seq() {
    return recognize(seq(alt(tag("\\x"), tag("\\X")), one(is_hex_digit), opt(one(is_hex_digit))));
}
// octal_escape_seq   = `\` octal_digit [ octal_digit [ octal_digit ] ]
auto octal_escape_seq() {
    return recognize(
        seq(tag("\\"), one(is_octal_digit), opt(one(is_octal_digit)), opt(one(is_octal_digit))));
}
// unicode_escape_seq = `\u` hex{4} | `\U` hex{8}
auto unicode_escape_seq() {
    return alt(recognize(seq(tag("\\u"), one(is_hex_digit), one(is_hex_digit), one(is_hex_digit),
                             one(is_hex_digit))),
               recognize(seq(tag("\\U"), one(is_hex_digit), one(is_hex_digit), one(is_hex_digit),
                             one(is_hex_digit), one(is_hex_digit), one(is_hex_digit),
                             one(is_hex_digit), one(is_hex_digit))));
}
// rune_escape_seq = simple | hex | octal | unicode
auto rune_escape_seq() {
    return alt(simple_escape_seq(), hex_escape_seq(), octal_escape_seq(), unicode_escape_seq());
}

// A string body element: an ordinary char (not the quote, newline, NUL, or `\`) or
// an escape. `quote` selects the closing delimiter.
auto string_body_char(char quote) {
    return alt(recognize(one(
                   [quote](char c) { return c != quote && c != '\n' && c != '\0' && c != '\\'; })),
               rune_escape_seq());
}
// double_quoted_string_literal = `"` { body } `"`  -- commit after the opening quote
auto double_quoted_string() {
    return recognize(seq(tag("\""), cut(seq(many(string_body_char('"')), tag("\"")))));
}
auto single_quoted_string() {
    return recognize(seq(tag("'"), cut(seq(many(string_body_char('\'')), tag("'")))));
}
// string_literal = single_quoted_string_literal | double_quoted_string_literal
auto string_literal() {
    return alt(double_quoted_string(), single_quoted_string());
}

// line_comment = "//" { !("\n" | "\x00") }
auto line_comment() {
    return recognize(seq(tag("//"), take_till(is_eol_or_nul)));
}
// block_comment = "/*" ... "*/"  (non-nesting) -- commit after "/*" so a missing
// "*/" is a precise, propagating error rather than a stray '/' token
auto block_comment() {
    return recognize(seq(tag("/*"), cut(seq(take_until("*/"), tag("*/")))));
}
auto comment() {
    return alt(line_comment(), block_comment());
}

// --- numeric classification (post-pass, via the int/float grammars) ---------

// decimal_exponent = ( "e" | "E" ) [ "+" | "-" ] decimal_digits
auto decimal_exponent() {
    return seq(one(is_exp_marker), opt(one(is_sign)), take_while1(is_decimal_digit));
}
// int_literal = hex_literal | octal_literal | decimal_literal
auto int_literal() {
    return alt(recognize(seq(tag("0"), one(is_x_marker), take_while1(is_hex_digit))),
               recognize(seq(tag("0"), take_while1(is_octal_digit))),
               alt(tag("0"), recognize(seq(one(is_nonzero_digit), take_while(is_decimal_digit)))));
}
// float_literal = digits "." [digits] [exp] | digits exp | "." digits [exp]
auto float_literal() {
    return alt(recognize(seq(take_while1(is_decimal_digit), tag("."), take_while(is_decimal_digit),
                             opt(decimal_exponent()))),
               recognize(seq(take_while1(is_decimal_digit), decimal_exponent())),
               recognize(seq(tag("."), take_while1(is_decimal_digit), opt(decimal_exponent()))));
}
std::optional<TokenKind> classify_numeric(std::string_view text) {
    const Range<char> span(text);
    if (all_consuming(int_literal())(span)) {
        return TokenKind::IntLiteral;
    }
    if (all_consuming(float_literal())(span)) {
        return TokenKind::FloatLiteral;
    }
    return std::nullopt;
}

// --- the token stream -------------------------------------------------------

enum class Cat : std::uint8_t { Whitespace, Comment, StringLit, Numeric, Ident, Symbol };
struct RawToken {
    Cat cat;
    Range<char> span;
};

template <typename P>
auto raw(Cat cat, P parser) {
    return map(parser, [cat](Range<char> span) { return RawToken{cat, span}; });
}

auto token_stream() {
    return many(alt(raw(Cat::Whitespace, take_while1(is_whitespace)), raw(Cat::Comment, comment()),
                    raw(Cat::StringLit, string_literal()), raw(Cat::Numeric, numeric_literal()),
                    raw(Cat::Ident, identifier()),
                    raw(Cat::Symbol, recognize(one(is_symbol_char)))));
}

// --- string value decoding (post-pass) -------------------------------------

constexpr int kHexBase = 16;
constexpr int kOctalBase = 8;
constexpr int kMaxHexEscapeDigits = 2;
constexpr int kMaxOctalEscapeDigits = 3;
constexpr int kShortUnicodeDigits = 4;
constexpr int kLongUnicodeDigits = 8;

constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;
constexpr std::uint32_t kSurrogateLo = 0xD800;
constexpr std::uint32_t kSurrogateHi = 0xDFFF;
constexpr std::uint32_t k1ByteMax = 0x7F;
constexpr std::uint32_t k2ByteMax = 0x7FF;
constexpr std::uint32_t k3ByteMax = 0xFFFF;
constexpr std::uint32_t kLow6 = 0x3F;
constexpr std::uint32_t kContByte = 0x80;
constexpr std::uint32_t k2BytePrefix = 0xC0;
constexpr std::uint32_t k3BytePrefix = 0xE0;
constexpr std::uint32_t k4BytePrefix = 0xF0;
constexpr std::uint32_t kByteMask = 0xFF;
constexpr unsigned kShift6 = 6;
constexpr unsigned kShift12 = 12;
constexpr unsigned kShift18 = 18;

void append_utf8(std::string& out, std::uint32_t cp) {
    const auto byte = [&out](std::uint32_t v) { out.push_back(static_cast<char>(v & kByteMask)); };
    if (cp <= k1ByteMax) {
        byte(cp);
    } else if (cp <= k2ByteMax) {
        byte(k2BytePrefix | (cp >> kShift6));
        byte(kContByte | (cp & kLow6));
    } else if (cp <= k3ByteMax) {
        byte(k3BytePrefix | (cp >> kShift12));
        byte(kContByte | ((cp >> kShift6) & kLow6));
        byte(kContByte | (cp & kLow6));
    } else {
        byte(k4BytePrefix | (cp >> kShift18));
        byte(kContByte | ((cp >> kShift12) & kLow6));
        byte(kContByte | ((cp >> kShift6) & kLow6));
        byte(kContByte | (cp & kLow6));
    }
}

// span/i point at hex digits of a \u (4) or \U (8) escape (well-formed, since the
// string was recognized). Reads `digits`, validates the scalar, appends UTF-8.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): i (cursor, advanced) vs esc_off (error offset)
std::optional<Error> append_unicode_escape(std::string_view span, std::size_t& i, int digits,
                                           std::size_t esc_off, std::string& out) {
    std::uint32_t cp = 0;
    for (int k = 0; k < digits && i < span.size(); ++k) {
        cp = (cp * static_cast<std::uint32_t>(kHexBase)) +
             static_cast<std::uint32_t>(hex_value(span[i]));
        ++i;
    }
    if (cp > kMaxCodePoint) {
        return Error{esc_off, "unicode escape is out of range"};
    }
    if (cp >= kSurrogateLo && cp <= kSurrogateHi) {
        return Error{esc_off, "unicode escape is a surrogate code point"};
    }
    append_utf8(out, cp);
    return std::nullopt;
}

// Decode one escape sequence in `span` at index i (the backslash); advances i.
// Only \u/\U can fail (surrogate / out-of-range); other forms are well-formed
// because the string was recognized by the grammar.
std::optional<Error> decode_escape(std::string_view span, std::size_t& i, std::size_t base,
                                   std::string& out) {
    const std::size_t esc_off = base + i;
    ++i;  // past backslash
    const char e = span[i];
    switch (e) {
        case 'a':
            out.push_back('\a');
            ++i;
            return std::nullopt;
        case 'b':
            out.push_back('\b');
            ++i;
            return std::nullopt;
        case 'f':
            out.push_back('\f');
            ++i;
            return std::nullopt;
        case 'n':
            out.push_back('\n');
            ++i;
            return std::nullopt;
        case 'r':
            out.push_back('\r');
            ++i;
            return std::nullopt;
        case 't':
            out.push_back('\t');
            ++i;
            return std::nullopt;
        case 'v':
            out.push_back('\v');
            ++i;
            return std::nullopt;
        case '\\':
            out.push_back('\\');
            ++i;
            return std::nullopt;
        case '\'':
            out.push_back('\'');
            ++i;
            return std::nullopt;
        case '"':
            out.push_back('"');
            ++i;
            return std::nullopt;
        case '?':
            out.push_back('?');
            ++i;
            return std::nullopt;
        case 'u':
            ++i;
            return append_unicode_escape(span, i, kShortUnicodeDigits, esc_off, out);
        case 'U':
            ++i;
            return append_unicode_escape(span, i, kLongUnicodeDigits, esc_off, out);
        case 'x':
        case 'X': {
            ++i;
            std::uint32_t v = 0;
            for (int k = 0; k < kMaxHexEscapeDigits && i < span.size() && is_hex_digit(span[i]);
                 ++k) {
                v = (v * static_cast<std::uint32_t>(kHexBase)) +
                    static_cast<std::uint32_t>(hex_value(span[i]));
                ++i;
            }
            out.push_back(static_cast<char>(v & kByteMask));
            return std::nullopt;
        }
        default: {  // octal
            std::uint32_t v = 0;
            for (int k = 0; k < kMaxOctalEscapeDigits && i < span.size() && is_octal_digit(span[i]);
                 ++k) {
                v = (v * static_cast<std::uint32_t>(kOctalBase)) +
                    static_cast<std::uint32_t>(span[i] - '0');
                ++i;
            }
            out.push_back(static_cast<char>(v & kByteMask));
            return std::nullopt;
        }
    }
}

// Decode one recognized quoted-string span (quotes included) into `out`.
std::optional<Error> decode_string(std::string_view span, std::size_t base, std::string& out) {
    const std::size_t close = span.size() - 1;  // closing quote
    std::size_t i = 1;
    while (i < close) {
        if (span[i] == '\\') {
            if (auto e = decode_escape(span, i, base, out)) {
                return e;
            }
        } else {
            out.push_back(span[i]);
            ++i;
        }
    }
    return std::nullopt;
}

std::optional<Error> validate_comment(std::string_view span, std::size_t off) {
    const auto nul = span.find('\0');
    if (nul != std::string_view::npos) {
        return Error{off + nul, "null character in comment"};
    }
    return std::nullopt;
}

// --- post-pass: raw tokens -> classified, decoded tokens --------------------

// Decode the run of adjacent string literals (separated only by whitespace/comments)
// starting at raws[i] into one merged StringLiteral token; advances i past the run.
std::optional<Error> append_string_token(const std::vector<RawToken>& raws, std::size_t& i,
                                         std::string_view full, std::vector<Token>& tokens) {
    const std::size_t n = raws.size();
    const auto off = static_cast<std::size_t>(raws[i].span.data() - full.data());
    std::string value;
    if (auto e = decode_string(raws[i].span.to_string_view(), off, value)) {
        return e;
    }
    std::size_t end = off + raws[i].span.size();
    std::size_t j = i + 1;
    for (;;) {
        while (j < n && (raws[j].cat == Cat::Whitespace || raws[j].cat == Cat::Comment)) {
            if (raws[j].cat == Cat::Comment) {
                const auto coff = static_cast<std::size_t>(raws[j].span.data() - full.data());
                if (auto e = validate_comment(raws[j].span.to_string_view(), coff)) {
                    return e;
                }
            }
            ++j;
        }
        if (j >= n || raws[j].cat != Cat::StringLit) {
            break;
        }
        const auto noff = static_cast<std::size_t>(raws[j].span.data() - full.data());
        if (auto e = decode_string(raws[j].span.to_string_view(), noff, value)) {
            return e;
        }
        end = noff + raws[j].span.size();
        ++j;
    }
    tokens.push_back(
        Token{TokenKind::StringLiteral, full.substr(off, end - off), std::move(value), off});
    i = j;
    return std::nullopt;
}

Result<std::vector<Token>> build_tokens(const std::vector<RawToken>& raws, std::string_view full) {
    std::vector<Token> tokens;
    const std::size_t n = raws.size();
    std::size_t i = 0;
    while (i < n) {
        const RawToken& rt = raws[i];
        const auto off = static_cast<std::size_t>(rt.span.data() - full.data());
        const std::string_view sv = rt.span.to_string_view();
        switch (rt.cat) {
            case Cat::Whitespace:
                ++i;
                break;
            case Cat::Comment:
                if (auto e = validate_comment(sv, off)) {
                    return std::move(*e);
                }
                ++i;
                break;
            case Cat::Ident:
                tokens.push_back(Token{keyword_or_identifier(sv), sv, {}, off});
                ++i;
                break;
            case Cat::Symbol: {
                // Cat::Symbol was matched via is_symbol_char(sv[0]), which IS
                // symbol_kind(sv[0]).has_value(), so the lookup is always engaged here; assert the
                // invariant rather than suppress the unchecked-access check.
                const std::optional<TokenKind> kind = symbol_kind(sv[0]);
                assert(kind && "Cat::Symbol implies a symbol kind");
                tokens.push_back(Token{*kind, sv, {}, off});
                ++i;
                break;
            }
            case Cat::Numeric: {
                const auto kind = classify_numeric(sv);
                if (!kind) {
                    return Error{off, "invalid numeric literal '" + std::string(sv) + "'"};
                }
                tokens.push_back(Token{*kind, sv, {}, off});
                ++i;
                break;
            }
            case Cat::StringLit:
                if (auto e = append_string_token(raws, i, full, tokens)) {
                    return std::move(*e);
                }
                break;
        }
    }
    return tokens;
}

bool has_bom(std::string_view s) {
    constexpr unsigned char kB0 = 0xEF;
    constexpr unsigned char kB1 = 0xBB;
    constexpr unsigned char kB2 = 0xBF;
    return s.size() >= 3 && static_cast<unsigned char>(s[0]) == kB0 &&
           static_cast<unsigned char>(s[1]) == kB1 && static_cast<unsigned char>(s[2]) == kB2;
}

}  // namespace

Result<LexResult> lex(std::string source) {
    auto owned = std::make_unique<std::string>(std::move(source));
    const std::string_view full = *owned;
    const std::size_t start = has_bom(full) ? 3 : 0;
    const Range<char> input(full.data() + start, full.size() - start);

    auto stream = token_stream()(input);
    if (!stream) {
        // A committed (cut) failure: a missing close quote / bad escape / missing "*/".
        // This is at a later source position than everything already tokenized, so when
        // an input has multiple errors (e.g. an invalid numeric *then* an unterminated
        // string) the reported one may not be the source-leftmost. Files normally carry
        // one error at a time, so we accept this for the precision cut buys us.
        Error e = std::move(stream.error());
        e.byte_offset += start;
        return e;
    }

    auto built = build_tokens(stream.value().value, full);
    if (!built) {
        return std::move(built.error());
    }

    const Range<char> rem = stream.value().remaining;
    if (!rem.empty()) {
        const auto off = static_cast<std::size_t>(rem.data() - full.data());
        return Error{off, "unexpected character '" + quote_byte(full[off]) + "'"};
    }
    return LexResult{std::move(built).value(), std::move(owned)};
}

}  // namespace rapidproto
