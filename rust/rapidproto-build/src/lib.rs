//! Runs RapidProto's generator in `build.rs` to write Rust decoders for your Protobuf schemas.
//!
//! The generator parses the schemas itself, without protoc, whether they use proto2, proto3, or
//! editions 2023 or 2024. They can import `google/protobuf/timestamp.proto` and the other
//! well-known types with no copy of the files on disk.
//!
//! Your crate needs the [`rapidproto`] runtime as a dependency and this crate as a build
//! dependency. Write both requirements with `=` and the same version number. Mixed versions usually
//! fail to compile, starting with the error
//! `use the same version of rapidproto and rapidproto-build`. Both crates are test releases, so
//! upgrading can break your build script or the code that reads the messages.
//!
//! Here is a schema in `proto/message.proto` and the build script that generates its decoders:
//!
//! ```proto
//! syntax = "proto3";
//! package shop;
//!
//! message Order {
//!   string id = 1;
//!   repeated int64 quantities = 2;
//! }
//! ```
//!
//! ```no_run
//! // build.rs
//! fn main() -> Result<(), rapidproto_build::GenError> {
//!     rapidproto_build::Config::new("crate::schema")
//!         .include("proto")
//!         .compile(&["proto/message.proto"])?;
//!     Ok(())
//! }
//! ```
//!
//! [`Config::compile`] names its file after the first schema you pass it, here `message.rs` in
//! `OUT_DIR`. Put the `include!` in the module whose path you gave [`Config::new`], here
//! `crate::schema`:
//!
//! ```ignore
//! // src/main.rs
//! mod schema {
//!     include!(concat!(env!("OUT_DIR"), "/message.rs"));
//! }
//!
//! use schema::arena::shop::Order;
//!
//! fn main() -> Result<(), rapidproto::DecodeError> {
//!     let input = b"\x0a\x03B12";
//!     let arena = rapidproto::Arena::new();
//!     let order = Order::decode(input, &arena)?;
//!     assert_eq!(order.id(), "B12");
//!     Ok(())
//! }
//! ```
//!
//! By default `Config` generates only [arena decoders], and for `package shop` they land in
//! `schema::arena::shop`. To see what a schema's messages, enums and fields turn into, browse the
//! runtime's [`example`] module, which also explains [how names are formed][names].
//!
//! Some other methods change the output:
//!
//! - [`Config::stream`] adds streaming decoders.
//! - [`Config::field_modes`] applies a decode profile.
//! - [`Config::serde`] adds `serde::Serialize` impls.
//!
//! [`Config::generate`] hands you the code as a `String` and writes nothing.
//!
//! To keep the generated code under version control and review its diffs like any other source,
//! generate it with the `rapidproto-rust` command instead of a build script.
//! `cargo install rapidproto-build --version 0.1.0-alpha.2` installs it. Its flags set the same
//! options as `Config`'s methods, and `rapidproto-rust --help` lists them.
//!
//! # How the generator runs
//!
//! RapidProto's generator is implemented in C++. The native generator is that C++ code compiled by
//! this crate's build script, with your C++17 compiler. The wasm generator is the same code
//! compiled ahead of time to WebAssembly, shipped in this crate and run in the [`wasmi`]
//! interpreter. Both produce the same generated code.
//!
//! The `native` and `wasm` features choose which of them your build has, and the default has both.
//! `Config` then uses the native generator if it built and the wasm generator if it didn't. When it
//! falls back, the first `compile` call prints a warning that begins
//! `rapidproto-build uses the slower wasm generator` and gives the reason. To build one generator
//! only, set `default-features = false` and pick its feature:
//!
//! - `native` leaves out the interpreter and its dependencies. Without a C++ compiler there's then
//!   nothing to fall back to, and your crate doesn't build.
//! - `wasm` skips the C++ build, and with it the warning. Pick it when your machines have no C++
//!   compiler, as on a slim CI image.
//!
//! Cargo [compiles build scripts and their dependencies without optimization][build-override], in
//! release builds too, which leaves the wasm generator slow. It can take minutes on a large schema,
//! and [the crate's README][times] has times for a few sizes. To optimize build-time code, add this
//! to your workspace root's `Cargo.toml`:
//!
//! ```toml
//! [profile.dev.build-override]
//! opt-level = 3
//!
//! [profile.release.build-override]
//! opt-level = 3
//! ```
//!
//! A workspace member's profiles are ignored, with a warning, and a dependency can't set them for
//! you. It applies to every proc macro and build script in the build and all their dependencies, so
//! a clean build gets slower. That's worth it only when the wasm generator does the work, and more
//! so the larger your schemas are. The native generator is compiled optimized in every profile and
//! gains nothing from it.
//!
//! # Recursion limit
//!
//! rustc gives up instantiating generic code once it nests deeper than its recursion limit, which
//! defaults to 128. Generated code can reach it through a chain of message types in which each
//! holds the next as a field. Each message in the chain costs one level, and each `repeated` field
//! two. So a chain longer than about 128 messages, or about 64 joined by `repeated` fields, stops
//! the build, and rustc reports `reached the recursion limit while instantiating`. Few schemas have
//! chains that long. If yours does, set a higher limit at the top of the failing crate's root file,
//! such as `lib.rs` or `main.rs`:
//!
//! ```
//! #![recursion_limit = "256"]
//! ```
//!
//! With [`Config::serde`], serializing the messages can hit the limit as well, in whichever crate
//! calls the serializer. When the messages come from another crate, the error points into that
//! crate's generated file, but the attribute belongs in the one doing the serializing.
//!
//! [`rapidproto`]: https://docs.rs/rapidproto/latest/rapidproto/
//! [arena decoders]: https://docs.rs/rapidproto/latest/rapidproto/#arena-and-streaming-decoders
//! [`example`]: https://docs.rs/rapidproto/latest/rapidproto/example/index.html
//! [names]: https://docs.rs/rapidproto/latest/rapidproto/example/index.html#names
//! [`wasmi`]: https://docs.rs/wasmi/latest/wasmi/
//! [build-override]: https://doc.rust-lang.org/cargo/reference/profiles.html#build-dependencies
//! [times]: https://github.com/VeaaC/rapidproto/blob/main/rust/rapidproto-build/README.md#native-and-wasm-generators
#![warn(missing_docs)]

