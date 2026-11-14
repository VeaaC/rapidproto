//! The `rapidproto-rust` command-line tool, which generates Rust decoders from `.proto` files.
//!
//! It runs the same generator as [`rapidproto_build::Config`] and writes the code to a file or to
//! standard output. `rapidproto-rust --help` lists its options.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use rapidproto_build::Config;

struct Args {
    entries: Vec<PathBuf>,
    config: Config,
    out: Option<PathBuf>,
}

const USAGE: &str = "\
usage: rapidproto-rust --module-path <path> [options] <entry.proto>...
  --module-path <path>  where the output is included: crate, or a path from it like crate::schema
  -I <dir>              add an import search path (repeatable; default: the current directory)
  -o, --out <file>      write the generated Rust here (default: stdout)
  --field-modes <file>  apply a decode profile of drop, raw and unknown-fields lines (repeatable)
  --drop <name>         remove a field and its accessor, or every field of type <name> (repeatable)
  --raw <name>          keep a message field encoded, or every field of type <name> (repeatable)
  --unknown <message>   add has_unknown_fields() to one message (repeatable)
  --unknown-present     add has_unknown_fields() to every message
  --stream              also generate stream decoders
  --no-arena            do not generate arena decoders (use with --stream)
  --serde               implement serde::Serialize (needs rapidproto's serde feature)
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
    if let Some(reason) = rapidproto_build::NATIVE_FALLBACK {
        eprintln!(
            "note: this rapidproto-rust uses the slower wasm generator: the native one did not build, \
             {reason}"
        );
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
    let mut module_path = None;
    let mut includes = Vec::new();
    let mut modes = String::new();
    let (mut unknown_present, mut stream, mut arena, mut serde) = (false, false, true, false);
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
            "--module-path" => module_path = Some(take(inline.as_deref(), "--module-path")?),
            "-I" => includes.push(take(inline.as_deref(), "-I")?),
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
            "--unknown-present" => unknown_present = true,
            "--stream" => stream = true,
            "--no-arena" => arena = false,
            "--serde" => serde = true,
            other if other.starts_with('-') && other != "-" => {
                return Err(format!("unknown option '{other}'"));
            }
            _ => entries.push(PathBuf::from(arg)),
        }
    }
    let mut config = Config::new(module_path.ok_or("no --module-path given")?)
        .field_modes(modes)
        .unknown_present(unknown_present)
        .stream(stream)
        .arena(arena)
        .serde(serde);
    for dir in includes {
        config = config.include(dir);
    }
    Ok(Some(Args {
        entries,
        config,
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
