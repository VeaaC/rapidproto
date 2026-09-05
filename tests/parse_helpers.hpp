// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The two parse primitives every front-end suite builds on: lex + parse-whole-file, requiring
// success AND full consumption for the ok path, and defining "rejected" as "errored OR left tokens
// unconsumed" for the negative one. Before this header those two definitions were restated in
// ~18 per-file helpers (three of which spelled rejection `!r.is_ok()` and five `r.is_err()`);
// each suite now keeps only its one-line domain wrapper.

#include <string>
#include <utility>

#include "catch_amalgamated.hpp"
#include "rapidproto/lexer.hpp"
#include "rapidproto/parser.hpp"
#include "rapidproto/range.hpp"

namespace rapidproto::test {

// Lex `src`, parse the whole file, require success + full consumption, return the FileNode.
inline FileNode parse_file_ok(std::string src) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());
    return std::move(r.value().value);
}

// Whether the whole-file parse REJECTS `src`: an error, or tokens left unconsumed (a parse that
// silently stopped early is as wrong as one that failed).
inline bool parse_file_rejects(std::string src) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = parse_file(Range<Token>(lexed.tokens));
    return r.is_err() || !r.value().remaining.empty();
}

// The generic sub-parser pair: run `fn` (any parser entry point) over the lexed tokens.
// Same ok/rejected definitions as the whole-file forms above.
template <typename Fn>
auto parse_ok(std::string src, Fn fn) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = fn(Range<Token>(lexed.tokens));
    REQUIRE(r.is_ok());
    CHECK(r.value().remaining.empty());  // consumed every token
    return std::move(r.value().value);
}

template <typename Fn>
bool parse_rejects(std::string src, Fn fn) {
    auto lr = lex(std::move(src));
    REQUIRE(lr.is_ok());
    const LexResult lexed = std::move(lr).value();
    auto r = fn(Range<Token>(lexed.tokens));
    return r.is_err() || !r.value().remaining.empty();
}

}  // namespace rapidproto::test