use std::collections::btree_map::Entry;
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::path::{Component, Path, PathBuf};
use std::sync::{Mutex, PoisonError};

/// Why this build uses the wasm generator although the `native` feature is on: the reason the
/// native one did not build. `None` when it built, or was not asked for.
#[doc(hidden)]
pub const NATIVE_FALLBACK: Option<&str> = option_env!("RAPIDPROTO_BUILD_FALLBACK");

/// One in-memory source file: `name` is how an `import` refers to it (e.g. `"types.proto"`), and
/// `body` is its text.
#[doc(hidden)]
pub struct SourceFile {
    /// How an `import` statement spells this file (e.g. `"types.proto"`).
    pub name: String,
    /// The file's `.proto` text.
    pub body: String,
}

/// The inputs for one generation: the entry files (the roots of the import closure, each named the
/// way an `import` would spell it), the sources those entries and their imports may pull in (a
/// superset is fine — only what the closure reaches is used), and the generation options.
#[doc(hidden)]
pub struct Request {
    /// The entry `.proto`s, named as an `import` would spell them.
    pub entries: Vec<String>,
    /// The entries and every file they may import (a superset is fine).
    pub sources: Vec<SourceFile>,
    /// A decode profile — the text of a `.modes` file (`drop` / `raw` / `unknown-fields` lines) — or
    /// empty for the default (decode every field).
    pub modes: String,
    /// Reserve space to track unknown fields on every message (see [`Config::unknown_present`]).
    pub unknown_present: bool,
    /// Emit the random-access `arena` decoder model (default on; see [`Config::arena`]).
    pub arena: bool,
    /// Emit the `stream` decoder model (default off; see [`Config::stream`]).
    pub stream: bool,
    /// Implement `serde::Serialize` (default off; see [`Config::serde`]).
    pub serde: bool,
    /// Where the generated file is included (see [`Config::new`]).
    pub module_path: String,
}

impl Default for Request {
    /// No entries or sources, no decode profile, the `arena` model only, included at the crate root.
    fn default() -> Self {
        Request {
            entries: Vec::new(),
            sources: Vec::new(),
            modes: String::new(),
            unknown_present: false,
            arena: true,
            stream: false,
            serde: false,
            module_path: "crate".to_owned(),
        }
    }
}

