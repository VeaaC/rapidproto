// Cross-language perf harness for the arena decoder: decode one C++ arena-bench scenario repeatedly
// under `perf stat`, for an instructions/byte comparison against the C++ bench's `ins/B` on
// byte-identical input. Same arms and checksum walks as the `bench` runner (any of its arms works).
//
//   SCENARIO=records RAPIDPROTO_BENCH_DIR=<dir> \
//     perf stat -e instructions:u taskset -c 0 ./target/release/examples/perf_bench
//
// ARM picks the arm (default `rapidproto`, the cold arena; WARM=1 is short for `rapidproto-warm`, the
// C++ arena-warm arm); ITERS the decode count (default: ~2 GB of input). The run prints the bytes
// decoded to divide the instruction count by.

fn main() {
    let default_arm = if std::env::var("WARM").is_ok_and(|v| v == "1") {
        "rapidproto-warm"
    } else {
        "rapidproto"
    };
    rapidproto_bench::runner::perf(rapidproto_bench::scenario_specs(), default_arm);
}
