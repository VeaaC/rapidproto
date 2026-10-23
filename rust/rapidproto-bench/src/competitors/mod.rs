//! Competitor decoders, one module per crate, added one at a time. Each module exposes a per-message-
//! type arm builder (`dataset`, `bigset`, `big`, `wideset`, `recordset`, `particleset`) returning an
//! [`Arm`](crate::Arm) that decodes and walks every field with the shared checksum. Register a new
//! competitor in the `arms!` macro in `lib.rs` (one line per competitor).

pub mod femtopb;
// The google-upb capstone arm, gated behind the off-by-default `google-upb` feature: it wraps Google's
// upb-backed `protobuf` v4 (in the separate `rapidproto-bench-upb` crate, to isolate its `::protobuf::`
// from the v3 the rust-protobuf arm uses) and needs an exact-version protoc to build.
#[cfg(feature = "google-upb")]
pub mod google_upb;
pub mod prost;
pub mod quick_protobuf;
pub mod rust_protobuf;
