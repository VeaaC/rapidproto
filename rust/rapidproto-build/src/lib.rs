//! Generate Rust protobuf **decoders** from `.proto` files, using only cargo — no `protoc` and no
//! separate code generator to install.
//!
//! Most consumers call [`Config`] from a `build.rs` to generate at build time:
//!
//! ```no_run
//! // build.rs
//! rapidproto_build::Config::new()
//!     .include("proto")
//!     .compile(&["proto/message.proto"])
//!     .expect("generate decoders");
//! ```
//!
//! then include the generated code (one file per import closure) and decode with it:
//!
//! ```ignore
//! mod schema {
//!     include!(concat!(env!("OUT_DIR"), "/message.rs"));
//! }
//! ```
//!
//! The generated decoders use the `rapidproto` runtime crate. To get the generated code as a string
//! instead of a file, use [`Config::generate`]; to generate from sources already in memory, use
//! [`generate`].
//!
//! The same generator is also available as a command-line tool, `rapidproto-rust` (install this crate
//! with `cargo install` and run `rapidproto-rust --help`), for generating decoders outside a build
//! script.

use std::collections::btree_map::Entry;
use std::collections::BTreeMap;
use std::env;
use std::io::Cursor;
use std::path::{Component, Path, PathBuf};
use std::sync::OnceLock;

use wasmi::{Engine, Linker, Module, Store};
use wasmi_wasi::sync::WasiCtxBuilder;
use wasmi_wasi::wasi_common::pipe::{ReadPipe, WritePipe};
use wasmi_wasi::WasiCtx;

/// The embedded generator. Rebuilt by `wasm/build-wasm.sh` and byte-checked in CI, so it stays in
/// sync with the generator source.
const RUSTGEN_WASM: &[u8] = include_bytes!("../wasm/rustgen.wasm");

/// One in-memory source file: `name` is how an `import` refers to it (e.g. `"types.proto"`), and
/// `body` is its text.
pub struct SourceFile {
    /// How an `import` statement spells this file (e.g. `"types.proto"`).
    pub name: String,
    /// The file's `.proto` text.
    pub body: String,
}

/// The inputs for one generation: the entry files (the roots of the import closure, each named the
/// way an `import` would spell it), the sources those entries and their imports may pull in (a
/// superset is fine — only what the closure reaches is used), and the generation options. Build one
/// with `..Request::default()` for the options left at their defaults.
pub struct Request {
    /// The entry `.proto`s, named as an `import` would spell them.
    pub entries: Vec<String>,
    /// The entries and every file they may import (a superset is fine).
    pub sources: Vec<SourceFile>,
    /// A decode profile — the text of a `.modes` file (`drop` / `raw` / `unknown-fields` lines) — or
    /// empty for the default (decode every field).
    pub modes: String,
    /// Reserve space to track unknown fields on every message (see [`Config::unknown_present`]).
    pub unknown_all: bool,
    /// Emit the random-access `arena` decoder model (default on; see [`Config::arena`]).
    pub arena: bool,
    /// Emit the `stream` visitor model (default off; see [`Config::stream`]).
    pub stream: bool,
    /// Make the `<msg>_stream!` macros callable from other crates, under this prefix (default
    /// `None`; needs `stream`; see [`Config::export_stream_macros`]).
    pub export_stream_macros: Option<String>,
}

impl Default for Request {
    /// No entries or sources, no decode profile, the `arena` model only — the same options as
    /// [`Config::new`].
    fn default() -> Self {
        Request {
            entries: Vec::new(),
            sources: Vec::new(),
            modes: String::new(),
            unknown_all: false,
            arena: true,
            stream: false,
            export_stream_macros: None,
        }
    }
}

/// An error from the generator.
#[derive(Debug)]
pub struct GenError {
    /// The generator's exit status (negative for an I/O or internal failure).
    pub code: i32,
    /// A human-readable description of the failure.
    pub message: String,
}

impl std::fmt::Display for GenError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "generator failed (exit {}): {}", self.code, self.message)
    }
}

impl std::error::Error for GenError {}

