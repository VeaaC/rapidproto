#pragma once

// Lexer: source text -> a flat token stream.
//
// Comments and whitespace are discarded. String literals are fully escape-decoded
// (into Token::str_value) and adjacent string literals are merged into one token.
// Integer vs float literals are classified syntactically here; their numeric value
// is interpreted later. Keywords are recognized via a lookup table — note that the
// parser may still accept a keyword token where an identifier is expected (proto
// allows keywords as names), so this is purely lexical classification.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rapidproto/result.hpp"

namespace rapidproto {

enum class TokenKind : std::uint8_t {
    // Literals / names
    Identifier,
    IntLiteral,
    FloatLiteral,
    StringLiteral,

    // Keywords. The count is pinned in src/lexer.cpp: an assert derives it from the
    // KwSyntax..KwNan enumerator range and compares it to the table's size, so an enumerator
    // added without a table entry (which would lex as Identifier, silently) trips every debug
    // lexer-test run. (A runtime assert, not a static_assert -- map sizes are not constexpr.)
    KwSyntax,  // KEEP FIRST of the keyword block (the count assertion anchors here)
    KwEdition,
    KwPackage,
    KwImport,
    KwWeak,
    KwPublic,
    KwOption,
    KwExport,
    KwLocal,
    KwMessage,
    KwEnum,
    KwService,
    KwExtend,
    KwExtensions,
    KwRpc,
    KwReturns,
    KwStream,
    KwGroup,
    KwOneof,
    KwRequired,
    KwOptional,
    KwRepeated,
    KwInt32,
    KwInt64,
    KwUint32,
    KwUint64,
    KwSint32,
    KwSint64,
    KwFixed32,
    KwFixed64,
    KwSfixed32,
    KwSfixed64,
    KwFloat,
    KwDouble,
    KwBool,
    KwString,
    KwBytes,
    KwReserved,
    KwMap,
    KwTo,
    KwMax,
    KwInf,
    KwNan,  // KEEP LAST of the keyword block (the count assertion anchors here)

    // Symbols. The spec defines 15 punctuation tokens; `Plus` ('+') is a deliberate
    // extension — '+' is not a standalone token in the spec (it appears only inside a
    // numeric exponent), but the option-value grammar allows a leading '+' on scalars,
    // so the lexer emits it and the parser decides where it is valid.
    Semicolon,  // ;
    Comma,      // ,
    Dot,        // .
    Colon,      // :
    Slash,      // /
    Equals,     // =
    Minus,      // -
    Plus,       // +
    LAngle,     // <
    RAngle,     // >
    LParen,     // (
    RParen,     // )
    LBracket,   // [
    RBracket,   // ]
    LBrace,     // {
    RBrace,     // }
};

struct Token {
    TokenKind kind;
    std::string_view text;        // the token's span in the source buffer
    std::string str_value;        // decoded value (string literals only; empty otherwise)
    std::size_t byte_offset = 0;  // offset of `text` within the source buffer
};

// The lexed token stream plus the source it views into. `source` is held behind a
// pointer so the std::string object never moves: Token::text string_views into it
// stay valid even when the LexResult (or the enclosing Result) is moved.
struct LexResult {
    std::vector<Token> tokens;
    std::unique_ptr<std::string> source;
};

// Lex `source` (consumed). On success the tokens reference *result.source.
Result<LexResult> lex(std::string source);

}  // namespace rapidproto
