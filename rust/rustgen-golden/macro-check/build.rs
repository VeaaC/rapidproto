// Write two calls of every `*_stream!` macro in the goldens, one per form, each selecting every field,
// as one block.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::{env, fs};

fn main() {
    let goldens = Path::new(env!("CARGO_MANIFEST_DIR")).join("../goldens");
    println!("cargo::rerun-if-changed={}", goldens.display());
    let mut files: Vec<PathBuf> = fs::read_dir(&goldens)
        .expect("read goldens")
        .map(|entry| entry.expect("golden entry").path())
        .filter(|path| path.extension().is_some_and(|e| e == "rs"))
        .collect();
    files.sort();

    let (mut macros, mut called) = (0, 0);
    let mut calls = String::from("{\nlet mut state = ();\n");
    for file in files {
        let text = fs::read_to_string(&file).expect("read golden");
        let mut path = "";
        let mut keys = Vec::new();
        for line in text.lines().map(str::trim_start) {
            if line.starts_with("macro_rules! ") {
                macros += 1;
            } else if let Some(rest) =
                line.strip_prefix("($($f:ident => $c:expr),* $(,)?) => { $crate")
            {
                path = rest.split_once('!').expect("a macro call").0;
            } else if let Some(rest) = line.strip_prefix("(@f [$($a:tt)*] ") {
                keys.push(rest.split_once(" =>").expect("a field row").0);
            } else if line.starts_with("(@f $a:tt) =>") {
                let plain = keys.iter().map(|key| format!("{key} => |_| ()"));
                let shared = keys.iter().map(|key| format!("{key} => |_, _| ()"));
                let plain = plain.collect::<Vec<_>>().join(", ");
                let shared = shared.collect::<Vec<_>>().join(", ");
                let path = format!("::rustgen_golden{path}");
                writeln!(calls, "let _ = {path}! {{ {plain} }};").unwrap();
                writeln!(calls, "let _ = {path}!(&mut state; {{ {shared} }});").unwrap();
                keys.clear();
                called += 1;
            }
        }
    }
    calls.push_str("}\n");
    // If the macros' shape changes, this must fail, not silently stop checking anything.
    assert!(
        called > 0 && called == macros,
        "found {macros} macros but built {called} calls"
    );
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));
    fs::write(out.join("calls.rs"), calls).expect("write calls");
}
