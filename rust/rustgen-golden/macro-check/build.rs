// Write one call of every `*_stream!` macro in the goldens, each selecting every field, as one block.

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
    let mut calls = String::from("{\n");
    for file in files {
        let text = fs::read_to_string(&file).expect("read golden");
        let mut path = "";
        let mut fields = Vec::new();
        for line in text.lines().map(str::trim_start) {
            if line.starts_with("macro_rules! ") {
                macros += 1;
            } else if let Some(rest) =
                line.strip_prefix("($($f:ident => $c:expr),* $(,)?) => { $crate")
            {
                path = rest.split_once('!').expect("a macro call").0;
            } else if let Some(rest) = line.strip_prefix("(@f [$($a:tt)*] ") {
                let key = rest.split_once(" =>").expect("a field row").0;
                fields.push(format!("{key} => |_| ()"));
            } else if line.starts_with("(@f $a:tt) =>") {
                let selected = fields.join(", ");
                writeln!(calls, "let _ = ::rustgen_golden{path}! {{ {selected} }};").unwrap();
                fields.clear();
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
