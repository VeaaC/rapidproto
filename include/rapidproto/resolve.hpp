#pragma once

// Name resolution passes over the parsed AST.
//
// FQN computation (compute_fqns): compute the fully-qualified name (FQN) of every named
// element — messages, enums, enum values, and extension fields — in place. FQNs are
// absolute (leading '.'), e.g. ".pkg.Outer.Inner". Enum VALUES are sibling-scoped: their
// FQN is qualified by the enum's ENCLOSING scope, not the enum itself (".pkg.VALUE", not
// ".pkg.Enum.VALUE"), matching protobuf name scoping. Extension fields are qualified by
// their DECLARATION scope, not the extendee.
//
// Type resolution (resolve_types): resolve every type reference (field/map/extendee/
// extension/group types) to its FQN and kind, build a symbol table + extension registry.
// Requires compute_fqns first.

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include "rapidproto/ast.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {

struct ResolvedFileSet;  // defined in rapidproto/resolver.hpp

void compute_fqns(FileNode& file);
void compute_fqns(ResolvedFileSet& file_set);

enum class SymbolKind : std::uint8_t { Message, Enum };

struct SymbolTable {
    std::unordered_map<std::string, SymbolKind> symbols;  // FQN -> kind
    // FQN -> the defining node. A generator-agnostic type index: built here, during the same walk
    // that fills `symbols`, so code generators (which need to inspect or recurse into a referenced
    // type) reuse it instead of re-walking the AST. The pointers reference nodes inside `file_set`,
    // which must outlive the table.
    std::unordered_map<std::string, const MessageNode*> messages;
    std::unordered_map<std::string, const EnumNode*> enums;
    // (extendee FQN, field number) -> the extension FieldNode (parsers need this). The pointers
    // reference the FieldNodes inside `file_set`, which must outlive the table.
    std::map<std::pair<std::string, std::int32_t>, const FieldNode*> extensions;
};

// Resolve all type references in `file_set` in place (writing resolved_type_fqn/is_message_type/
// is_enum_type, plus the post-resolution presence/encoding fixup for user-defined types) and return
// the symbol table + extension registry. Errors on an unresolved or not-visible type reference.
// Precondition: compute_fqns(file_set) has run.
Result<SymbolTable> resolve_types(ResolvedFileSet& file_set);

// Run all semantic passes in order on a resolved file set: editions features ->
// FQN computation -> type resolution -> decode-relevant option interpretation.
// Returns the symbol table + extension registry, or the first error. This is the top-level entry a
// parser/codegen consumer uses after the import resolver builds the file set.
Result<SymbolTable> analyze(ResolvedFileSet& file_set);

}  // namespace rapidproto
