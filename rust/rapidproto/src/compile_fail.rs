//! Code that must not compile.
//!
//! Threads cannot share an arena, since its bump pointer is not atomic:
//!
//! ```compile_fail,E0277
//! fn assert_sync<T: Sync>() {}
//! assert_sync::<rapidproto::Arena<'static>>();
//! ```
//!
//! So a thread cannot use another thread's arena through a reference:
//!
//! ```compile_fail,E0277
//! let arena = rapidproto::Arena::new();
//! std::thread::scope(|s| {
//!     s.spawn(|| arena.bytes_used());
//! });
//! ```
//!
//! An arena cannot move to another thread while something borrows it:
//!
//! ```compile_fail,E0505
//! let arena = rapidproto::Arena::new();
//! let borrowed = &arena;
//! std::thread::spawn(move || drop(arena));
//! borrowed.bytes_used();
//! ```
//!
//! An `Owned` is `Send` only when its message is:
//!
//! ```compile_fail,E0277
//! use rapidproto::{Arena, ArenaDecode, DecodeError, Owned};
//!
//! struct RcMessage(std::rc::Rc<u8>);
//! struct RcDecoder;
//! // SAFETY: a test type, never decoded.
//! unsafe impl ArenaDecode for RcDecoder {
//!     type Message<'a> = RcMessage;
//!     fn decode_borrowed<'a>(_: &'a [u8], _: &'a Arena<'a>) -> Result<&'a RcMessage, DecodeError> {
//!         unimplemented!()
//!     }
//! }
//!
//! fn assert_send<T: Send>() {}
//! assert_send::<Owned<RcDecoder>>();
//! ```
//!
//! And `Sync` only when its message is:
//!
//! ```compile_fail,E0277
//! use rapidproto::{Arena, ArenaDecode, DecodeError, Owned};
//!
//! struct CellMessage(std::cell::Cell<u8>);
//! struct CellDecoder;
//! // SAFETY: a test type, never decoded.
//! unsafe impl ArenaDecode for CellDecoder {
//!     type Message<'a> = CellMessage;
//!     fn decode_borrowed<'a>(_: &'a [u8], _: &'a Arena<'a>) -> Result<&'a CellMessage, DecodeError> {
//!         unimplemented!()
//!     }
//! }
//!
//! fn assert_sync<T: Sync>() {}
//! assert_sync::<Owned<CellDecoder>>();
//! ```
