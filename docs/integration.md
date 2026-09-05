# The `rapidprotoc` CLI & CMake integration

*How generation is wired into a build. Back to the [README](../README.md).*

## The `rapidprotoc` CLI

```
rapidprotoc [options] <entry.proto>...
```

| Flag | Meaning |
|---|---|
| `--arena` | Emit the arena decoder (`<stem>.rp.hpp`). **The default** if neither model flag is given. |
| `--stream` | Emit the streaming decoder (`<stem>.rp.stream.hpp`). Combine with `--arena` to emit both. |
| `--dump` | Emit the [debug dumper](dumper.md) (`<stem>.rp.dump.hpp`), a JSON-like text dumper over the arena tree. Implies `--arena`. |
| `--unknown-present` | Arena: reserve the "unknown fields present" bit (`has_unknown_fields()`) on **every** message. |
| `--unknown=<message>` | Arena: reserve that bit on **one** message (repeatable; a one-line `unknown-fields` profile entry). |
| `--field-modes=<file>` | Arena: apply a decode profile file (repeatable; see [Decode profiles](profiles.md)). |
| `--drop=<name>` | Arena: drop one field or type inline (as a one-line profile entry). |
| `--raw=<name>` | Arena: keep a message field's or type's payloads for deferred `decode()`s, inline. |
| `-I <dir>` | Add an import search path (repeatable). |
| `--out-dir <dir>` | Where to write the headers (and `rapidproto/runtime.hpp`, plus `arena_runtime.hpp` for `--arena` and `dump_runtime.hpp` for `--dump`). Default: the current directory. |
| `--namespace-prefix <ns>` | Rename the root GENERATED code lives under — `<ns>::arena::pkg::Msg` and so on (default `rp`). Dot-separated; what is accepted is emitted verbatim, and a component that could not compile as written is refused instead — empty, a C++ keyword or `std`, a macro, `rp_`/`RP_`/`rapidproto` (the generator's own names), or a reserved identifier (`__x`, `_X...`, and a leading `_` in the first component). The runtime stays in `rapidproto::` either way. See [using both models](using-both-models.md). |
| `--no-wellknown` | Don't load the bundled well-known-type definitions. |
| `--depfile <path>` | Write a Make/Ninja depfile (the entries' headers depend on **every** input `.proto` and profile file) so a build regenerates when any input changes. Used by the CMake helper; harmless otherwise. |
| `--list-outputs` | Dry run: print every path a generation would write, relative to `--out-dir`, one per line (listed entries first, decoders before each file's common header) — nothing is written. The full resolve pipeline runs first, so a schema error fails the listing exactly as it would fail generation. This is how the CMake helper learns what to declare. Not combinable with `--list-inputs` or `--depfile`. |
| `--list-inputs` | Dry run: print the on-disk `.proto` closure (absolute, deduplicated; embedded well-known types excluded) — the files whose edits can change the output list. Same restrictions as `--list-outputs`. |
| `-v`, `--verbose` | Log each written file (`wrote <path>`); output is otherwise silent on success. |
| `-h`, `--help` | Print the full flag table and exit. |
| `--version` | Print the tool version and exit. |

Non-fatal `warning:` diagnostics (e.g. a schema using the MessageSet wire format) go to stderr
regardless of `-v`, and never change the exit code.

`<stem>` is the schema's path relative to the first `-I` directory that contains it, and its
basename when no `-I` does — so `-I proto proto/sub/a.proto` writes `sub/a.rp.hpp`, while
`sub/a.proto` with no `-I` writes `a.rp.hpp`. The output therefore always stays under `--out-dir`.
Three inputs are refused rather than resolved, because either answer would silently lose a schema:
two entries that generate the same header, two that share a name relative to the include paths (they
would deduplicate to one), and an `import` whose path escapes the output directory. Nothing is
written when one of these fires.

Multiple entries resolve as **one batch**: shared imports parse once, every file in the union
gets its decoder exactly once, and a decode profile resolves against all of them together. Each
generated file covers the entry **and** its transitive imports **and** the well-known types it
uses, plus the shared `<stem>.rp.common.hpp` and the runtime, so the output directory is
self-contained.

## CMake integration

RapidProto ships a `rapidproto_generate()` helper that turns a `.proto` into a linkable, header-only
target: it runs `rapidprotoc` at build time, tracks the whole import closure for correct **incremental
rebuilds** (a touched import re-triggers generation, via a depfile), and puts the output directory on
your include path. Link the target and `#include` the generated header; there's nothing else to wire up.

How much the build system knows about the generated files depends on whether the generator exists at
configure time. With `find_package` (an installed rapidproto), the helper asks the real generator
(`--list-outputs`) and declares **every** generated file — deleting any of them regenerates it, and a
schema error fails `cmake` itself with the generator's own diagnostic. In-tree and under FetchContent
the tool is built by your own buildsystem and cannot be asked yet, so the helper declares the listed
schemas' headers and the runtime copies; an *imported* schema's headers are still generated and kept
fresh, but deleting one by hand needs a regeneration (touch an entry, or rebuild from clean) to
recover. A `.proto` entry that another build rule produces (so it does not exist at configure time)
takes the same reduced path — which also means a *mistyped* entry path surfaces at build time, not
at configure. Two `rapidproto_generate()` targets must not share an `OUT_DIR` — the
helper refuses it at configure.

```cmake
rapidproto_generate(my_schema
  GENERATOR   both                  # arena | stream | both           (default: arena)
  PROTOS      proto/person.proto    # one or more entry .proto files
  IMPORT_DIRS proto)                # -I roots your schema imports against
  # also: NAMESPACE_PREFIX <ns>, OUT_DIR <dir>, UNKNOWN_PRESENT (arena), NO_WELLKNOWN, DUMP (arena dumper),
  #       FIELD_MODES <file>... / DROP <name>... / RAW <name>... / UNKNOWN <message>...  (arena profiles)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE my_schema)   # generates before `app` compiles, adds the include dir
```

Then `#include "person.rp.hpp"` (arena) and/or `"person.rp.stream.hpp"` (streaming); each is the
entry's stem under its import-relative path. `GENERATOR both` writes both decoders from one `rapidprotoc`
invocation, so a single TU can use both models for one schema (see
[Using both models](using-both-models.md)).

Get the helper and the `rapidproto::rapidprotoc` tool it drives, either way:

```cmake
# Build from source within your build:
include(FetchContent)
FetchContent_Declare(rapidproto
  GIT_REPOSITORY https://github.com/VeaaC/rapidproto
  GIT_TAG        v0.3.1)                       # pin the release you want -- but NOTE: v0.3.1
                                               # predates the rp:: model roots this manual
                                               # documents (arena `pkg::Msg` and streaming
                                               # `pkg::stream::Msg`, not `rp::arena::pkg::Msg` /
                                               # `rp::stream::pkg::Msg`); until 0.4.0 is
                                               # released, pin a commit from main instead
FetchContent_MakeAvailable(rapidproto)         # defines rapidproto_generate() + rapidproto::rapidprotoc

# …or use an installed RapidProto (cmake --install <build> --prefix <prefix>):
find_package(rapidproto REQUIRED)              # same helper + tool, imported
```

Both expose the identical `rapidproto::rapidprotoc` target, so one `rapidproto_generate()` call is
source-agnostic.

**CMake version.** Incremental import-tracking uses `add_custom_command(DEPFILE)`: supported on Ninja at
any version, and on the Makefile generators with CMake ≥ 3.20 (Xcode / Visual Studio ≥ 3.21). On an
older CMake with those generators the helper still generates correctly but won't auto-retrigger on an
import edit (it warns); re-run CMake or clean-build after editing an imported `.proto`.

**Cross-compiling.** `rapidprotoc` must run on the **build host**, not the target, so it must be a
**host build**. Build/install RapidProto for the host and bring that host tool in (e.g. a host-prefixed
`find_package`). `rapidproto_generate()` rejects the in-tree (target-built) tool when
`CMAKE_CROSSCOMPILING` is set; ensure the imported `rapidproto::rapidprotoc` it sees is a host binary.
