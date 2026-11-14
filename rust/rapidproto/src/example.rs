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
//! Include the generated file at that path:
//!
//! ```ignore
//! pub mod example {
//!     include!(concat!(env!("OUT_DIR"), "/weather.rs"));
//! }
//! ```
//!
//! The generated code has three modules:
//!
//! - [`arena`] holds the arena decoders, such as [`arena::weather::Report`].
//! - [`stream`] holds the stream decoders, such as [`stream::weather::Report`] and its
//!   [`report_stream!`](stream::weather::report_stream) macro.
//! - [`common`] holds the enums, which both kinds of decoder use.
//!
//! # Names
//!
//! Each package becomes a module path in all three modules, so `weather` becomes `arena::weather`,
//! and a package `a.b` would become `arena::a::b`. Types declared inside a message, and the enum for
//! each of its `oneof`s, go in a module named after the message, so the `status` oneof of `Station`
//! becomes `arena::weather::station::Status`. Enum constants drop the enum's name as a prefix, so
//! `UNIT_CELSIUS` becomes `Unit::CELSIUS`. A field named after a Rust keyword or a generated method
//! is renamed, such as `type` to `r#type` and `decode` to `decode_`.
//!
//! # Accessors
//!
//! An arena message has an accessor for each field, named after the field:
//!
//! | Field | Accessor returns |
//! |---|---|
//! | Scalar | Its value, or zero when the field is absent |
//! | `string` or `bytes` | A [`ProtoStr`](crate::ProtoStr) or [`ProtoBytes`](crate::ProtoBytes), empty when absent |
//! | Enum | Its generated enum type, such as `Unit`, holding 0 when absent |
//! | Field with presence, such as `optional` | An `Option` of the above |
//! | `required` scalar, enum, `string` or `bytes` | Its value |
//! | Message, `required` or not | An `Option` of a reference, such as `Option<&Location>` |
//! | `repeated` field | A slice, such as `&[f32]` or `&[Station]` |
//! | `map` | A [`MapView`](crate::MapView) |
//! | `oneof` | An `Option` of a reference to an enum with one variant per member |
//!
//! An absent field with a `[default = ...]` in the schema returns `None`, not the default.
//!
//! # Examples
//!
//! In these examples, `input` holds an encoded `Report`. Decoding it into an arena:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x14\x0a\x06Zurich\x18\x01\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41";
//! use example::arena::weather::Report;
//! use rapidproto::Arena;
//!
//! let arena = Arena::new();
//! let report = Report::decode(input, &arena)?;
//! let station = &report.stations()[0];
//! assert_eq!(station.name(), "Zurich");
//! assert_eq!(station.temperatures(), [12.5, 13.0]);
//! assert_eq!(
//!     format!("{station:?}"),
//!     r#"Station { name: "Zurich", unit: Celsius, temperatures: [12.5, 13.0] }"#
//! );
//! # Ok(())
//! # }
//! ```
//!
//! Reading an enum, a map and a oneof:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x1d\x0a\x06Zurich\x18\x01\x2a\x0e\x0a\x04city\x12\x06Zurich\x38\x90\x1c";
//! use example::arena::weather::{station::Status, Report};
//! use example::common::weather::{Unit, UnitClass};
//! use rapidproto::Arena;
//!
//! let arena = Arena::new();
//! let station = &Report::decode(input, &arena)?.stations()[0];
//! assert_eq!(station.unit(), Unit::CELSIUS);
//! assert_eq!(station.unit().classify(), UnitClass::Celsius);
//! assert_eq!(station.labels().get("city").map(|city| city.to_str()), Some(Ok("Zurich")));
//! assert!(matches!(station.status(), Some(Status::UptimeSeconds(3600))));
//! # Ok(())
//! # }
//! ```
//!
//! Reading only the station names:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x12\x0a\x06Zurich\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41";
//! use example::stream::weather::{report_stream, station_stream, Report};
//!
//! let mut names = Vec::new();
//! Report::new(input)?.decode(report_stream! {
//!     stations => |station| station.decode(station_stream! {
//!         name => |name| names.push(name),
//!     }),
//! })?;
//! assert_eq!(names, ["Zurich"]);
//! # Ok(())
//! # }
//! ```
//!
//! Stopping at the first temperature of the whole report. The `stations` handler returns the station's
//! `decode` result, so the stop ends the report's `decode` too:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x12\x0a\x06Zurich\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41\
//! #               \x0a\x10\x0a\x04Oslo\x22\x08\x00\x00\x80\x3f\x00\x00\x00\x40";
//! use example::stream::weather::{report_stream, station_stream, Report};
//! use rapidproto::stream::Flow;
//!
//! let mut temperatures = Vec::new();
//! let flow = Report::new(input)?.decode(report_stream! {
//!     stations => |station| station.decode(station_stream! {
//!         temperatures => |t| { temperatures.push(t); Flow::Stop },
//!     }),
//! })?;
//! assert_eq!(temperatures, [12.5]);
//! assert_eq!(flow, Flow::Stop);
//! # Ok(())
//! # }
//! ```
//!
//! Reading the first temperature of every station. The `stations` handler discards the station's
//! `Flow`, so the stop ends only that station:
//!
//! ```
//! # mod example { include!(concat!(env!("CARGO_MANIFEST_DIR"), "/src/example/weather.rs")); }
//! # fn main() -> Result<(), rapidproto::DecodeError> {
//! # let input = b"\x0a\x12\x0a\x06Zurich\x22\x08\x00\x00\x48\x41\x00\x00\x50\x41\
//! #               \x0a\x10\x0a\x04Oslo\x22\x08\x00\x00\x80\x3f\x00\x00\x00\x40";
//! use example::stream::weather::{report_stream, station_stream, Report};
//! use rapidproto::stream::Flow;
//!
//! let mut temperatures = Vec::new();
//! let flow = Report::new(input)?.decode(report_stream! {
//!     stations => |station| {
//!         station.decode(station_stream! {
//!             temperatures => |t| { temperatures.push(t); Flow::Stop },
//!         })?;
//!         Ok(())
//!     },
//! })?;
//! assert_eq!(temperatures, [12.5, 1.0]);
//! assert_eq!(flow, Flow::Continue);
//! # Ok(())
//! # }
//! ```

// tests/test_rustgen.cpp checks weather.rs against weather.proto and regenerates it.
include!("example/weather.rs");
