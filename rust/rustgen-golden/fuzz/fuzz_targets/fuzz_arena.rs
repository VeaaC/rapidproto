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
//! On a successful decode the message's `Debug` output is written to a counting sink. That reads every
//! field through its accessor, recursively, so the whole read side runs on the hostile input too;
//! `black_box` keeps it from being optimized away (the C++ harness's `volatile sink`). A fresh
//! `Arena` per arm, like the C++.
//!
//! Sanitizer: cargo-fuzz builds this under AddressSanitizer, which catches any out-of-bounds read,
//! use-after-free, or leak. Rust has no direct equivalent of C's UBSan, but the decoder's UB
//! surface is covered here by ASan, Rust's always-on slice-bounds checks and the fuzz profile's
//! integer-overflow checks, and by the separate Miri job (strict provenance + Stacked/Tree Borrows
//! over the unsafe arena). Together they are the never-crash / no-UB gate the arena decoder needs
//! against hostile input.
#![no_main]

use std::fmt::{self, Write};
use std::hint::black_box;

use libfuzzer_sys::fuzz_target;
use rapidproto::Arena;

use rustgen_golden::arena_manyreq::arena::mr::ManyRequired;
use rustgen_golden::arena_modes_lean::arena::fm::{Blob, Holder as ModedHolder};
use rustgen_golden::arena_unknown_tracked::arena::au::Holder as UnkHolder;
use rustgen_golden::packed::arena::pk::Packed;
use rustgen_golden::proto2::arena::p2::WithGroup;
use rustgen_golden::proto3::arena::p3::Msg;

// Counts the bytes `Debug` writes, so printing runs in full without allocating.
struct Count(usize);

impl Write for Count {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.0 += s.len();
        Ok(())
    }
}

fn print(message: &dyn fmt::Debug) {
    let mut count = Count(0);
    write!(count, "{message:?}").expect("Debug fails only when the sink does");
    black_box(count.0);
}

fuzz_target!(|data: &[u8]| {
    // 1. Rich proto3: presence, repeated, maps, oneofs, recursion + the depth guard.
    {
        let arena = Arena::new();
        if let Ok(m) = Msg::decode(data, &arena) {
            print(m);
        }
    }
    // 2. Wire-feature surface: proto2 groups, then the packed / fixed-width message.
    {
        let arena = Arena::new();
        if let Ok(g) = WithGroup::decode(data, &arena) {
            print(g);
        }
    }
    {
        let arena = Arena::new();
        if let Ok(p) = Packed::decode(data, &arena) {
            print(p);
        }
    }
    // 3. >64 `required`: the multi-word transient required mask.
    {
        let arena = Arena::new();
        if let Ok(r) = ManyRequired::decode(data, &arena) {
            print(r);
        }
    }
    // 4. `--unknown-present`: the per-message unknown-fields bit, which `Debug` does not print.
    {
        let arena = Arena::new();
        if let Ok(h) = UnkHolder::decode(data, &arena) {
            print(h);
            black_box(h.has_unknown_fields());
        }
    }
    // 5. Field-modes: raw payload capture (required / repeated / group), dropped-field skips, and
    //    the deferred decode -- a captured raw payload must feed the field type's decoder without
    //    UB on any input.
    {
        let arena = Arena::new();
        if let Ok(h) = ModedHolder::decode(data, &arena) {
            print(h);
            if let Some(payload) = h.blob() {
                if !payload.is_empty() {
                    let scratch = Arena::new();
                    if let Ok(blob) = Blob::decode(&payload, &scratch) {
                        print(blob);
                    }
                }
            }
        }
    }
});
