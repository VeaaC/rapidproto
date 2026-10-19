//! The osmstat fixture test, mirroring the C++ example's `check_fixture.py` (minus the
//! regenerate-from-generator check, which the C++ CTest owns -- the fixture is single-sourced):
//!
//! 1. the committed `mini.osm.pbf` decodes, and the PARALLEL and SEQUENTIAL paths print
//!    BYTE-IDENTICAL stdout (the two execution modes must agree on what the file contains);
//! 2. that output equals the golden -- the SAME golden the C++ tool is held to, so the Rust and C++
//!    implementations are cross-validated against each other;
//! 3. a TRUNCATED file makes osmstat exit nonzero (a framing error must not pass as success).
//!
//! The fixture is the one the C++ example vendors; this test reads it by relative path, so there is a
//! single committed artifact (validated against its generator by the C++ CTest).

use std::path::PathBuf;
use std::process::Command;

// Hand-derived from what the C++ example's make_fixture.py encodes; identical to that example's
// golden (see examples/osm-pbf/testdata/check_fixture.py).
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
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../examples/osm-pbf/testdata/mini.osm.pbf")
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

    let par = osmstat(&[fix]);
    assert!(
        par.status.success(),
        "parallel run failed: {}",
        String::from_utf8_lossy(&par.stderr)
    );
    let seq = osmstat(&["--sequential", fix]);
    assert!(
        seq.status.success(),
        "sequential run failed: {}",
        String::from_utf8_lossy(&seq.stderr)
    );

    let par_out = String::from_utf8(par.stdout).unwrap();
    let seq_out = String::from_utf8(seq.stdout).unwrap();
    assert_eq!(par_out, seq_out, "parallel and sequential stdout differ");
    assert_eq!(par_out, GOLDEN, "stdout does not match the golden");
}

#[test]
fn baseline_agrees_on_the_fixture() {
    // --baseline runs the osmpbf crate over the same file; it must succeed and (on stderr) agree with
    // our id sum -- an independent third decoder cross-checking the fixture.
    let fix = fixture();
    let out = osmstat(&["--baseline", fix.to_str().unwrap()]);
    assert!(
        out.status.success(),
        "baseline run failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(
        err.contains("id sum 0000000000002ee9"),
        "osmpbf baseline disagrees on id sum:\n{err}"
    );
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
    ] {
        let out = osmstat(&args);
        assert!(
            !out.status.success(),
            "osmstat exited 0 on a truncated file (args {args:?})"
        );
    }
    let _ = std::fs::remove_file(&tmp);
}
