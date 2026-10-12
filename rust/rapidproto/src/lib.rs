//! RapidProto — decode-only Protobuf runtime for generated Rust decoders.
//!
//! **Work in progress** (the `rust` feature branch). This crate will provide the arena, the
//! borrowed view types (`ProtoStr`, array/map views), the wire-decode primitives, and the
//! `DecodeError` that generated code builds on. It is `std` by default, `alloc`-capable, and
//! `no_std`-no-alloc-capable via a caller-seeded arena.
//!
//! This is an early skeleton; the data types and decode logic are still being built out.
#![cfg_attr(not(feature = "std"), no_std)]
