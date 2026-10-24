//! `rapidproto-rust` — generate Rust protobuf decoders from `.proto` files on the command line.
//!
//! The same code generation as [`rapidproto_build::Config`] (used from a `build.rs`), exposed as a
//! standalone tool: pass entry `.proto`s, `-I` include dirs, and decode-profile options, and it
//! writes the generated Rust to a file or stdout. No `protoc` needed. Argument parsing
//! and output live here; the generation itself is [`rapidproto_build::Config::generate`].

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use rapidproto_build::Config;

struct Args {
    entries: Vec<PathBuf>,
    config: Config,
    out: Option<PathBuf>,
}

const USAGE: &str = "\
usage: rapidproto-rust [options] <entry.proto>...
  -I <dir>              add an import search path (repeatable; default: the current directory)
  -o, --out <file>      write the generated Rust here (default: stdout)
  --field-modes <file>  a decode profile (drop/raw/unknown-fields lines; repeatable)
  --drop <name>         drop a field or type (no storage, no accessor)
  --raw <name>          keep a message field's/type's payloads for deferred decodes
  --unknown <name>      reserve the unknown-fields bit on one message (repeatable)
  --unknown-present     reserve the unknown-fields bit on every message
  --stream              also emit the stream visitor model (<Msg>Visitor + <msg>_stream!)
  --export-stream-macros <prefix>
                        make the <msg>_stream! macros callable from other crates (needs --stream)
  --no-arena            omit the arena model (combine with --stream for stream-only output)
  -h, --help            show this help";

fn main() -> ExitCode {
    let args = match parse_args() {
        Ok(Some(args)) => args,
        Ok(None) => return ExitCode::SUCCESS, // --help
        Err(message) => {
            eprintln!("error: {message}");
            return ExitCode::from(2);
        }
    };
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("error: {message}");
            ExitCode::FAILURE
        }
    }
}

fn run(args: &Args) -> Result<(), String> {
    if args.entries.is_empty() {
        return Err("no entry .proto given".to_owned());
    }
    let rust = args
        .config
        .generate(&args.entries)
        .map_err(|e| e.to_string())?;
    match &args.out {
        Some(path) => {
            std::fs::write(path, rust).map_err(|e| format!("cannot write {}: {e}", path.display()))
        }
        None => {
            use std::io::Write;
            std::io::stdout()
                .write_all(rust.as_bytes())
                .map_err(|e| e.to_string())
        }
    }
}

fn parse_args() -> Result<Option<Args>, String> {
    let mut entries = Vec::new();
    let mut config = Config::new();
    let mut modes = String::new();
    let mut out = None;
    let mut argv = std::env::args().skip(1);
    while let Some(arg) = argv.next() {
        let mut take = |inline: Option<&str>, flag: &str| -> Result<String, String> {
            match inline {
                Some(v) => Ok(v.to_owned()),
                None => argv.next().ok_or_else(|| format!("{flag} needs a value")),
            }
        };
        let (key, inline) = match arg.split_once('=') {
            Some((k, v)) => (k.to_owned(), Some(v.to_owned())),
            None => (arg.clone(), None),
        };
        match key.as_str() {
            "-h" | "--help" => {
                println!("{USAGE}");
                return Ok(None);
            }
            "-I" => config = config.include(take(inline.as_deref(), "-I")?),
            "-o" | "--out" => out = Some(PathBuf::from(take(inline.as_deref(), "--out")?)),
            "--field-modes" => {
                let path = take(inline.as_deref(), "--field-modes")?;
                modes.push_str(&read_text(Path::new(&path))?);
                if !modes.ends_with('\n') {
                    modes.push('\n');
                }
            }
            "--drop" => push_mode(&mut modes, "drop", &take(inline.as_deref(), "--drop")?),
            "--raw" => push_mode(&mut modes, "raw", &take(inline.as_deref(), "--raw")?),
            "--unknown" => push_mode(
                &mut modes,
                "unknown-fields",
                &take(inline.as_deref(), "--unknown")?,
            ),
            "--unknown-present" => config = config.unknown_present(true),
            "--stream" => config = config.stream(true),
            "--export-stream-macros" => {
                config =
                    config.export_stream_macros(take(inline.as_deref(), "--export-stream-macros")?)
            }
            "--no-arena" => config = config.arena(false),
            other if other.starts_with('-') && other != "-" => {
                return Err(format!("unknown option '{other}'"));
            }
            _ => entries.push(PathBuf::from(arg)),
        }
    }
    Ok(Some(Args {
        entries,
        config: config.field_modes(modes),
        out,
    }))
}

fn read_text(path: &Path) -> Result<String, String> {
    std::fs::read_to_string(path).map_err(|e| format!("cannot read {}: {e}", path.display()))
}

fn push_mode(modes: &mut String, directive: &str, name: &str) {
    modes.push_str(directive);
    modes.push(' ');
    modes.push_str(name);
    modes.push('\n');
}
