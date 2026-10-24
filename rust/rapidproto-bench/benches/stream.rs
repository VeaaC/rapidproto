//! Decode benchmark for the generated Rust streaming decoder: the scenarios of the C++ streaming bench
//! (`rapidproto_bench`, `tests/bench_streamgen.cpp`), each timing its `generated` arm (see `src/stream.rs`)
//! for a Rust-vs-Rust A/B of a streaming-decoder change against the shape it targets. The `stream`
//! runner (`src/stream_main.rs`) is the arm comparison; this is the A/B.
//!
//! Input: the shared payload directory, produced by the C++ bench's generator mode -- there is no Rust
//! builder, so without `RAPIDPROTO_BENCH_DIR` the bench is skipped:
//!
//! ```text
//! RAPIDPROTO_BENCH_DUMP_DIR=<dir> ./build/<cfg>/rapidproto_bench
//! RAPIDPROTO_BENCH_DIR=<dir> RUSTFLAGS="-Cllvm-args=-align-all-functions=6" \
//!     taskset -c 0 cargo bench -p rapidproto-bench --bench stream
//! ```
//!
//! Run discipline and units as the arena bench (`benches/decode.rs`). The 90 varint-sweep scenarios
//! (group `stream_rv`) use shortened sample settings; select a subset with criterion's name filter
//! (`cargo bench .. -- "rv fx3"`).
#![allow(clippy::all)]

use std::cell::OnceCell;

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};
use rapidproto_bench::payloads::{self, Source};
use rapidproto_bench::stream;

fn stream_bench(c: &mut Criterion) {
    let Some(dir) = std::env::var_os("RAPIDPROTO_BENCH_DIR") else {
        eprintln!("stream: skipped (the payloads need RAPIDPROTO_BENCH_DIR)");
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    // The single-payload scenarios, then the sweep's pools in their own group.
    for (group_name, pools) in [("stream", false), ("stream_rv", true)] {
        let mut group = c.benchmark_group(group_name);
        if pools {
            group
                .sample_size(10)
                .warm_up_time(std::time::Duration::from_millis(500))
                .measurement_time(std::time::Duration::from_secs(1));
        }
        for sc in stream::scenarios() {
            if matches!(sc.source, Source::Pool(_)) != pools {
                continue;
            }
            if !payloads::present(&dir, &sc.source) {
                eprintln!(
                    "{group_name}: {} skipped (not in the payload directory)",
                    sc.name
                );
                continue;
            }
            let arms = (sc.arms)();
            let generated = arms[0].decoder().expect("the generated arm");
            // Loaded on first use: criterion runs only the benches its name filter selects.
            let inputs = OnceCell::new();
            group.throughput(Throughput::Bytes(
                payloads::bytes_per_call(&dir, &sc.source) as u64,
            ));
            group.bench_function(sc.name.as_str(), |b| {
                let inputs = inputs.get_or_init(|| payloads::load(&dir, &sc.source));
                let mut at = 0usize;
                b.iter(|| {
                    let buf = &inputs.buffers[at % inputs.buffers.len()];
                    at += 1;
                    black_box(generated(black_box(buf.as_slice())))
                });
            });
        }
        group.finish();
    }
}

criterion_group!(benches, stream_bench);
criterion_main!(benches);