/// An error from the generator.
#[derive(Debug)]
pub struct GenError {
    /// The generator's status: 2 for a malformed request, 3 for a schema that does not resolve, 4 for
    /// a bad decode profile; negative for an I/O or internal failure. The tests and the CLI read it.
    #[doc(hidden)]
    pub code: i32,
    /// A description of the failure.
    pub message: String,
}

impl std::fmt::Display for GenError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "generator failed (status {}): {}",
            self.code, self.message
        )
    }
}

impl std::error::Error for GenError {}

/// Encodes a [`Request`] into the byte frame the generator takes, through either backend: a `RPWF5`
/// magic, then little-endian-u32-counted entry names, u32-counted `{name, body}` sources, the
/// `.modes` text, a one-byte unknown-present flag, two one-byte model flags (arena, stream), a one-byte
/// serde flag, and the module path. Bodies are length-prefixed, so a proto's newlines/quotes need no
/// escaping. The generator validates the options; the decode side is `rapidproto/rustgen/frame.hpp`
/// in the repository.
mod frame {
    use super::{GenError, Request};

    /// Write a u32 little-endian count/length, rejecting (never truncating) a value past u32::MAX --
    /// so a pathologically large source can't silently wrap its length prefix and corrupt the frame.
    fn put_len(buf: &mut Vec<u8>, n: usize) -> Result<(), GenError> {
        let len = u32::try_from(n).map_err(|_| GenError {
            code: -1,
            message: format!("frame field too large: {n} bytes (max {})", u32::MAX),
        })?;
        buf.extend_from_slice(&len.to_le_bytes());
        Ok(())
    }

    fn put_bytes(buf: &mut Vec<u8>, bytes: &[u8]) -> Result<(), GenError> {
        put_len(buf, bytes.len())?;
        buf.extend_from_slice(bytes);
        Ok(())
    }

    pub(super) fn encode(request: &Request) -> Result<Vec<u8>, GenError> {
        let mut buf = Vec::new();
        buf.extend_from_slice(b"RPWF5\n");
        put_len(&mut buf, request.entries.len())?;
        for entry in &request.entries {
            put_bytes(&mut buf, entry.as_bytes())?;
        }
        put_len(&mut buf, request.sources.len())?;
        for source in &request.sources {
            put_bytes(&mut buf, source.name.as_bytes())?;
            put_bytes(&mut buf, source.body.as_bytes())?;
        }
        put_bytes(&mut buf, request.modes.as_bytes())?;
        buf.push(u8::from(request.unknown_present));
        buf.push(u8::from(request.arena));
        buf.push(u8::from(request.stream));
        buf.push(u8::from(request.serde));
        put_bytes(&mut buf, request.module_path.as_bytes())?;
        Ok(buf)
    }
}

/// A way to run the generator (see [How the generator runs](crate#how-the-generator-runs)).
#[doc(hidden)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Backend {
    #[cfg(rp_native)]
    Native,
    #[cfg(feature = "wasm")]
    Wasm,
}

/// The backends this build has, the one [`Config`] uses first.
#[doc(hidden)]
pub const BACKENDS: &[Backend] = &[
    #[cfg(rp_native)]
    Backend::Native,
    #[cfg(feature = "wasm")]
    Backend::Wasm,
];

/// Run the generator on `request` through a given backend. Public for checking that the backends agree.
#[doc(hidden)]
pub fn generate_with(backend: Backend, request: &Request) -> Result<String, GenError> {
    let frame = frame::encode(request)?;
    let (status, text) = match backend {
        #[cfg(rp_native)]
        Backend::Native => native::run(&frame)?,
        #[cfg(feature = "wasm")]
        Backend::Wasm => wasm::run(frame)?,
    };
    if status == 0 {
        Ok(String::from_utf8_lossy(&text).into_owned())
    } else {
        Err(GenError {
            code: status,
            message: String::from_utf8_lossy(&text).trim_end().to_owned(),
        })
    }
}

/// The generator compiled by the build script, called in-process through its C ABI
/// (src/rustgen/native_entry.cpp in the repository).
#[cfg(rp_native)]
mod native {
    use std::ffi::{c_char, c_int, c_void};

    use super::{internal, GenError};

