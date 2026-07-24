# Consumer example

A runnable end-to-end consumer of the generated decoders, driven by the
[`rapidproto_generate()`](../../docs/integration.md#cmake-integration) CMake helper. Two executables,
one schema (`proto/message.proto`, which imports `proto/types.proto` so the import closure and the
depfile are exercised too):

- **`main.cpp`** — decodes the same buffer with **both models in one translation unit**
  (`GENERATOR both`): the arena tree by accessor, the streaming decoder by callback, and the
  [mid-decode hybrid](../../docs/using-both-models.md) (stream the outer message, materialize a chosen
  sub-message via `rp_bytes()`).
- **`lean_main.cpp`** — the same schema under a [decode profile](../../docs/profiles.md)
  (`lean.modes`): one field **dropped** (reading it would not compile) and one kept **raw** (the
  sub-message's payload borrowed as bytes, decoded only on demand).

## Building

In-tree (the default when the RapidProto tests are enabled), from the repository root:

```sh
cmake --preset gcc && cmake --build --preset gcc
ctest --test-dir build/gcc -R rapidproto_example    # runs both executables as tests
```

Standalone, against an **installed** RapidProto — this is the `find_package` consumer CI builds to keep
the installed packaging surface honest:

```sh
cmake --install <build-dir> --prefix <prefix>       # install RapidProto first
cmake -S examples/consumer -B example-build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build example-build && ctest --test-dir example-build
```
