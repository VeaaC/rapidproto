#pragma once

// The schema parser: token stream -> AST. Built on the same Range/Result/Parsed
// combinators as the lexer (now over Range<Token>), with concrete recursive-descent
// functions at the recursive grammar points (per the protobuf.com grammar).
//
// It implements the full schema grammar (file, message, field, enum, oneof, map,
// options, literals). Error offsets are token indices into the input range; the import
// resolver maps them back to source byte offsets via Token::byte_offset.

#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/lexer.hpp"  // Token, TokenKind
#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {

// Threaded through the declaration parsers: the syntax level drives presence/enum
// openness normalization (editions are refined later by the feature pass).
struct ParseContext {
    SyntaxLevel syntax_level = SyntaxLevel::Proto2;  // absent syntax/edition decl => proto2
    std::string edition;
};

// OptionValue = ScalarValue | MessageLiteral | ListLiteral
Result<Parsed<OptionValue, Token>> parse_value(Range<Token> in);

// OptionDecl = "option" OptionName "=" OptionValue ";"
Result<Parsed<Option, Token>> parse_option_decl(Range<Token> in);

// CompactOptions = "[" Option { "," Option } "]"  (inline field/enum-value options)
Result<Parsed<std::vector<Option>, Token>> parse_compact_options(Range<Token> in);

// EnumDecl = [ "export" | "local" ] "enum" ident "{" { EnumElement } "}"
Result<Parsed<EnumNode, Token>> parse_enum(Range<Token> in, const ParseContext& ctx);

// FieldDecl = [ "required"|"optional"|"repeated" ] TypeName ident "=" intLit [CompactOptions] ";"
// Presence/repeated-encoding are normalized from `ctx`; proto3 message-typed presence is finalized
// later by the type-resolution pass, editions presence/encoding by the feature pass.
Result<Parsed<FieldNode, Token>> parse_field(Range<Token> in, const ParseContext& ctx);

// MapFieldDecl = "map" "<" KeyType "," TypeName ">" ident "=" intLit [CompactOptions] ";"
Result<Parsed<MapFieldNode, Token>> parse_map_field(Range<Token> in);

// OneofDecl = "oneof" ident "{" { TypeName ident "=" intLit [CompactOptions] ";" | OptionDecl | ";"
// } "}"
Result<Parsed<OneofNode, Token>> parse_oneof(Range<Token> in);

// MessageDecl = [ "export"|"local" ] "message" ident "{" { MessageElement } "}". Nested messages,
// groups (synthesized as a nested message + an is_group field), extend, and extension ranges are
// all handled within the body.
Result<Parsed<MessageNode, Token>> parse_message(Range<Token> in, const ParseContext& ctx);

// File = [ SyntaxDecl ] { FileElement }. The top-level entry point: determines the syntax level
// from an optional syntax/edition declaration (absent => proto2), then parses file elements.
// `service` blocks are parsed past and dropped (gRPC out of scope).
Result<Parsed<FileNode, Token>> parse_file(Range<Token> in);

}  // namespace rapidproto
