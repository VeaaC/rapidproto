//! Dogfooding gate: regenerate every committed `rustgen-golden` golden through the embedded wasm
//! blob and assert it is byte-identical to the checked-in file.
//!
//! The committed goldens are NATIVE rustgen output, pinned against the C++ generator by the
//! `[rustgen-golden]` case in `tests/test_rustgen.cpp`. So native == committed (C++ CI) and
//! wasm == committed (this test) together prove wasm == native, continuously — the real
//! proof-of-function for the blob, through the actual shipping runtime (wasmi).
//!
//! The golden set mirrors that C++ case: one plain golden per `tests/corpus/**.proto` (stem =
//! basename), the `tests/bench/bench.proto` golden, the stream-only goldens (plain and with exported
//! stream macros), and four field-modes goldens. The field-modes profiles are mirrored from
//! `test_rustgen.cpp` (the `*_spec()` helpers); if either side changes, this test fails loudly with
//! a diff — which is the point.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use rapidproto_build::Config;

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()
        .expect("repo root")
}

fn golden_dir() -> PathBuf {
    repo_root().join("rust/rustgen-golden/goldens")
}

/// Regenerate one golden and compare to the committed file; record the stem as verified.
fn check(stem: &str, generated: &str, verified: &mut BTreeSet<String>) {
    let path = golden_dir().join(format!("{stem}.rs"));
    let committed = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("read golden {}: {e}", path.display()));
    assert!(
        generated == committed,
        "wasm output for golden '{stem}' differs from the committed (native) golden {}.\n\
         The blob drifted from native rustgen, or a golden was not regenerated.",
        path.display()
    );
    verified.insert(stem.to_owned());
}

/// The config every golden starts from: the proto's own directory as the include path.
fn config_for(proto: &Path) -> Config {
    Config::new().include(proto.parent().expect("proto has a parent dir"))
}

/// Generate `proto` under `config`, panicking with the proto's path on failure.
fn gen(config: Config, proto: &Path) -> String {
    config
        .generate(&[proto])
        .unwrap_or_else(|e| panic!("generate {}: {e}", proto.display()))
}

/// A plain golden (no field modes), emitted with BOTH models (arena + stream), matching the committed
/// plain goldens (test_rustgen's `kBoth`).
fn gen_plain(proto: &Path) -> String {
    gen(config_for(proto).stream(true), proto)
}

/// A field-modes golden: `modes` is a `.modes` profile. Emitted arena-only (field modes are
/// arena-storage options, N/A to a streaming visitor), matching test_rustgen's modes goldens.
fn gen_modes(proto: &Path, modes: &str) -> String {
    gen(config_for(proto).field_modes(modes), proto)
}

/// A stream-only golden (stream model, no arena): the `Config::arena(false).stream(true)` path.
fn gen_stream_only(proto: &Path) -> String {
    gen(config_for(proto).arena(false).stream(true), proto)
}

/// A stream-only golden whose macros are exported under `prefix`.
fn gen_stream_export(proto: &Path, prefix: &str) -> String {
    gen(
        config_for(proto)
            .arena(false)
            .stream(true)
            .export_stream_macros(prefix),
        proto,
    )
}

/// Every `*.proto` under `dir`, recursively, sorted (deterministic) — the plain-golden corpus.
fn corpus_protos(dir: &Path, out: &mut Vec<PathBuf>) {
    let mut entries: Vec<_> = std::fs::read_dir(dir)
        .unwrap_or_else(|e| panic!("read {}: {e}", dir.display()))
        .map(|e| e.expect("dir entry").path())
        .collect();
    entries.sort();
    for path in entries {
        if path.is_dir() {
            corpus_protos(&path, out);
        } else if path.extension().is_some_and(|e| e == "proto") {
            out.push(path);
        }
    }
}

#[test]
fn goldens_match_native_through_the_blob() {
    let root = repo_root();
    let mut verified = BTreeSet::new();

    // Plain goldens: one per corpus proto (stem = basename). Stems are distinct across subdirs (the
    // C++ case asserts it), so a basename stem is unambiguous.
    let mut protos = Vec::new();
    corpus_protos(&root.join("tests/corpus"), &mut protos);
    for proto in &protos {
        let stem = proto.file_stem().unwrap().to_string_lossy();
        check(&stem, &gen_plain(proto), &mut verified);
    }

    // The bench schema's golden (generated like a corpus proto, from tests/bench/).
    let bench = root.join("tests/bench/bench.proto");
    check("bench", &gen_plain(&bench), &mut verified);

    // The stream-only goldens (no arena), mirroring test_rustgen.cpp's `streamonly` and
    // `streamexport_*` cases; each exported golden uses its stem as the prefix.
    check(
        "streamonly",
        &gen_stream_only(&root.join("tests/corpus/nsedge/nestenum.proto")),
        &mut verified,
    );
    for (stem, rel) in [
        ("streamexport_p3", "proto3.proto"),
        ("streamexport_main", "imports/main.proto"),
        ("streamexport_nopkg", "nsedge/nopkg.proto"),
    ] {
        check(
            stem,
            &gen_stream_export(&root.join("tests/corpus").join(rel), stem),
            &mut verified,
        );
    }

    // Field-modes goldens — profiles mirrored from test_rustgen.cpp's *_spec() helpers (no `name`
    // directive, matching the empty profile_name there).
    let corpus = |rel: &str| root.join("tests/corpus").join(rel);
    check(
        "proto3_modes",
        &gen_modes(
            &corpus("proto3.proto"),
            "drop p3.Msg.name\nraw p3.Msg.self\nunknown-fields p3.Msg\n",
        ),
        &mut verified,
    );
    check(
        "arena_modes_lean",
        &gen_modes(
            &corpus("arena_modes.proto"),
            "drop fm.Holder.debug\ndrop fm.Holder.extra\ndrop fm.Holder.old_ids\n\
             raw fm.Blob\nraw fm.Holder.grp\n",
        ),
        &mut verified,
    );
    check(
        "arena_unknown_tracked",
        &gen_modes(
            &corpus("arena_unknown.proto"),
            "unknown-fields au.Holder\nunknown-fields au.Flag\n",
        ),
        &mut verified,
    );
    check(
        "messageset_unknown",
        &gen_modes(
            &corpus("messageset.proto"),
            "unknown-fields ms.Outer.Container\n",
        ),
        &mut verified,
    );

    // Every committed golden must have been regenerated and checked -- so a new golden cannot slip
    // in unverified by this gate.
    let mut committed = BTreeSet::new();
    for entry in std::fs::read_dir(golden_dir()).expect("read golden dir") {
        let path = entry.expect("dir entry").path();
        if path.extension().is_some_and(|e| e == "rs") {
            committed.insert(path.file_stem().unwrap().to_string_lossy().into_owned());
        }
    }
    let unverified: Vec<_> = committed.difference(&verified).collect();
    assert!(
        unverified.is_empty(),
        "committed goldens not covered by this gate: {unverified:?}"
    );
}
