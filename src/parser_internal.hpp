#pragma once

// Shared between the parser's translation units. The parser is split across TUs purely for BUILD
// COST: `src/parser.cpp` was ~340s of the gate's ~290s clang-tidy stage -- one file on the critical
// path -- because the check matchers walk an AST whose size is driven by combinator width. The
// compile itself is only ~18s, so this buys parallelism, not less work.
//
// What lives here is deliberately narrow: the few `auto`-returning combinator helpers that are too
// small to be worth a call boundary (each TU instantiates its own copy, which is cheap), plus
// declarations for the concrete-return parsers the TUs call across the split. Anything with a
// concrete return type belongs in a .cpp and is declared here -- putting a large combinator in this
// header would instantiate it in every TU and give the width back.

#include <cstdint>
#include <string_view>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/combinators.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {
// A nested namespace, not the anonymous one: these have to be visible across the parser's TUs.
// Each .cpp does `using namespace parse_detail;` so call sites read exactly as they did when this
// was all one file.
namespace parse_detail {

inline bool is_keyword(TokenKind k) {
    return k >= TokenKind::KwSyntax && k <= TokenKind::KwNan;
}

// A name position accepts an identifier or a keyword (proto allows keywords as names -- e.g. a
// field named `message`).
inline bool is_name_token(const Token& t) {
    return t.kind == TokenKind::Identifier || is_keyword(t.kind);
}

// Match one token of the given kind; produces the Token.
inline auto kind(TokenKind k) {
    return one([k](const Token& t) { return t.kind == k; });
}

inline auto name_token() {
    return one([](const Token& t) { return is_name_token(t); });
}

// [ "export" | "local" ]  ->  optional<Visibility>. Used by both message and enum declarations.
inline auto visibility_modifier() {
    return opt(alt(map(kind(TokenKind::KwExport), [](const Token&) { return Visibility::Export; }),
                   map(kind(TokenKind::KwLocal), [](const Token&) { return Visibility::Local; })));
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

// --- defined in parser.cpp, called from parser_enum.cpp -----------------------------------------

// A (sign-stripped) integer literal as an int32 enum/field number. Stays in parser.cpp because it
// leans on the literal-splitting helper the option parsers use; declared rather than inlined so the
// split does not drag that whole chain into a header.
std::int32_t parse_int32(std::string_view text, bool negative);

// --- defined in parser_enum.cpp, called from parser.cpp -----------------------------------------
// (parse_enum lives there too, but it is public API and stays declared in rapidproto/parser.hpp.)

// Range = ["-"] intLit [ "to" ( ["-"] intLit | "max" ) ]; `max` -> the given sentinel.
Result<Parsed<NumberRange, Token>> reserved_range(Range<Token> in, std::int32_t max_sentinel);

// The lambda-wrapped spelling combinator call sites consume.
inline auto reserved_range(std::int32_t max_sentinel) {
    return [max_sentinel](Range<Token> in) { return reserved_range(in, max_sentinel); };
}

// ReservedDecl = "reserved" ( Range {"," Range} | Name {"," Name} ) ";"
Result<Parsed<ReservedNode, Token>> parse_reserved(Range<Token> in, std::int32_t max_sentinel);

}  // namespace parse_detail
}  // namespace rapidproto
