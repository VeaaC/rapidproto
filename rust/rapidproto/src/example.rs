//! The code `rapidproto-build` generates for an example schema.
//!
//! This module exists only in the documentation, so your code cannot use `rapidproto::example`. It
//! shows the output for this `weather.proto`:
//!
//! ```proto
#![doc = include_str!("example/weather.proto")]
//! ```
//!
//! To generate the same code in your crate, call `rapidproto-build` from `build.rs`:
//!
//! ```ignore
//! // build.rs
//! rapidproto_build::Config::new("crate::example")
//!     .stream(true)
//!     .compile(&["weather.proto"])
//!     .expect("generate decoders");
//! ```
//!
//! then include the result at that path:
//!
//! ```ignore
//! pub mod example {
//!     include!(concat!(env!("OUT_DIR"), "/weather.rs"));
//! }
//! ```
//!
//! The generated code has three modules, each with a `weather` module for the schema's package:
//!
//! - [`arena`]: messages decoded whole into an [`Arena`](crate::Arena) and read through accessors,
//!   such as [`arena::weather::Report`].
//! - [`stream`]: decoders that pass the fields you select to a visitor without building the
//!   message, such as [`stream::weather::Report`] and its
//!   [`report_stream!`](stream::weather::report_stream) macro.
//! - [`common`]: the enums, shared by both.
//!
//! # Examples
//!
//! Decoding a report into an arena:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let bytes = b"\x0a\x12\x0a\x06Zurich\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41";
//! use example::arena::weather::Report;
//! use rapidproto::Arena;
//!
//! let arena = Arena::new();
//! let report = Report::decode(bytes, &arena)?;
//! let station = &report.stations()[0];
//! assert_eq!(station.name(), "Zurich");
//! assert_eq!(station.temperatures(), [12.5, 13.0]);
//! # Ok(())
//! # }
//! ```
//!
//! Reading only the station names:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let bytes = b"\x0a\x12\x0a\x06Zurich\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41";
//! use example::stream::weather::{report_stream, station_stream, Report};
//!
//! let mut names = Vec::new();
//! Report::new(bytes).decode(report_stream! {
//!     stations => |station| station.decode(station_stream! {
//!         name => |name| names.push(name),
//!     }),
//! })?;
//! assert_eq!(names, ["Zurich"]);
//! # Ok(())
//! # }
//! ```

// tests/test_rustgen.cpp checks weather.rs against weather.proto and regenerates it.
include!("example/weather.rs");