/// Encodes a [`Request`] into the byte frame the generator reads on stdin: a `RPWF3` magic, then
/// little-endian-u32-counted entry names, u32-counted `{name, body}` sources, the `.modes` text, a
/// one-byte unknown-all flag, two one-byte model flags (arena, stream), and the stream-macro export
/// prefix (empty = not exported). Bodies are length-prefixed, so a proto's newlines/quotes need no
/// escaping. The generator validates the options. (Internal: the decode side lives in the generator;
/// the frame and the blob move together, byte-checked in CI.)
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
        buf.extend_from_slice(b"RPWF3\n");
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
        buf.push(u8::from(request.unknown_all));
        buf.push(u8::from(request.arena));
        buf.push(u8::from(request.stream));
        put_bytes(
            &mut buf,
            request
                .export_stream_macros
                .as_deref()
                .unwrap_or("")
                .as_bytes(),
        )?;
        Ok(buf)
    }
}

/// Generate decoders from sources already in memory (no filesystem access), returning the generated
/// Rust — one module tree covering the whole import closure — or a [`GenError`]. Generation is a
/// pure function of the request: the same inputs always produce the same output. See [`Config`] to
/// generate from `.proto` files on disk instead.
pub fn generate(request: &Request) -> Result<String, GenError> {
    let frame = frame::encode(request)?;

    // Validating the embedded generator is the one expensive step, and it never changes, so compile
    // it once and reuse it: a caller that generates many files pays that cost once, not per file.
    // Only the lightweight Store/instance is per-call.
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

    // Drop the store so its clone of the pipe buffers is released; then we are the sole owner and
    // can take the captured bytes out.
    drop(store);
    let exit = match run {
        Ok(()) => 0,
        // A WASI `proc_exit(n)` surfaces as this; anything else is a real trap, not a clean exit.
        Err(error) => match error.i32_exit_status() {
            Some(code) => code,
            None => return Err(internal(error)),
        },
    };

    if exit == 0 {
        Ok(String::from_utf8_lossy(&take(stdout)).into_owned())
    } else {
        Err(GenError {
            code: exit,
            message: String::from_utf8_lossy(&take(stderr)).trim_end().to_owned(),
        })
    }
}

/// The embedded generator, compiled once and reused across [`generate`] calls. It is a fixed,
/// build-time asset, so a compile failure here means a broken build, not a runtime condition --
/// hence the panic.
fn compiled() -> &'static (Engine, Module) {
    static CELL: OnceLock<(Engine, Module)> = OnceLock::new();
    CELL.get_or_init(|| {
        let engine = Engine::default();
        let module =
            Module::new(&engine, RUSTGEN_WASM).expect("the embedded generator is a valid module");
        (engine, module)
    })
}

/// Take the bytes a finished in-memory pipe captured (sole owner by now).
fn take(pipe: WritePipe<Cursor<Vec<u8>>>) -> Vec<u8> {
    pipe.try_into_inner()
        .expect("pipe still shared after the store was dropped")
        .into_inner()
}

/// A failure inside the runtime (not a clean generator error): no exit code, so report it as -1.
fn internal(error: impl std::fmt::Display) -> GenError {
    GenError {
        code: -1,
        message: error.to_string(),
    }
}

/// Generate decoders from a `build.rs`: run the generator over a schema's import closure, write the
/// result into `OUT_DIR`, and register the inputs so cargo re-runs when a `.proto` changes.
///
/// ```no_run
/// // build.rs
/// rapidproto_build::Config::new()
///     .include("proto")
///     .compile(&["proto/message.proto"])
///     .expect("generate decoders");
/// ```
/// then include the generated file (one per import closure) in your crate:
/// ```ignore
/// mod schema {
///     include!(concat!(env!("OUT_DIR"), "/message.rs"));
/// }
/// ```
pub struct Config {
    includes: Vec<PathBuf>,
    modes: String,
    unknown_all: bool,
    arena: bool,
    stream: bool,
    export_stream_macros: Option<String>,
}

impl Default for Config {
    fn default() -> Self {
        // arena on, stream off: the default output is the random-access decoder, unchanged for every
        // existing consumer.
        Config {
            includes: Vec::new(),
            modes: String::new(),
            unknown_all: false,
            arena: true,
            stream: false,
            export_stream_macros: None,
        }
    }
}

impl Config {
    /// A fresh config: no include dirs, no decode profile, the `arena` model only.
    pub fn new() -> Self {
        Self::default()
    }

