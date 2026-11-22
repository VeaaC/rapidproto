//! Streaming-decoder runner: the scenarios of the C++ streaming bench (`rapidproto_bench`) over the same
//! bytes, the generated streaming decoder (the baseline) against a hand-written wire loop -- see
//! `stream.rs` for the arms. The loop, options and output are the shared runner's (`runner.rs`); build
//! profile as the arena runner's (`main.rs`). rust/README.md shows how to write the payloads and run
//! the bench.

use rapidproto_bench::runner::{self, Report};
use rapidproto_bench::stream;

fn main() {
    let report = Report {
        decoder: "rust-stream",
        ub: |_, _| false,
        legend: "",
        // Both arms carry their C++ names.
        cpp_arm: |arm| match arm {
            "generated" => Some("generated"),
            "wire" => Some("wire"),
            _ => None,
        },
    };
    std::process::exit(runner::run(stream::scenarios(), &report));
}
