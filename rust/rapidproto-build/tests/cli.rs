//! The `rapidproto-rust` command line.

use std::path::PathBuf;
use std::process::{Command, Output};

fn run(args: &[&str]) -> Output {
    let proto = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../examples/consumer/proto");
    Command::new(env!("CARGO_BIN_EXE_rapidproto-rust"))
        .arg("-I")
        .arg(&proto)
        .args(args)
        .arg(proto.join("message.proto"))
        .output()
        .expect("run rapidproto-rust")
}

#[test]
fn the_module_path_is_required() {
    let output = run(&["--stream"]);
    assert_eq!(output.status.code(), Some(2));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("--module-path"), "{stderr}");
}

#[test]
fn the_module_path_reaches_the_output() {
    for args in [
        ["--module-path", "crate::schema"].as_slice(),
        ["--module-path=crate::schema"].as_slice(),
    ] {
        let output = run(args);
        assert!(output.status.success(), "{output:?}");
        let rust = String::from_utf8_lossy(&output.stdout);
        assert!(rust.contains("pub(in crate::schema::common) const _"));
        assert!(!rust.contains("pub mod stream"), "stream is opt-in");
    }
}

#[test]
fn stream_and_arena_flags_select_the_models() {
    let output = run(&["--module-path", "crate", "--stream", "--no-arena"]);
    assert!(output.status.success(), "{output:?}");
    let rust = String::from_utf8_lossy(&output.stdout);
    assert!(rust.contains("pub mod stream"));
    assert!(!rust.contains("pub mod arena"));
}

#[test]
fn a_bad_module_path_is_reported() {
    let output = run(&["--module-path", "schema"]);
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("is not a path of ASCII identifiers"),
        "{stderr}"
    );
}
