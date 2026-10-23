//! Streaming-decoder runner: the scenarios of the C++ streaming bench (`rapidproto_bench`) over the same
//! bytes, the generated streaming decoder (the baseline) against a hand-written wire loop -- see
//! `stream.rs` for the arms. The loop, options and output are the shared runner's (`runner.rs`, env
//! vars documented there); build profile and run discipline as the arena runner's (`main.rs`).
//!
//! ```text
//! # produce the shared payloads once (from the C++ bench; ~1.2 GB, keep it on disk):
//! RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_bench
//! RAPIDPROTO_BENCH_DIR=<dir> RUSTFLAGS="-Cllvm-args=-align-all-functions=6" \
//!     taskset -c 0 cargo run -p rapidproto-bench --release --bin stream
//! ```

use rapidproto_bench::runner::{self, Report};
use rapidproto_bench::stream;

fn main() {
    let report = Report {
        decoder: "rust-stream",
        ub: |_, _| false,
        legend: "",
    };
    std::process::exit(runner::run(stream::scenarios(), &report));
}
