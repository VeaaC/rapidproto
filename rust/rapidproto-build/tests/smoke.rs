//! End-to-end smoke test: drive the embedded wasm generator through wasmi on the consumer example
//! and check it produces plausible Rust. Byte-for-byte parity with native rustgen is pinned
//! separately by the golden tests; this just proves the runtime wiring works.

use std::path::PathBuf;

use rapidproto_build::{generate, Request, SourceFile};

fn proto(name: &str) -> SourceFile {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../examples/consumer/proto")
        .join(name);
    SourceFile {
        name: name.to_owned(),
        body: std::fs::read_to_string(&path).expect("read fixture proto"),
    }
}

#[test]
fn generates_rust_for_the_consumer_example() {
    let request = Request {
        entries: vec!["message.proto".to_owned()],
        sources: vec![proto("message.proto"), proto("types.proto")],
        modes: String::new(),
        unknown_all: false,
        arena: true,
        stream: true, // exercise both models through the blob
    };
    let rust = generate(&request).expect("generation succeeds");
    assert!(
        rust.contains("mod demo"),
        "expected the demo module:\n{rust}"
    );
    assert!(
        rust.contains("Shape"),
        "expected the Shape message:\n{rust}"
    );
    assert!(
        rust.contains("pub mod stream") && rust.contains("Visitor<'a>"),
        "expected the stream visitor model:\n{rust}"
    );
}

#[test]
fn a_missing_import_is_reported_not_panicked() {
    // Only the entry is supplied; its import of types.proto cannot be resolved.
    let request = Request {
        entries: vec!["message.proto".to_owned()],
        sources: vec![proto("message.proto")],
        modes: String::new(),
        unknown_all: false,
        arena: true,
        stream: false,
    };
    let error = generate(&request).expect_err("missing import should fail");
    assert_eq!(error.code, 3, "resolve-stage exit code");
    assert!(
        error.message.contains("types.proto"),
        "error should name the missing import: {}",
        error.message
    );
}
