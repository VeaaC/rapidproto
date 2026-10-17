//! RapidProto's Rust code generator, driven from Rust with no C++ toolchain.
//!
//! The generator is rapidproto's C++ front-end compiled to a `wasm32-wasi` blob (see
//! `wasm/build-wasm.sh`); this crate embeds that blob and runs it through the [`wasmi`] interpreter.
//! [`generate`] hands the blob a source closure + a decode profile and returns the generated Rust,
//! so a build (our own golden regeneration, a consumer's `build.rs`) needs only cargo.
//!
//! The blob is a WASI command module: it reads a length-prefixed frame on stdin and writes the Rust
//! to stdout (see [`frame`] for the wire format, which mirrors `src/rustgen/wasm_entry.cpp`). Errors
//! come back on stderr with a nonzero exit code, surfaced here as [`GenError`].

use std::collections::btree_map::Entry;
use std::collections::BTreeMap;
use std::io::Cursor;
use std::path::{Component, Path, PathBuf};
use std::sync::OnceLock;

use wasmi::{Engine, Linker, Module, Store};
use wasmi_wasi::sync::WasiCtxBuilder;
use wasmi_wasi::wasi_common::pipe::{ReadPipe, WritePipe};
use wasmi_wasi::WasiCtx;

/// The embedded generator blob. Regenerated from the C++ source by `wasm/build-wasm.sh`; a CI gate
/// byte-diffs it against a fresh build so it can never drift from the generator.
const RUSTGEN_WASM: &[u8] = include_bytes!("../wasm/rustgen.wasm");

/// One source file handed to the generator: its import-closure name (the key an `import` resolves
/// to, e.g. `"types.proto"`) and its text.
pub struct SourceFile {
    pub name: String,
    pub body: String,
}

/// The inputs for one generation: the entry files (closure roots, named as any importer would name
/// them), every source the closure might reach (a superset is fine — the generator pulls exactly
/// what it needs), and the decode profile.
pub struct Request {
    pub entries: Vec<String>,
    pub sources: Vec<SourceFile>,
    /// A `.modes` profile (`drop`/`raw`/`unknown-fields` lines), or empty for none.
    pub modes: String,
    /// Reserve the "unknown fields present" bit on every message (`--unknown-present`).
    pub unknown_all: bool,
}

/// A generation that the blob rejected: its exit code and the message it wrote to stderr.
#[derive(Debug)]
pub struct GenError {
    pub code: i32,
    pub message: String,
}

impl std::fmt::Display for GenError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "generator failed (exit {}): {}", self.code, self.message)
    }
}

impl std::error::Error for GenError {}

/// The stdin frame the blob reads. Mirror of the reader in `src/rustgen/wasm_entry.cpp`: a `RPWF1`
/// magic, then little-endian-u32-counted entry names, u32-counted `{name, body}` sources, the
/// `.modes` text, and a one-byte unknown-all flag. Bodies are length-prefixed, so a proto's
/// newlines/quotes need no escaping.
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
        buf.extend_from_slice(b"RPWF1\n");
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
        Ok(buf)
    }
}

/// Run the embedded generator over `request`, returning the generated Rust (one module tree for the
/// whole closure) or the blob's error. The blob is a pure function of its input: same frame, same
/// bytes out — and byte-identical to native `rapidprotoc --rust`.
pub fn generate(request: &Request) -> Result<String, GenError> {
    let frame = frame::encode(request)?;

    // Validating/translating the ~1 MB blob is the expensive step, and it never changes, so compile
    // it once and reuse it: a caller that generates many files (our golden gate does 49) pays it
    // once, not per file. Only the lightweight Store/instance is per-call.
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

/// The embedded blob, compiled once and reused across [`generate`] calls. The blob is validated at
/// build time (it is our own asset, byte-checked by the staleness gate), so a compile failure here
/// is a broken build, not a runtime condition -- hence the panic.
fn compiled() -> &'static (Engine, Module) {
    static CELL: OnceLock<(Engine, Module)> = OnceLock::new();
    CELL.get_or_init(|| {
        let engine = Engine::default();
        let module = Module::new(&engine, RUSTGEN_WASM)
            .expect("the embedded rustgen blob is a valid module");
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

/// Generate from files on disk, the way the CLI (and our golden regeneration) drives it: hand the
/// generator every `.proto` under the `include` dirs (keyed by its import-relative path) plus each
/// entry under the name an importer would use, then [`generate`]. The import closure is resolved
/// inside the generator; over-inclusion here is harmless. An I/O failure is reported as a -1
/// [`GenError`] (there is no generator exit code for it).
pub fn generate_from_disk(
    entries: &[PathBuf],
    includes: &[PathBuf],
    modes: &str,
    unknown_all: bool,
) -> Result<String, GenError> {
    // Canonicalize the include dirs once, so the source walk and the entry-key derivation compare in
    // the same absolute/normalized/symlink-resolved terms native `canonical_entry_name` uses. Without
    // this a `./`-prefixed or relative-vs-absolute entry path would key differently from how an
    // importer names the same file, splitting the closure into two entries.
    let includes: Vec<PathBuf> = includes
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
        modes: modes.to_owned(),
        unknown_all,
    })
}

/// The name an `import` would use for `entry`: its path relative to the first include dir that
/// contains it, else its file name. `entry` and `includes` must already be canonicalized (see
/// [`generate_from_disk`]) so the prefix comparison matches native's canonical entry key.
pub fn import_relative_name(entry: &Path, includes: &[PathBuf]) -> String {
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
            // First include dir wins (match native/protoc), so skip a path an earlier -I already
            // provided -- and skip reading it, too.
            // Read (and keep) only if an earlier -I has not already provided this key.
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
