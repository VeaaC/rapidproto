//! The generated OSM decoders, pulled in from `build.rs`'s output. RapidProto emits several decode
//! models; this example uses the `arena` one -- messages decode into a bump [`Arena`](rapidproto::Arena)
//! and the generated types are lightweight views that borrow it, so a decode copies nothing and the
//! scalar/packed fields are read straight out of the arena. We re-export that module as `OSMPBF`.

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
