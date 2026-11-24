# CLI & CMake

`rapidprotoc` is RapidProto's generator for C++. It reads `.proto` schemas and writes their decoders
as headers, along with a copy of the runtime that those headers include. You can run it yourself or
have [`rapidproto_generate()`](#cmake-integration) run it as part of a CMake build. Rust projects
generate from `build.rs` with
[`rapidproto-build`](https://docs.rs/rapidproto-build/latest/rapidproto_build/) instead.

The examples below use `proto/shop/order.proto`, which imports a second schema and a well-known
type:

```proto
// proto/shop/order.proto
syntax = "proto3";
package shop;

import "shop/money.proto";
import "google/protobuf/timestamp.proto";

message Order {
  string id = 1;
  Money total = 2;
  google.protobuf.Timestamp placed_at = 3;
}
```

`proto/shop/money.proto` defines `Money` and an enum, `Currency`. Generating the arena decoder for
`order.proto` writes headers for it and for everything it imports:

```sh
rapidprotoc -v -I proto --out-dir=gen proto/shop/order.proto
```

```text
wrote gen/shop/order.rp.hpp
wrote gen/shop/order.rp.common.hpp
wrote gen/shop/money.rp.hpp
wrote gen/shop/money.rp.common.hpp
wrote gen/google/protobuf/timestamp.rp.hpp
wrote gen/google/protobuf/timestamp.rp.common.hpp
wrote gen/rapidproto/runtime.hpp
wrote gen/rapidproto/arena_runtime.hpp
```

The two files under `rapidproto/` are the runtime. Nothing under `proto` provides
`timestamp.proto`. The generator used its bundled copy instead.

## The `rapidprotoc` CLI

```text
rapidprotoc [options] <entry.proto>...
```

Every argument without a leading `-` names an entry, a schema you want generated. In the tables,
`<stem>` is a schema's path below its `-I` directory, minus `.proto` ([File names](#file-names) has
the full rule). The run also writes headers for every schema the entries import.

| Flag | Effect |
| --- | --- |
| `-I<dir>` | look for imported schemas in `<dir>`; repeatable, tried in command-line order |
| `--out-dir=<dir>` | write the headers and the runtime under `<dir>`, creating it; default `.` |
| `--arena` | write `<stem>.rp.hpp`, the [arena decoder](arena.md); the default without `--stream` |
| `--stream` | write `<stem>.rp.stream.hpp`, the [streaming decoder](streaming.md); with `--arena`, both |
| `--dump` | write `<stem>.rp.dump.hpp`, the [debug dumper](dumper.md); implies `--arena` |
| `--namespace-prefix=<ns>` | set the [root namespace](#the-root-namespace); default `rp` |
| `--no-wellknown` | don't fall back on the bundled [well-known types](semantics.md#well-known-types) |
| `-v`, `--verbose` | print `wrote <path>` for each file written |
| `-h`, `--help` | print the usage and the flags |
| `--version` | print the version |

The [decode profile](profiles.md) flags can be repeated, and together they make up one profile that
changes the arena decoder and nothing else. In a run that doesn't generate the arena decoder, such
as one with `--stream` alone, any of them is a usage error.

| Flag | Effect |
| --- | --- |
| `--field-modes=<file>` | read [profile directives](profiles.md#writing-a-profile) from `<file>` |
| `--drop=<name>` | [drop](profiles.md#dropping-fields) a field, or every field whose type is `<name>` |
| `--raw=<name>` | keep a sub-message field, or every field whose type is `<name>`, as [raw bytes](profiles.md#raw-sub-messages) |
| `--unknown=<message>` | add [`has_unknown_fields()`](profiles.md#unknown-fields) to one message |
| `--unknown-present` | add `has_unknown_fields()` to every message |

`-I`, `--out-dir`, `--namespace-prefix` and [`--depfile`](#depfiles-and-dry-runs) can also take
their value from the following argument, as in `-I proto`. The profile flags that take a value
can't. Given `--drop shop.Order.placed_at`, `rapidprotoc` prints `error: unknown flag '--drop'`.

`rapidprotoc` exits with 0 on success and with 1 when it can't generate, for example on a schema
error or a missing import. A usage error, such as an unknown flag or a refused `--namespace-prefix`,
exits with 2. Warnings, such as the one for a message in the MessageSet wire format, go to stderr
and don't change the exit code.

### File names

Each file generated for a schema is named by its stem plus a suffix such as `.rp.hpp`. For an entry,
the stem is its path below the first `-I` directory that holds it, minus `.proto`. With `-I proto`,
`proto/shop/order.proto` has the stem `shop/order`, and its arena decoder is the
`gen/shop/order.rp.hpp` in the listing above. With `-I proto/shop -I proto` the stem is just
`order`, because the first directory already holds `order.proto`. An entry that none of the `-I`
directories holds gets its file name minus `.proto` as the stem. For an imported schema the stem is
the path written in the `import` statement, which is how `money.proto` became `shop/money`.

Two entries can't have the same stem. Without an `-I`, `api/v1/order.proto` and `api/v2/order.proto`
would both be `order`. Passing both, as in
`rapidprotoc --out-dir=gen api/v1/order.proto api/v2/order.proto`, fails without writing anything:

```text
error: '/home/me/shop/api/v1/order.proto' and '/home/me/shop/api/v2/order.proto' both generate gen/order.*
  one would overwrite the other; pass an -I they share, so each keeps a distinct path relative to it
```

With `-I api` added, the stems are `v1/order` and `v2/order`. The run also refuses an import such
as `import "../up/u.proto";`, whose headers would land outside the output directory.

### The root namespace

Generated code sits under one root namespace, `rp` unless you pick another. For `package shop;` the
arena decoder's classes are in `rp::arena::shop`, the streaming decoder's in `rp::stream::shop`, and
the enums in `rp::common::shop`. `--namespace-prefix` replaces the root, which you'd do when [`rp`
or a namespace inside it clashes](using-both-models.md#next-to-protoc-generated-code) with protoc's
generated code or with your own. `--namespace-prefix=acme` gives `acme::arena::shop` and so on,
while the runtime keeps its own namespace, `rapidproto`. A dotted value such as `acme.rp` nests the
root as `acme::rp::arena::shop`.

The value has to be C++ identifiers separated by dots. Keywords, `std`, `rapidproto`, names starting
with `rp_` or `RP_`, identifiers C++ reserves and common macro names such as `NULL` or `linux` are
refused as components, with an error that names the rule.

### Depfiles and dry runs

| Flag | Effect |
| --- | --- |
| `--depfile=<file>` | also write a Make-style depfile |
| `--list-outputs` | print each file the run would write, and write nothing |
| `--list-inputs` | print each schema file the run would read, and write nothing |

A depfile holds one rule. Its targets are the entries' decoder headers. Its prerequisites are the
`--field-modes` files and every schema the run read from disk, by absolute path. Adding
`--depfile=gen/order.d` to the first command writes:

```text
gen/shop/order.rp.hpp: /home/me/shop/proto/shop/money.proto /home/me/shop/proto/shop/order.proto
```

The bundled `timestamp.proto` isn't read from disk and doesn't appear.

`--list-outputs` prints paths relative to `--out-dir`. The entries come first, in the order the
command gives them, then the schemas they import, and the runtime last. `--list-inputs` prints
sorted absolute paths. A well-known type that an `-I` directory provides is read from there and
listed, but the bundled ones aren't. Both dry runs go through the same checks as a real run and fail
with the same errors. They can't be combined with each other or with `--depfile`.

## The output directory

Put the output directory itself on the include path. A header is then included by its stem and
suffix:

```cpp
#include "shop/order.rp.hpp"
```

```sh
g++ -std=c++17 -Igen main.cpp -o app
```

Generated headers include the runtime and the headers of imported schemas by paths such as
`"rapidproto/arena_runtime.hpp"` and `"shop/money.rp.hpp"`, which only resolve from the output
directory. With its parent directory on the include path instead, `#include "gen/shop/order.rp.hpp"`
fails on g++ with `rapidproto/arena_runtime.hpp: No such file or directory`.

The runtime is header-only and needs nothing beyond the standard library. There's nothing to build
or link for it. Which of its files a run writes depends on the decoders it generates:

| File | Written | Defines |
| --- | --- | --- |
| `rapidproto/runtime.hpp` | by every run | the wire reader, [`rapidproto::ByteView`](arena.md), [`rapidproto::DecodeStatus`](streaming.md#errors-and-stopping-early) |
| `rapidproto/arena_runtime.hpp` | with the arena decoder | [`rapidproto::Arena`](arena.md#the-arena), [`rapidproto::ArenaDecodeError`](arena.md#errors), [`rapidproto::decode_owned`](arena.md#owned-messages) |
| `rapidproto/dump_runtime.hpp` | with `--dump` | [`rapidproto::dump`](dumper.md), [`rapidproto::DumpOptions`](dumper.md#indentation-and-line-width) |

Each schema also gets a common header, `<stem>.rp.common.hpp`, whichever decoders you generate. It
defines the schema's enums. Because both decoder headers include it, they [share one type per
enum](using-both-models.md).

A generated header is compiled again in every source file that includes it, and the
[compile cost](benchmarks.md#compile-cost---what-the-throughput-costs-to-build) adds up. Keep such
includes out of headers that many files pull in.

A source file can't mix generated headers from two output directories. Two directories mean two
copies of the runtime and of every common header, and to `#pragma once` each copy is a separate
file. Say the arena decoder for `order.proto` is generated into `gen-arena`, the streaming decoder
into `gen-stream`, and `main.cpp` includes `shop/order.rp.hpp` and then `shop/order.rp.stream.hpp`:

```sh
clang++ -std=c++17 -Igen-arena -Igen-stream main.cpp -o app
```

```text
gen-stream/shop/money.rp.common.hpp:9:12: error: redefinition of 'Currency'
```

Which definition clashes first depends on the schemas and the `-I` order. With a top-level schema
such as `person.proto`, it can be the runtime's `redefinition of 'WireType'` instead.

g++ accepts the file as long as each pair of copies has the same contents and the same modification
time, to the second. Don't count on that. Two separate runs can easily write the copies a second
apart. Generate everything one source file includes in one run: both decoders, if it uses both, and
every schema it uses. In CMake that means a single target, with `GENERATOR both` where both decoders
are needed.

## CMake integration

```cmake
cmake_minimum_required(VERSION 3.16)
project(shop CXX)

find_package(rapidproto 0.5 REQUIRED)

rapidproto_generate(shop_schema
  GENERATOR   both
  PROTOS      proto/shop/order.proto
  IMPORT_DIRS proto)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE shop_schema)
```

The `find_package` line needs RapidProto installed. [Getting the helper](#getting-the-helper) shows
how to install it, or how to fetch it instead.

`rapidproto_generate()` makes `shop_schema` an interface library. Linking `app` to it is all the
setup `app` needs. The generator runs before `main.cpp` is compiled. Because `shop_schema` puts its
output directory on the include path, `main.cpp` can include `"shop/order.rp.hpp"` and
`"shop/order.rp.stream.hpp"` directly. The library also gives `app` the `cxx_std_17` compile
feature. That's only a minimum; a project built as C++20 stays on C++20. The
[consumer example](../examples/consumer) is a complete project set up this way.

A target's headers come from a single `rapidprotoc` run with all of its `PROTOS` as entries. The
build reruns it after an edit to one of those schemas, to a schema they import or to a `FIELD_MODES`
file. A new `rapidprotoc` binary reruns it too. To generate the headers without compiling anything,
build `<target>_generate`, here `shop_schema_generate`.

The helper needs CMake 3.16 or newer. With Makefiles below CMake 3.20, or with Xcode or Visual
Studio below 3.21, an edit to an imported schema regenerates nothing until you also touch an entry
in `PROTOS` or build from clean.

| Keyword | Meaning | Default |
| --- | --- | --- |
| `PROTOS <file>...` | the entries | required |
| `GENERATOR <decoders>` | `arena`, `stream` or `both` | `arena` |
| `DUMP` | `--dump` | off |
| `IMPORT_DIRS <dir>...` | the `-I` directories, in search order | none |
| `NAMESPACE_PREFIX <ns>` | `--namespace-prefix` | `rp` |
| `OUT_DIR <dir>` | `--out-dir` | `${CMAKE_CURRENT_BINARY_DIR}/rapidproto/<target>` |
| `NO_WELLKNOWN` | `--no-wellknown` | off |
| `FIELD_MODES <file>...` | `--field-modes` for each file | none |
| `DROP <name>...` | `--drop` for each name | none |
| `RAW <name>...` | `--raw` for each name | none |
| `UNKNOWN <message>...` | `--unknown` for each message | none |
| `UNKNOWN_PRESENT` | `--unknown-present` | off |

Relative paths in `PROTOS`, `IMPORT_DIRS` and `FIELD_MODES` are taken from the directory of the
`CMakeLists.txt` that calls `rapidproto_generate()`. A relative `OUT_DIR` is taken from there as
well, a known bug that puts the generated files in your source tree. Until it's fixed, give
`OUT_DIR` as an absolute path, for example `${CMAKE_CURRENT_BINARY_DIR}/gen`, or omit it to get the
default.

Configure fails if two targets share an `OUT_DIR`. It also fails when `GENERATOR stream` comes with
`DUMP` or one of the profile keywords, all of which need the arena decoder.

### Getting the helper

The [release archives](https://github.com/VeaaC/rapidproto/releases) have the `rapidprotoc` binary
but not the CMake package that `find_package` loads. To get both, build from the release tag and
install:

```sh
git clone https://github.com/VeaaC/rapidproto
cd rapidproto
git checkout v0.5.1
cmake --preset release
cmake --build --preset release --target rapidprotoc
cmake --install build/release --prefix /opt/rapidproto
```

The presets need CMake 3.21, while a project that uses the installed helper needs only 3.16.
Configure your project with `-DCMAKE_PREFIX_PATH=/opt/rapidproto`. The `0.5` in
`find_package(rapidproto 0.5 REQUIRED)` accepts any 0.5.x release but not 0.4 or 0.6, following the
[compatibility rules](../README.md#compatibility).

To compile the generator inside your own build instead, replace the `find_package` line with
FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(rapidproto
  GIT_REPOSITORY https://github.com/VeaaC/rapidproto
  GIT_TAG        v0.5.1)
FetchContent_MakeAvailable(rapidproto)
```

As a subproject, RapidProto skips its own tests and install rules and compiles without `-Werror`.
Either way, your own custom commands can run the generator through the `rapidproto::rapidprotoc`
target.

### Installed and fetched generators

A generator found with `find_package` is already built when CMake configures, whether you installed
it or Conan or vcpkg did. During configure the helper runs it with `--list-inputs` and
`--list-outputs`, which tells the build every schema the target reads and every header it writes,
including those for imported schemas. A schema or profile error, or a refused `NAMESPACE_PREFIX`,
then fails `cmake` itself. After an edit to any of those schemas, CMake configures again before the
headers are regenerated.

With FetchContent the generator doesn't exist until the build has compiled it. All the helper can
declare at configure time are the entries' headers and the runtime copy. The headers for imported
schemas are written anyway and regenerated after edits to those schemas. Delete one by hand,
though, and the next build fails on the missing header. Touch any of the target's schemas to get it
back. A broken schema or profile is reported only when the build runs the generator.

Even an installed generator can't list the outputs of an entry that doesn't exist yet at configure
time. The helper then declares no more than it does with FetchContent. That covers an entry
that a build rule creates later. It also covers a misspelled entry path, which passes configure and
then fails in Ninja:

```text
ninja: error: '/home/me/shop/proto/shop/oder.proto', needed by 'rapidproto/shop_schema/shop/oder.rp.hpp', missing and no known rule to make it
```

### Package managers

The repository has a [Conan recipe](https://github.com/VeaaC/rapidproto/blob/main/conanfile.py) and
a [vcpkg overlay port](https://github.com/VeaaC/rapidproto/tree/main/ports/rapidproto). Neither is
in [ConanCenter](https://conan.io/center) or in vcpkg's [public
registry](https://vcpkg.io/en/packages). You use them from a clone of the RapidProto repository.
They install the CMake package that `cmake --install` writes. With your project configured through
Conan's [toolchain](https://docs.conan.io/2/reference/tools/cmake/cmaketoolchain.html) or vcpkg's
[toolchain file](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration), the
`CMakeLists.txt` above needs no changes. A vcpkg [cross build](#cross-compiling) needs one more
setting when you configure.

For Conan, run this in a checkout of the v0.5.1 tag, made as [above](#getting-the-helper):

```sh
conan create . --build=missing
```

It builds the generator and stores it in your local Conan cache as `rapidproto/0.5.1`. Run on main,
the same command would store unreleased code under that name. Your own recipe then lists RapidProto
as a tool requirement, as the repository's [test
package](https://github.com/VeaaC/rapidproto/tree/main/test_package) does:

```python
from conan import ConanFile


class ShopConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeToolchain"

    def build_requirements(self):
        self.tool_requires("rapidproto/0.5.1")
```

There's no `CMakeDeps` in it because the RapidProto package carries its own CMake config file, which
`find_package` loads directly.

For vcpkg, take the port from a clone of main, where it builds the latest release, v0.5.1. A
release's hash is added to the port on main only after the release is tagged. The copy at the tag
fails its hash check. Register the clone's `ports` directory as an
[overlay](https://learn.microsoft.com/en-us/vcpkg/concepts/overlay-ports). Pass
`--overlay-ports=<clone>/ports` to vcpkg, or add the directory to `"overlay-ports"` in
[`vcpkg-configuration.json`](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-configuration-json).
Then list RapidProto in [`vcpkg.json`](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-json)
as a host dependency:

```json
{
  "dependencies": [
    { "name": "rapidproto", "host": true }
  ]
}
```

### Cross-compiling

The headers don't depend on the target, but the generator that writes them has to run on the build
machine. With Conan's `tool_requires` or vcpkg's `"host": true` it's built for that machine. A
fetched generator is compiled for the target, though, and in a cross build `rapidproto_generate()`
rejects it at configure time with
`cross-compiling, but rapidproto::rapidprotoc is the in-tree generator`. Install RapidProto for the
build machine instead and use `find_package`.

A toolchain that keeps `find_package` inside its sysroot (`CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY`)
won't follow `CMAKE_PREFIX_PATH` to a prefix you filled with `cmake --install`. Point
`rapidproto_DIR` at the prefix's config directory instead, as in
`-Drapidproto_DIR=/opt/rapidproto/lib/cmake/rapidproto`. In a vcpkg cross build, `find_package`
searches only the target triplet's install tree. Set `rapidproto_DIR` to the `share/rapidproto`
directory in the host triplet's tree.
