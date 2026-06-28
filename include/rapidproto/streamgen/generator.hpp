#pragma once

// Streaming-decoder code generator: turns one analyzed FileNode into the C++ header text a
// consumer includes to decode that file's messages.

#include <string>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/naming.hpp"

namespace rapidproto::streamgen {

// Generate the streaming-decoder header text for `file` from a prebuilt C++ name table. When
// emitting a whole resolved set, build the table ONCE with `build_cpp_names(any_file, set.files,
// namespace_of(prefix))` and call this per file -- the table is identical for every file in the
// set, so this avoids an O(files^2) rebuild. The table also carries the namespace prefix.
//
// Precondition: `file` (and every file the table was built from) has been ANALYZED -- run
// `analyze()` on the ResolvedFileSet first; otherwise a field referencing a user-defined (message/
// enum) type would emit an empty type name.
std::string generate_header(const FileNode& file, const codegen::CppNameTable& symbols);

// Convenience: build the name table for `file` and emit. `all_files` is the whole resolved set
// (topological order; a file's imports precede it, and `file` is one of them) so a field whose type
// comes from an imported file resolves to that file's dedup-stable name. Pass
// `ResolvedFileSet::files`.
//
// `namespace_prefix` (dot-separated, e.g. "rp" or "my.decoders") is prepended to every file's C++
// namespace, so a proto `package a.b` becomes `namespace prefix::a::b`. Empty (the default) keeps
// protoc parity (`namespace a::b`); a prefix lets the generated decoders coexist with protoc's
// headers (which use the same `a::b`) in one translation unit.
std::string generate_header(const FileNode& file, const std::vector<FileNode>& all_files,
                            const std::string& namespace_prefix = {});

// Convenience for a self-contained file with no cross-file type references (only `file` is
// indexed).
std::string generate_header(const FileNode& file);

}  // namespace rapidproto::streamgen
