//! The generated OSM decoders (from `build.rs`). The one generated file holds both decode models:
//! `arena::OSMPBF` (borrowed views over a bump [`Arena`](rapidproto::Arena), used by [`walk`](crate::walk))
//! and `stream::OSMPBF` (the streaming visitor model, used by [`walk_stream`](crate::walk_stream)).
//! The same workload is run through both -- the point of the example.

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

/// The arena model: `decode()` into an `Arena` and read borrowed views.
pub use generated::arena::OSMPBF as arena;
/// The streaming model: `Msg::new(bytes).decode(visitor)`, one callback per field, nothing
/// materialized. The `*_stream!` macros and `*Visitor` traits live here too.
pub use generated::stream::OSMPBF as stream;