    type Sink = extern "C" fn(context: *mut c_void, data: *const c_char, len: usize);

    extern "C" {
        fn rapidproto_rustgen_run(
            frame: *const c_char,
            frame_len: usize,
            sink: Sink,
            context: *mut c_void,
        ) -> c_int;
    }

    // The stack the generator gets, as much as the wasm module links with: room for the parser's and
    // resolver's depth-capped recursion on a deep schema, whatever thread the caller is on.
    const STACK_BYTES: usize = 8 << 20;

    /// The generator's status and its output (the generated Rust, or the error message).
    pub(super) fn run(frame: &[u8]) -> Result<(i32, Vec<u8>), GenError> {
        std::thread::scope(|scope| {
            std::thread::Builder::new()
                .stack_size(STACK_BYTES)
                .spawn_scoped(scope, || call(frame))
                .map_err(internal)?
                .join()
                .map_err(|_| internal("the native generator panicked"))
        })
    }

    fn call(frame: &[u8]) -> (i32, Vec<u8>) {
        // Copies the output the generator hands over; `context` is the `Vec` below.
        extern "C" fn sink(context: *mut c_void, data: *const c_char, len: usize) {
            // SAFETY: `context` is the `&mut Vec<u8>` passed to the call, alive and unaliased for its
            // duration; `data` is valid for `len` bytes during this callback.
            unsafe {
                let out = &mut *context.cast::<Vec<u8>>();
                out.extend_from_slice(std::slice::from_raw_parts(data.cast::<u8>(), len));
            }
        }
        let mut text = Vec::new();
        // SAFETY: `frame` is valid for `frame.len()` bytes, and `sink` and `context` fit each other;
        // the generator calls `sink` once, before returning, and lets no exception escape.
        let status = unsafe {
            rapidproto_rustgen_run(
                frame.as_ptr().cast(),
                frame.len(),
                sink,
                std::ptr::from_mut(&mut text).cast(),
            )
        };
        if status == -1 {
            let detail = String::from_utf8_lossy(&text);
            text = format!("internal error in the native generator: {detail}").into_bytes();
        }
        (status, text)
    }
}

/// The embedded wasm generator in the wasmi interpreter: the frame as stdin, the output on stdout or
/// stderr, the status as the exit code.
#[cfg(feature = "wasm")]
mod wasm {
    use std::io::Cursor;
    use std::sync::OnceLock;

    use wasmi::{Engine, Linker, Module, Store};
    use wasmi_wasi::sync::WasiCtxBuilder;
    use wasmi_wasi::wasi_common::pipe::{ReadPipe, WritePipe};
    use wasmi_wasi::WasiCtx;

    use super::{internal, GenError};

    /// The embedded generator. Rebuilt by `wasm/build-wasm.sh` and byte-checked in CI, so it stays
    /// in sync with the generator source.
    const RUSTGEN_WASM: &[u8] = include_bytes!("../wasm/rustgen.wasm");

    /// The generator's status and its output (the generated Rust, or the error message).
    pub(super) fn run(frame: Vec<u8>) -> Result<(i32, Vec<u8>), GenError> {
        // Validating the embedded generator is the one expensive step, and it never changes, so
        // compile it once and reuse it: a caller that generates many files pays that cost once, not
        // per file. Only the lightweight Store/instance is per-call.
        let (engine, module) = compiled();

        // In-memory stdio: feed the frame as stdin, capture stdout/stderr. The pipes are cloneable
        // handles over a shared buffer; the WasiCtx holds one clone, we keep one to read afterwards.
        let stdout = WritePipe::new_in_memory();
        let stderr = WritePipe::new_in_memory();
        let wasi = WasiCtxBuilder::new()
            .stdin(Box::new(ReadPipe::from(frame)))
            .stdout(Box::new(stdout.clone()))
            .stderr(Box::new(stderr.clone()))
            .build();

        let mut store = Store::new(engine, wasi);
        let mut linker: Linker<WasiCtx> = Linker::new(engine);
        wasmi_wasi::sync::add_to_linker(&mut linker, |ctx| ctx).map_err(internal)?;

        let instance = linker
            .instantiate_and_start(&mut store, module)
            .map_err(internal)?;
        let start = instance
            .get_typed_func::<(), ()>(&store, "_start")
            .map_err(internal)?;
        let run = start.call(&mut store, ());

        // Drop the store so its clone of the pipe buffers is released; then we are the sole owner
        // and can take the captured bytes out.
        drop(store);
        let exit = match run {
            Ok(()) => 0,
            // A WASI `proc_exit(n)` surfaces as this; anything else is a real trap, not a clean exit.
            Err(error) => match error.i32_exit_status() {
                Some(code) => code,
                None => return Err(internal(error)),
            },
        };
        Ok((exit, take(if exit == 0 { stdout } else { stderr })))
    }

