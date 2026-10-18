//! Competitor decoders, one module per crate, added one at a time. Each module exposes a per-message-
//! type arm builder (`dataset`, `bigset`, `wideset`, `recordset`, `particleset`) returning an
//! [`Arm`](crate::Arm) that decodes and walks every field with the shared checksum. Register a new
//! competitor in the `arms!` macro in `lib.rs` (one line per competitor).

pub mod prost;
pub mod rust_protobuf;
