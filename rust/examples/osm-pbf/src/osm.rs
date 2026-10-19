//! The generated OSM decoders (from `build.rs`). RapidProto emits several decode models; this example
//! uses `arena` -- views that borrow a bump [`Arena`](rapidproto::Arena) -- re-exported here as `OSMPBF`.

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