    /// The embedded generator, compiled once and reused across calls. It is a fixed, build-time
    /// asset, so a compile failure here means a broken build, not a runtime condition -- hence the
    /// panic.
    fn compiled() -> &'static (Engine, Module) {
        static CELL: OnceLock<(Engine, Module)> = OnceLock::new();
        CELL.get_or_init(|| {
            let engine = Engine::default();
            let module = Module::new(&engine, RUSTGEN_WASM)
                .expect("the embedded generator is a valid module");
            (engine, module)
        })
    }

    /// Take the bytes a finished in-memory pipe captured (sole owner by now).
    fn take(pipe: WritePipe<Cursor<Vec<u8>>>) -> Vec<u8> {
        pipe.try_into_inner()
            .expect("pipe still shared after the store was dropped")
            .into_inner()
    }
}

/// A failure inside the runtime (not a clean generator error): no exit code, so report it as -1.
fn internal(error: impl std::fmt::Display) -> GenError {
    GenError {
        code: -1,
        message: error.to_string(),
    }
}

/// Options for generating decoders.
///
/// The [crate docs](crate) show how to use it in `build.rs`.
#[derive(Debug)]
pub struct Config {
    module_path: String,
    includes: Vec<PathBuf>,
    modes: String,
    unknown_present: bool,
    arena: bool,
    stream: bool,
    serde: bool,
    backend: Backend,
}

impl Config {
    /// Creates a config whose output you include at `module_path`.
    ///
    /// A new config generates only arena decoders and applies no
    /// [decode profile](Config::field_modes). Until you add directories with
    /// [`include`](Config::include), an import can name any `.proto` file under the current
    /// directory, by its path relative to it. A build script runs in the package root.
    ///
    /// The module path names the module that will hold the `include!`, written from the crate root:
    /// `crate` for the root itself, or a path such as `crate::schema`. Each segment after `crate`
    /// is an ASCII identifier, with keywords written raw, as in `crate::r#type`. `new` accepts any
    /// string; a malformed one makes [`generate`](Config::generate) and
    /// [`compile`](Config::compile) return a [`GenError`].
    ///
    /// The generated code refers to its own items by absolute paths that start with the module
    /// path, so it compiles only in the module that path names. Included anywhere else, it fails
    /// with E0433 errors, reported in the generated file, saying a module on that path can't be
    /// found. The module can't be inside a function body, whose items have
    /// no `crate::` path; in a doctest, that means declaring `fn main` yourself. Include each file
    /// once per crate. A package's library, binaries and `tests/` files are separate crates,
    /// though, and each can include it at the same path.
    ///
    /// Two generated files need two module paths, and two `Config`s with them, as in the example
    /// below. A schema that both files import is then generated into each, as separate types. To
    /// share them, pass both schemas to one `compile`, which writes a single file.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// // build.rs
    /// use rapidproto_build::{Config, GenError};
    ///
    /// fn main() -> Result<(), GenError> {
    ///     Config::new("crate::proto::orders")
    ///         .include("proto")
    ///         .compile(&["proto/orders.proto"])?;
    ///     Config::new("crate::proto::users")
    ///         .include("proto")
    ///         .stream(true)
    ///         .compile(&["proto/users.proto"])?;
    ///     Ok(())
    /// }
    /// ```
    ///
    /// ```ignore
    /// // src/lib.rs
    /// pub mod proto {
    ///     pub mod orders {
    ///         include!(concat!(env!("OUT_DIR"), "/orders.rs"));
    ///     }
    ///     pub mod users {
    ///         include!(concat!(env!("OUT_DIR"), "/users.rs"));
    ///     }
    /// }
    /// ```
    pub fn new(module_path: impl Into<String>) -> Self {
        Config {
            module_path: module_path.into(),
            includes: Vec::new(),
            modes: String::new(),
            unknown_present: false,
            arena: true,
            stream: false,
            serde: false,
            backend: BACKENDS[0],
        }
    }

