//! The code `rapidproto-build` generates for an example schema.
//!
//! This module exists only in the documentation and cannot be imported. It shows the output for the
//! `weather.proto` schema below, with `Config::stream` on and the stream macros exported:
//!
//! - [`arena`]: messages decoded whole into an [`Arena`](crate::Arena) and read through accessors,
//!   such as [`arena::weather::Report`].
//! - [`stream`]: decoders that pass the fields you select to a visitor without building the message,
//!   such as [`stream::weather::Report`] and its [`report_stream!`](stream::weather::report_stream)
//!   macro.
//! - [`common`]: the enums, shared by both.
//!
//! ```proto
#![doc = include_str!("example/weather.proto")]
//! ```
//!
//! # Example
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

include!("example/weather.rs");
