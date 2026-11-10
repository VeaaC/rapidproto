//! `Config::compile` as a build script calls it. One test only: it sets `OUT_DIR` for this process.

use std::fs;
use std::path::PathBuf;

use rapidproto_build::Config;

#[test]
fn two_entries_with_one_file_name_fail_instead_of_overwriting() {
    let root = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("compile-names");
    let _ = fs::remove_dir_all(&root);
    for dir in ["a", "b", "out"] {
        fs::create_dir_all(root.join(dir)).unwrap();
    }
    fs::write(
        root.join("a/x.proto"),
        "syntax = \"proto3\"; package a; message A {}",
    )
    .unwrap();
    fs::write(
        root.join("b/x.proto"),
        "syntax = \"proto3\"; package b; message B {}",
    )
    .unwrap();
    std::env::set_var("OUT_DIR", root.join("out"));

    let first = Config::new("crate::a")
        .include(root.join("a"))
        .compile(&[root.join("a/x.proto")])
        .unwrap();
    assert_eq!(first, root.join("out/x.rs"));
    let error = Config::new("crate::b")
        .include(root.join("b"))
        .compile(&[root.join("b/x.proto")])
        .unwrap_err();
    assert!(
        error.message.contains("also writes x.rs"),
        "{}",
        error.message
    );
    // The first file is still there.
    assert!(fs::read_to_string(&first)
        .unwrap()
        .contains("pub struct A<'a>"));
}