    /// Adds a directory to search for `.proto` files.
    ///
    /// Once you add one, the current directory is no longer searched. If several directories have the
    /// same file, the first one added wins.
    pub fn include(mut self, dir: impl Into<PathBuf>) -> Self {
        self.includes.push(dir.into());
        self
    }

    /// Sets a decode profile, which changes what the arena decoders store.
    ///
    /// `modes` holds one directive per line, and `#` starts a comment:
    ///
    /// - `drop pkg.Msg.field` removes the field and its accessor.
    /// - `raw pkg.Msg.field` keeps a message or group field as its encoded message. The accessor
    ///   returns it as [`ProtoBytes`], in an `Option` for a singular field and in a slice for a
    ///   `repeated` one, ready for the field type's `decode`.
    /// - `unknown-fields pkg.Msg` adds `has_unknown_fields()` to the message.
    ///
    /// `drop` and `raw` also take a message type, `pkg.Msg`, to cover every field of that type, and
    /// `drop` takes an enum type the same way. A directive for a field wins over one for its type.
    /// Neither applies to a `oneof` member, `raw` does not apply to a `map` field, and `drop` does
    /// not apply to a `required` field. Naming such a field fails, and a directive for its type
    /// leaves it as it is. Streaming decoders ignore the profile.
    ///
    /// ```no_run
    /// // build.rs
    /// rapidproto_build::Config::new("crate::schema")
    ///     .field_modes("drop shop.Order.notes\nraw shop.Order.items")
    ///     .compile(&["shop.proto"])
    ///     .expect("generate decoders");
    /// ```
    ///
    /// [`ProtoBytes`]: https://docs.rs/rapidproto/latest/rapidproto/struct.ProtoBytes.html
    pub fn field_modes(mut self, modes: impl Into<String>) -> Self {
        self.modes = modes.into();
        self
    }

    /// Adds `has_unknown_fields()` to every arena message.
    ///
    /// It returns `true` if the encoded message had a field the schema does not declare. Unknown
    /// fields inside a sub-message do not count.
    pub fn unknown_present(mut self, yes: bool) -> Self {
        self.unknown_present = yes;
        self
    }

    /// Generates arena decoders, which decode the whole message into an [`Arena`].
    ///
    /// They are on by default. At least one of `arena` and [`stream`](Config::stream) must be on.
    ///
    /// [`Arena`]: https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html
    pub fn arena(mut self, yes: bool) -> Self {
        self.arena = yes;
        self
    }

    /// Generates streaming decoders, which pass the fields you select to your handlers.
    ///
    /// They are off by default. The [`rapidproto::stream`] docs show how to use them.
    ///
    /// Each message gets a `msg_stream!` macro, which builds a visitor from your handlers. The macro
    /// expands where you call it and reaches the rest of the generated code through the path given to
    /// [`Config::new`], so each module on that path must be visible from the call. Make the
    /// modules `pub` to call it from `tests/` or another crate. To call it from elsewhere in the
    /// crate, a module nested below the crate root needs `pub(crate)`.
    ///
    /// A `proc-macro` crate cannot export macros, so streaming decoders do not compile there. Put
    /// the generated code in a separate crate instead.
    ///
    /// [`rapidproto::stream`]: https://docs.rs/rapidproto/latest/rapidproto/stream/index.html
    pub fn stream(mut self, yes: bool) -> Self {
        self.stream = yes;
        self
    }

    /// Implements `serde::Serialize` for the arena messages and the types generated for the schema's
    /// enums.
    ///
    /// It is off by default. When it is on, the generated code needs the `serde` feature of
    /// `rapidproto`, and does not compile without it. The [`rapidproto` docs] describe the output.
    ///
    /// To make this optional in your own crate, declare a `serde` feature that enables
    /// `rapidproto/serde`, and read it in `build.rs`:
    ///
    /// ```no_run
    /// // build.rs
    /// rapidproto_build::Config::new("crate::schema")
    ///     .serde(std::env::var_os("CARGO_FEATURE_SERDE").is_some())
    ///     .compile(&["message.proto"])
    ///     .expect("generate decoders");
    /// ```
    ///
    /// [`rapidproto` docs]: https://docs.rs/rapidproto/latest/rapidproto/#serde
    pub fn serde(mut self, yes: bool) -> Self {
        self.serde = yes;
        self
    }

