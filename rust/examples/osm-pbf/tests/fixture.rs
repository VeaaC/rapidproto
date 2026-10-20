//! The osmstat fixture test, over the committed `testdata/mini.osm.pbf`:
//!
//! 1. the file decodes, and every (model x execution) combination -- arena/stream, parallel/sequential
//!    -- prints BYTE-IDENTICAL stdout: the two decode models and the two execution modes must all agree
//!    on what the file contains;
//! 2. that output equals the golden below;
//! 3. a TRUNCATED file makes osmstat exit nonzero in both models (a framing error must not pass as
//!    success).

use std::path::PathBuf;
use std::process::Command;

// Hand-derived from what testdata/make_fixture.py encodes (the generator for the committed fixture).
const GOLDEN: &str = "\
blocks           2
nodes            4 (dense 3, plain 1)
ways             1 (refs 3)
relations        1 (members 2)
tags             7 (nodes 4, ways 2, relations 1)
metadata rows    5
id sum           0000000000002ee9
bbox             lat [53.0000000, 53.0000020]  lon [8.7999999, 8.8000020]
distinct keys    3
  key amenity                  3
  key highway                  2
  key name                     2
";

fn fixture() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("testdata/mini.osm.pbf")
}

fn osmstat(args: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_osmstat"))
        .args(args)
        .output()
        .expect("run osmstat")
}

#[test]
fn golden_and_modes_agree() {
    let fix = fixture();
    let fix = fix.to_str().unwrap();

    // Every model x execution combination must succeed and print the same bytes as the golden -- so
    // the arena and streaming decoders are cross-checked against each other and against a fixed
    // expectation, under both the parallel and the single-thread path.
    for args in [
        vec![fix],
        vec!["--sequential", fix],
        vec!["--stream", fix],
        vec!["--stream", "--sequential", fix],
    ] {
        let out = osmstat(&args);
        assert!(
            out.status.success(),
            "run {args:?} failed: {}",
            String::from_utf8_lossy(&out.stderr)
        );
        let stdout = String::from_utf8(out.stdout).unwrap();
        assert_eq!(
            stdout, GOLDEN,
            "stdout for {args:?} does not match the golden"
        );
    }
}

#[test]
fn truncated_file_is_rejected() {
    let bytes = std::fs::read(fixture()).expect("read fixture");
    let mut tmp = std::env::temp_dir();
    tmp.push("osmstat_truncated.osm.pbf");
    std::fs::write(&tmp, &bytes[..bytes.len() - 20]).expect("write truncated");
    for args in [
        vec![tmp.to_str().unwrap()],
        vec!["--sequential", tmp.to_str().unwrap()],
        vec!["--stream", tmp.to_str().unwrap()],
    ] {
        let out = osmstat(&args);
        assert!(
            !out.status.success(),
            "osmstat exited 0 on a truncated file (args {args:?})"
        );
    }
    let _ = std::fs::remove_file(&tmp);
}
