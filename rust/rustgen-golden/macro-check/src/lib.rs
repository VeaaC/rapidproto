//! Every generated stream macro, with every field selected, expanded in a crate with no direct
//! dependency on the runtime. See `Cargo.toml`.
#![no_std]
// The macros munch one field per recursion, and the goldens have messages with many fields.
#![recursion_limit = "4096"]

pub fn call_every_macro() {
    include!(concat!(env!("OUT_DIR"), "/calls.rs"))
}
