//! The generated OSM decoders (from `build.rs`). The one generated file holds both decode models:
//! `arena::OSMPBF` (borrowed views over a bump [`Arena`](rapidproto::Arena), used by [`walk_arena`](crate::walk_arena))
//! and `stream::OSMPBF` (the streaming visitor model, used by [`walk_stream`](crate::walk_stream)).
//! The same workload is run through both -- the point of the example.

// `pub(crate)`, not private: the stream macros reach the generated code through this path, and
// `walk_stream` calls them from outside this module.
pub(crate) mod generated {
    include!(concat!(env!("OUT_DIR"), "/fileformat.rs"));
}

/// The arena model: `decode()` into an `Arena` and read borrowed views.
pub use generated::arena::OSMPBF as arena;
/// The streaming model: `Msg::new(bytes)?.decode(visitor)`, one callback per field, nothing
/// materialized. The `*_stream!` macros and `*Visitor` traits live here too.
pub use generated::stream::OSMPBF as stream;
