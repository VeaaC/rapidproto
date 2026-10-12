# RapidProto — Rust support (work in progress)

This directory is the in-tree Cargo workspace for RapidProto's native Rust decoders, developed on
the long-running `rust` branch. It is **not yet released** and not built by the C++ CMake project.

- `rapidproto/` — the decode-only runtime (arena, borrowed views, wire primitives, errors).
- `rapidproto-build/` — the build-time driver that generates Rust decoders (via the WASM-embedded
  C++ generator).

The public interface is still taking shape and is not yet stable. The scratch spikes at the repo root
(`rust-poc/`, `rust-m0/`) are exploratory and are **not** part of this workspace.