    /// Add a directory to search for `.proto` files (repeatable; if a file is found under more than
    /// one, the first added wins).
    pub fn include(mut self, dir: impl Into<PathBuf>) -> Self {
        self.includes.push(dir.into());
        self
    }

    /// Set a decode profile: the text of a `.modes` file (`drop` / `raw` / `unknown-fields` lines).
    pub fn field_modes(mut self, modes: impl Into<String>) -> Self {
        self.modes = modes.into();
        self
    }

    /// Reserve space to track unknown fields on every message.
    pub fn unknown_present(mut self, yes: bool) -> Self {
        self.unknown_all = yes;
        self
    }

    /// Emit the random-access `arena` decoder model (the generated `arena::` module). On by default;
    /// pass `false` to omit it (e.g. for a stream-only consumer). At least one model must stay enabled.
    pub fn arena(mut self, yes: bool) -> Self {
        self.arena = yes;
        self
    }

    /// Emit the `stream` visitor model (the generated `stream::` module: a `<Msg>Visitor` trait, a
    /// `<Msg>::decode` driver, and a `<msg>_stream!` builder macro) for decoding a few fields without
    /// materializing the message. Off by default. The macros are usable within the crate that
    /// `include!`s the file (glob-import the message's stream module); to call them from its `tests/`
    /// or from another crate, see [`Config::export_stream_macros`].
    pub fn stream(mut self, yes: bool) -> Self {
        self.stream = yes;
        self
    }

    /// Make each `<msg>_stream!` macro callable from outside the crate that `include!`s the generated
    /// file: from that crate's `tests/`, or from crates that depend on it. Needs [`Config::stream`].
    /// Off by default. (The generated types and `<Msg>Visitor` traits are public either way, so a
    /// hand-written visitor works from any crate.)
    ///
    /// Each macro is then also defined at the including crate's root, under a hidden name built from
    /// `prefix` (an identifier: ASCII letters, digits and `_`, not starting with a digit). Give each
    /// generated file you export from in one crate its own prefix.
    pub fn export_stream_macros(mut self, prefix: impl Into<String>) -> Self {
        self.export_stream_macros = Some(prefix.into());
        self
    }

    /// Generate decoders for `entries` (one import closure) and return the generated Rust (one module
    /// tree for the whole closure), writing nothing. Every `.proto` under the include directories (the
    /// current directory when none was added) is made available to the generator, and each entry is
    /// named by its path relative to the include directory that holds it. [`Config::compile`] is this
    /// plus writing the file into `OUT_DIR` and registering the inputs with cargo.
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
        generate(&Request {
            entries: entry_names,
            sources: sources
                .into_iter()
                .map(|(name, body)| SourceFile { name, body })
                .collect(),
            modes: self.modes.clone(),
            unknown_all: self.unknown_all,
            arena: self.arena,
            stream: self.stream,
            export_stream_macros: self.export_stream_macros.clone(),
        })
    }

    /// Generate decoders for `entries` (one import closure) into `OUT_DIR` and return the path to the
    /// generated file to `include!`. Registers the entries and every `.proto` on the include path
    /// with cargo, so editing a schema re-runs the build. Call this from a `build.rs` (it reads the
    /// `OUT_DIR` cargo sets).
    pub fn compile(&self, entries: &[impl AsRef<Path>]) -> Result<PathBuf, GenError> {
        let code = self.generate(entries)?;

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
/// dir), in forward-slash form so the key matches an `import` string on any platform.
fn collect_protos(
    root: &Path,
    dir: &Path,
    out: &mut BTreeMap<String, String>,
) -> Result<(), String> {
    let entries = std::fs::read_dir(dir)
        .map_err(|e| format!("cannot read include dir {}: {e}", dir.display()))?;
    for entry in entries {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        let file_type = entry.file_type().map_err(|e| e.to_string())?;
        if file_type.is_dir() {
            collect_protos(root, &path, out)?;
        } else if path.extension().is_some_and(|e| e == "proto") {
            let rel = path.strip_prefix(root).unwrap_or(&path);
            // First include dir wins (like protoc): read and keep this file only if an earlier
            // include dir has not already provided the same key.
            if let Entry::Vacant(slot) = out.entry(to_slash(rel)) {
                slot.insert(read(&path)?);
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
