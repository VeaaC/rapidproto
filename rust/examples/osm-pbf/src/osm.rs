//! The generated OSM decoders. `build.rs` renders both vendored schemas into one file (named after
//! the first entry, `fileformat.rs`); it carries every `OSMPBF` type. We re-export the `arena` variant
//! (borrowed views over an arena) that this showcase uses.

#[allow(
    clippy::all,
    unused,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod generated {
    include!(concat!(env!("OUT_DIR"), "/fileformat.rs"));
}

pub use generated::arena::OSMPBF;
