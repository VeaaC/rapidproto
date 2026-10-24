//! Dogfooding gate: regenerate every committed `rustgen-golden` golden through each generator backend
//! this build has (the native library and the embedded wasm blob, see the crate docs) and assert it is
//! byte-identical to the checked-in file.
//!
//! The committed goldens are rustgen output pinned against the C++ generator by the `[rustgen-golden]`
//! case in `tests/test_rustgen.cpp`. So that case (C++ CI) and this test together prove every backend
//! the crate ships produces exactly what native rustgen does, continuously, through the code a
//! consumer's build script actually runs.
//!
//! The golden set mirrors that C++ case: one plain golden per `tests/corpus/**.proto` (stem =
//! basename) plus the `tests/bench/bench.proto` and `tests/wire_fixtures/wire_all.proto` goldens,
//! each with exported stream macros under its stem; the stream-only golden; and four field-modes
//! goldens. The field-modes profiles are mirrored from `test_rustgen.cpp` (the `*_spec()` helpers);
//! if either side changes, this test fails loudly with a diff — which is the point.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use rapidproto_build::{Backend, Config, BACKENDS};

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
fn check(stem: &str, generated: &str, backend: Backend, verified: &mut BTreeSet<String>) {
    let path = golden_dir().join(format!("{stem}.rs"));
    let committed = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("read golden {}: {e}", path.display()));
    assert!(
        generated == committed,
        "{backend:?} output for golden '{stem}' differs from the committed golden {}.\n\
         The build's generator drifted from native rustgen, or a golden was not regenerated.",
        path.display()
    );
    verified.insert(stem.to_owned());
}

/// The config every golden starts from: the proto's own directory as the include path.
fn config_for(proto: &Path, backend: Backend) -> Config {
    Config::new()
        .include(proto.parent().expect("proto has a parent dir"))
        .backend(backend)
}

/// Generate `proto` under `config`, panicking with the proto's path on failure.
fn gen(config: Config, proto: &Path) -> String {
    config
        .generate(&[proto])
        .unwrap_or_else(|e| panic!("generate {}: {e}", proto.display()))
}

/// A plain golden (no field modes), emitted with BOTH models (arena + stream) and its stream macros
/// exported under `stem`, matching the committed plain goldens (test_rustgen's `both`).
fn gen_plain(proto: &Path, stem: &str, backend: Backend) -> String {
    gen(
        config_for(proto, backend)
            .stream(true)
            .export_stream_macros(stem),
        proto,
    )
}

/// A field-modes golden: `modes` is a `.modes` profile. Emitted arena-only (field modes are
/// arena-storage options, N/A to a streaming visitor), matching test_rustgen's modes goldens.
fn gen_modes(proto: &Path, modes: &str, backend: Backend) -> String {
    gen(config_for(proto, backend).field_modes(modes), proto)
}

/// A stream-only golden (stream model, no arena, macros not exported): the
/// `Config::arena(false).stream(true)` path.
fn gen_stream_only(proto: &Path, backend: Backend) -> String {
    gen(config_for(proto, backend).arena(false).stream(true), proto)
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
fn goldens_match_through_every_backend() {
    for &backend in BACKENDS {
        goldens_match(backend);
    }
}

fn goldens_match(backend: Backend) {
    let root = repo_root();
    let mut verified = BTreeSet::new();

    // Plain goldens: one per corpus proto (stem = basename). Stems are distinct across subdirs (the
    // C++ case asserts it), so a basename stem is unambiguous.
    let mut protos = Vec::new();
    corpus_protos(&root.join("tests/corpus"), &mut protos);
    for proto in &protos {
        let stem = proto.file_stem().unwrap().to_string_lossy();
        check(
            &stem,
            &gen_plain(proto, &stem, backend),
            backend,
            &mut verified,
        );
    }

    // The bench schema's golden (generated like a corpus proto, from tests/bench/).
    let bench = root.join("tests/bench/bench.proto");
    check(
        "bench",
        &gen_plain(&bench, "bench", backend),
        backend,
        &mut verified,
    );
    // The wire-fixture schema's golden (from tests/wire_fixtures/).
    let wire_all = root.join("tests/wire_fixtures/wire_all.proto");
    let generated = gen_plain(&wire_all, "wire_all", backend);
    check("wire_all", &generated, backend, &mut verified);

    // The stream-only golden (no arena, macros not exported), mirroring test_rustgen.cpp's
    // `streamonly` case.
    let nestenum = root.join("tests/corpus/nsedge/nestenum.proto");
    let generated = gen_stream_only(&nestenum, backend);
    check("streamonly", &generated, backend, &mut verified);

    // Field-modes goldens — profiles mirrored from test_rustgen.cpp's *_spec() helpers (no `name`
    // directive, matching the empty profile_name there).
    let corpus = |rel: &str| root.join("tests/corpus").join(rel);
    for (stem, proto, modes) in [
        (
            "proto3_modes",
            "proto3.proto",
            "drop p3.Msg.name\nraw p3.Msg.self\nunknown-fields p3.Msg\n",
        ),
        (
            "arena_modes_lean",
            "arena_modes.proto",
            "drop fm.Holder.debug\ndrop fm.Holder.extra\ndrop fm.Holder.old_ids\n\
             raw fm.Blob\nraw fm.Holder.grp\n",
        ),
        (
            "arena_unknown_tracked",
            "arena_unknown.proto",
            "unknown-fields au.Holder\nunknown-fields au.Flag\n",
        ),
        (
            "messageset_unknown",
            "messageset.proto",
            "unknown-fields ms.Outer.Container\n",
        ),
    ] {
        let generated = gen_modes(&corpus(proto), modes, backend);
        check(stem, &generated, backend, &mut verified);
    }

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
        "committed goldens not covered by this gate ({backend:?}): {unverified:?}"
    );
}
