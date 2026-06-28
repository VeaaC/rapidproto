// Self-containment + strict-lint anchor for the header-only library.
//
// Each public header is included alone here, which (1) proves it compiles
// standalone under the strict warning flags, and (2) gives clang-tidy a non-test
// translation unit so the headers are linted under the strict ROOT .clang-tidy,
// independent of the relaxed tests/ config. This TU intentionally defines nothing.

#include "rapidproto/arena_runtime.hpp"              // IWYU pragma: keep
#include "rapidproto/arenagen/generator.hpp"         // IWYU pragma: keep
#include "rapidproto/arenagen/layout.hpp"            // IWYU pragma: keep
#include "rapidproto/arenagen/runtime_embedded.hpp"  // IWYU pragma: keep
#include "rapidproto/ast.hpp"                        // IWYU pragma: keep
#include "rapidproto/cli/driver.hpp"                 // IWYU pragma: keep
#include "rapidproto/codegen/emit.hpp"               // IWYU pragma: keep
#include "rapidproto/codegen/naming.hpp"             // IWYU pragma: keep
#include "rapidproto/codegen/printer.hpp"            // IWYU pragma: keep
#include "rapidproto/codegen/runtime_embedded.hpp"   // IWYU pragma: keep
#include "rapidproto/codegen/wire.hpp"               // IWYU pragma: keep
#include "rapidproto/combinators.hpp"                // IWYU pragma: keep
#include "rapidproto/features.hpp"                   // IWYU pragma: keep
#include "rapidproto/interpret.hpp"                  // IWYU pragma: keep
#include "rapidproto/lexer.hpp"                      // IWYU pragma: keep
#include "rapidproto/parser.hpp"                     // IWYU pragma: keep
#include "rapidproto/range.hpp"                      // IWYU pragma: keep
#include "rapidproto/resolve.hpp"                    // IWYU pragma: keep
#include "rapidproto/resolver.hpp"                   // IWYU pragma: keep
#include "rapidproto/result.hpp"                     // IWYU pragma: keep
#include "rapidproto/runtime.hpp"                    // IWYU pragma: keep
#include "rapidproto/scalar.hpp"                     // IWYU pragma: keep
#include "rapidproto/source.hpp"                     // IWYU pragma: keep
#include "rapidproto/source_id.hpp"                  // IWYU pragma: keep
#include "rapidproto/streamgen/generator.hpp"        // IWYU pragma: keep
#include "rapidproto/wellknown.hpp"                  // IWYU pragma: keep
