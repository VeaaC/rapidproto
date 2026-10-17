//! libFuzzer harness: arena `decode()` over arbitrary (untrusted) bytes, into a fresh `Arena`,
//! through FIVE generated decoders whose arms differ most -- the Rust mirror of the C++
//! `tests/fuzz/fuzz_arena.cpp`:
//!   * the rich proto3 message (strings, repeated, maps, oneofs, recursion + the depth guard),
//!   * the wire-feature surface -- proto2 groups, and the packed/fixed message,
//!   * the >64-`required` message (the multi-word transient required mask),
//!   * the `--unknown-present` variant (the per-message unknown bit, incl. bool-wrapper inlining),
//!   * the field-modes profile (raw payload capture incl. groups + dropped-field skips, plus the
//!     deferred decode of a captured raw payload).
//!
//! On a successful decode a few accessors exercise the read side; `black_box` keeps them from being
//! optimized away (the C++ harness's `volatile sink`). A fresh `Arena` per arm, like the C++.
//!
//! Sanitizer: cargo-fuzz builds this under AddressSanitizer, which catches any out-of-bounds read,
//! use-after-free, or leak. Rust has no direct equivalent of C's UBSan, but the decoder's UB
//! surface is covered here by ASan plus debug overflow/bounds checks, and by the separate Miri job
//! (strict provenance + Stacked/Tree Borrows over the unsafe arena). Together they are the
//! never-crash / no-UB gate the arena decoder needs against hostile input.
#![no_main]

use std::hint::black_box;

use libfuzzer_sys::fuzz_target;
use rapidproto::Arena;

use rustgen_golden::arena_manyreq::arena::mr::ManyRequired;
use rustgen_golden::arena_modes_lean::arena::fm::{Blob, Holder as ModedHolder};
use rustgen_golden::arena_unknown_tracked::arena::au::Holder as UnkHolder;
use rustgen_golden::packed::arena::pk::Packed;
use rustgen_golden::proto2::arena::p2::WithGroup;
use rustgen_golden::proto3::arena::p3::Msg;

fuzz_target!(|data: &[u8]| {
    // 1. Rich proto3: presence, repeated, maps, oneofs, recursion + the depth guard.
    {
        let arena = Arena::new();
        if let Ok(m) = Msg::decode(data, &arena) {
            black_box(m.name().as_bytes().len());
            let mut sum = 0i64;
            for v in m.nums().iter().copied() {
                sum = sum.wrapping_add(i64::from(v));
            }
            black_box(sum);
            if let Some(inner) = m.self_() {
                black_box(inner.name().as_bytes().len());
            }
        }
    }
    // 2. Wire-feature surface: proto2 groups, then the packed / fixed-width message.
    {
        let arena = Arena::new();
        if let Ok(g) = WithGroup::decode(data, &arena) {
            if let Some(grp) = g.mygroup() {
                black_box(grp.a());
            }
        }
    }
    {
        let arena = Arena::new();
        if let Ok(p) = Packed::decode(data, &arena) {
            black_box(p.pd().len());
            black_box(p.psf().len());
            black_box(p.pf().len());
        }
    }
    // 3. >64 `required`: the multi-word transient required mask.
    {
        let arena = Arena::new();
        if let Ok(r) = ManyRequired::decode(data, &arena) {
            black_box(r.f1());
        }
    }
    // 4. `--unknown-present`: the per-message unknown-fields bit.
    {
        let arena = Arena::new();
        if let Ok(h) = UnkHolder::decode(data, &arena) {
            black_box(h.has_unknown_fields());
        }
    }
    // 5. Field-modes: raw payload capture (required / repeated / group), dropped-field skips, and
    //    the deferred decode -- a captured raw payload must feed the field type's decoder without
    //    UB on any input.
    {
        let arena = Arena::new();
        if let Ok(h) = ModedHolder::decode(data, &arena) {
            black_box(h.req_blob().map_or(0, <[u8]>::len));
            black_box(h.blobs().len());
            if let Some(grp) = h.grp() {
                black_box(grp.len());
            }
            if let Some(payload) = h.blob() {
                if !payload.is_empty() {
                    let scratch = Arena::new();
                    if let Ok(blob) = Blob::decode(payload, &scratch) {
                        black_box(blob.payload().is_some());
                    }
                }
            }
        }
    }
});
