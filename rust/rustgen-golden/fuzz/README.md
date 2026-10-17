# Fuzzing the arena decoder

A [`cargo-fuzz`](https://rust-fuzz.github.io/book/cargo-fuzz.html) harness that drives arbitrary
(untrusted) bytes through the generated arena `decode()` paths under AddressSanitizer — the
never-crash / no-UB gate for hostile input. It mirrors the C++ `tests/fuzz/fuzz_arena.cpp`: one
target, `fuzz_arena`, decodes each input through five generated decoders whose arms differ most
(rich proto3, the wire-feature surface, `>64`-required, `--unknown-present`, and the field-modes
profile with a deferred raw-payload decode).

The decoders under test are the committed goldens in `../goldens/` (included by `rustgen-golden`),
so this fuzzes exactly the code a consumer compiles.

## Run

```sh
cargo install cargo-fuzz          # once; needs a nightly toolchain
cd rust/rustgen-golden
cargo +nightly fuzz build fuzz_arena                                       # compile under ASan
cargo +nightly fuzz run fuzz_arena fuzz/corpus/fuzz_arena fuzz/seeds/fuzz_arena  # fuzz
```

The first dir is libFuzzer's growing corpus (writable, gitignored); the committed `seeds/` dir is
read-only input. New finds accumulate in `corpus/`, so repeat runs get faster.

Add `-- -max_total_time=300` (or `-runs=N`) to bound a run. A crash writes the offending input to
`fuzz/artifacts/fuzz_arena/`; replay it with `cargo +nightly fuzz run fuzz_arena <path>`.

CI runs `fuzz build` as an always-on compile gate plus a short seeded smoke; a deeper run is a
local / manual step, as with the C++ deep tier.

## Seeds

`seeds/fuzz_arena/` holds the binary wire fixtures reused from `tests/wire_fixtures` — real
protobuf wire encodings whose valid tag/length framing random bytes rarely produce, so they
bootstrap coverage of the decoder arms faster. The growing corpus (`corpus/`), `artifacts/`, and
`target/` are gitignored.