    /// Generate through `backend` instead of the default one -- for checking that the backends agree.
    #[doc(hidden)]
    pub fn backend(mut self, backend: Backend) -> Self {
        self.backend = backend;
        self
    }

    /// Generates decoders for `entries` and everything they import, and returns the code.
    ///
    /// Imports resolve against every `.proto` file under the include directories. An entry is named by
    /// its path relative to the first include directory that holds it, or by its file name if none
    /// does. [`Config::compile`] also writes the file and tells cargo when to re-run the build script.
    pub fn generate(&self, entries: &[impl AsRef<Path>]) -> Result<String, GenError> {
        // Canonicalize the include dirs once, so the source walk and the entry-key derivation compare
        // paths in the same absolute/normalized/symlink-resolved terms. Without this a `./`-prefixed
        // or relative-vs-absolute entry path would key differently from how an import names the same
        // file, splitting the closure into two entries.
        let includes: Vec<PathBuf> = self
            .include_dirs()
            .iter()
            .map(|dir| {
                dir.canonicalize()
                    .map_err(|e| internal(format!("include dir {}: {e}", dir.display())))
            })
            .collect::<Result<_, _>>()?;

        let mut sources: BTreeMap<String, String> = BTreeMap::new();
        for include in &includes {
            collect_protos(include, include, &mut sources).map_err(internal)?;
        }
        let mut entry_names = Vec::with_capacity(entries.len());
        for entry in entries {
            let entry = entry.as_ref();
            let canonical = entry
                .canonicalize()
                .map_err(|e| internal(format!("entry {}: {e}", entry.display())))?;
            let name = import_relative_name(&canonical, &includes);
            let body = read(&canonical).map_err(internal)?;
            sources.insert(name.clone(), body);
            entry_names.push(name);
        }
        let request = Request {
            entries: entry_names,
            sources: sources
                .into_iter()
                .map(|(name, body)| SourceFile { name, body })
                .collect(),
            modes: self.modes.clone(),
            unknown_present: self.unknown_present,
            arena: self.arena,
            stream: self.stream,
            serde: self.serde,
            module_path: self.module_path.clone(),
        };
        generate_with(self.backend, &request)
    }

    /// Generates decoders for `entries` and everything they import, writes them to `OUT_DIR`, and
    /// returns the file's path.
    ///
    /// The file is named after the first entry, so `proto/message.proto` gives `message.rs`. A second
    /// call whose first entry has the same name, such as `other/message.proto`, fails. Use
    /// [`Config::generate`] to write such files under names of your own.
    ///
    /// Cargo re-runs the build script when an entry or a `.proto` file under the include
    /// directories changes. `compile` arranges this with `cargo:rerun-if-changed` lines, which stop
    /// Cargo from watching the rest of the package; if the build script reads another file, such as
    /// a [decode profile](Config::field_modes), print a `cargo:rerun-if-changed` line for it too.
    pub fn compile(&self, entries: &[impl AsRef<Path>]) -> Result<PathBuf, GenError> {
        // Warned from here, the consumer's own build script, because cargo hides the warnings of a
        // dependency's build script.
        if let Some(reason) = NATIVE_FALLBACK {
            static WARNED: std::sync::Once = std::sync::Once::new();
            WARNED.call_once(|| {
                println!(
                    "cargo:warning=rapidproto-build uses the slower wasm generator: the native one \
                     did not build, {reason}. To use the wasm one on purpose, depend on \
                     rapidproto-build with `default-features = false, features = [\"wasm\"]`."
                );
            });
        }
        let out_dir = env::var_os("OUT_DIR").ok_or_else(|| GenError {
            code: -1,
            message: "compile(): OUT_DIR is unset -- call it from a cargo build script".to_owned(),
        })?;
        // One file covers the whole closure; name it after the first entry's stem, so several
        // compile() calls in one build script land in distinct files.
        let stem = entries
            .first()
            .and_then(|e| e.as_ref().file_stem())
            .map_or_else(|| "schema".to_owned(), |s| s.to_string_lossy().into_owned());
        let out_path = PathBuf::from(out_dir).join(format!("{stem}.rs"));
        // A build script runs once per process, so this remembers the files of this build only.
        // Checked before generating, so a clash fails fast.
        static WRITTEN: Mutex<BTreeSet<PathBuf>> = Mutex::new(BTreeSet::new());
        let first = WRITTEN
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .insert(out_path.clone());
        if !first {
            return Err(GenError {
                code: -1,
                message: format!(
                    "compile(): an earlier call in this build script also writes {stem}.rs, \
                     since each call names its file after its first entry. Use generate() and \
                     write the files under names of your own"
                ),
            });
        }
        let code = self.generate(entries)?;
        std::fs::write(&out_path, code).map_err(|e| GenError {
            code: -1,
            message: format!("compile(): cannot write {}: {e}", out_path.display()),
        })?;

        // Rebuild tracking: the entries themselves, plus every proto on the include path. Watching the
        // whole include path (not just the resolved closure, which the generator does not report) is
        // deliberately broad: a build never goes stale, at the cost of an occasional rebuild when an
        // unrelated proto on the path changes.
        for entry in entries {
            println!("cargo:rerun-if-changed={}", entry.as_ref().display());
        }
        for dir in &self.include_dirs() {
            track_protos(dir);
        }
        Ok(out_path)
    }

