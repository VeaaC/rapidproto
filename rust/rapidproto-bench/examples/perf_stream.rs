// Cross-language perf harness for the streaming decoder: decode one C++ streaming-bench scenario
// repeatedly under `perf stat`, for an instructions/byte comparison against the C++ bench's `ins/B` on
// byte-identical input. Same arms as the `stream` runner.
//
//   SCENARIO="records dense (stream)" RAPIDPROTO_BENCH_DIR=<dir> \
//     perf stat -e instructions:u taskset -c 0 ./target/release/examples/perf_stream
//
// ARM picks the arm (default `generated`; or `wire`); ITERS the decode count (default: ~2 GB of
// input). The run prints the bytes decoded to divide the instruction count by.

fn main() {
    rapidproto_bench::runner::perf(rapidproto_bench::stream::scenarios(), "generated");
}