    /// The include path: the added directories, or the cwd (the crate root in a build script) when
    /// none was added.
    fn include_dirs(&self) -> Vec<PathBuf> {
        if self.includes.is_empty() {
            vec![PathBuf::from(".")]
        } else {
            self.includes.clone()
        }
    }
}

/// Emit `cargo:rerun-if-changed` for every `.proto` under `dir` (recursively). An unreadable dir is
/// skipped silently -- it just means nothing there is tracked, and a real missing include surfaces as
/// a generation error, not here.
fn track_protos(dir: &Path) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            track_protos(&path);
        } else if path.extension().is_some_and(|e| e == "proto") {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }
}

/// The name an `import` would use for `entry`: its path relative to the first `includes` directory
/// that contains it, otherwise its file name. `entry` and `includes` must already be canonicalized
/// (see [`Config::generate`]) so the prefix comparison is done in matching terms.
fn import_relative_name(entry: &Path, includes: &[PathBuf]) -> String {
    for include in includes {
        if let Ok(rel) = entry.strip_prefix(include) {
            return to_slash(rel);
        }
    }
    entry
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

/// Recursively add every `.proto` under `dir`, keyed by its path relative to `root` (the include
/// dir), in forward-slash form so the key matches an `import` string on any platform. Only `dir`
/// itself must be readable: below it, a directory or file that cannot be read (or is not UTF-8) is
/// skipped -- over-inclusion is harmless, and an import that needed it still fails as a missing
/// import. So pointing the include path at a large tree (the CLI's cwd default) cannot fail on an
/// unrelated file.
fn collect_protos(
    root: &Path,
    dir: &Path,
    out: &mut BTreeMap<String, String>,
) -> Result<(), String> {
    let entries = std::fs::read_dir(dir)
        .map_err(|e| format!("cannot read include dir {}: {e}", dir.display()))?;
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(file_type) = entry.file_type() else {
            continue;
        };
        if file_type.is_dir() {
            let _ = collect_protos(root, &path, out); // an unreadable subdirectory is skipped
        } else if path.extension().is_some_and(|e| e == "proto") {
            let rel = path.strip_prefix(root).unwrap_or(&path);
            // First include dir wins (like protoc): read and keep this file only if an earlier
            // include dir has not already provided the same key.
            if let Entry::Vacant(slot) = out.entry(to_slash(rel)) {
                if let Ok(body) = std::fs::read_to_string(&path) {
                    slot.insert(body);
                }
            }
        }
    }
    Ok(())
}

fn read(path: &Path) -> Result<String, String> {
    std::fs::read_to_string(path).map_err(|e| format!("cannot read {}: {e}", path.display()))
}

/// Forward-slash rendering of a relative path (import keys are `/`-separated regardless of OS).
fn to_slash(path: &Path) -> String {
    path.components()
        .filter_map(|c| match c {
            Component::Normal(s) => Some(s.to_string_lossy()),
            _ => None,
        })
        .collect::<Vec<_>>()
        .join("/")
}
