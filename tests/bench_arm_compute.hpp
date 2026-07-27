// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The large real-world schema arm (googleapis compute.proto `Instance`), in its own translation
// unit.
//
// Why its own TU, and why the baselines have theirs (bench_baselines.hpp): GCC's inline-growth
// budget is a whole-TU property, so measured variants sharing a translation unit re-time each
// other whenever the file changes. Two measurements from this benchmark, both of which initially
// read as real results:
//
//   * a protozero baseline arm moved 25% without being touched, because the decoders beside it
//     in the same file changed size;
//   * this arm moved 21% purely because the protoc arms were relocated to a different file.
//
// Neither involved any change to the code being measured. Splitting does not make an individual
// arm's cross-build A/B exact -- `-DRP_FLATTEN=` still changes this TU's own size, leaving a
// ~4-5% link-order/alignment floor -- but it removes that class of false result.
#pragma once

namespace rpcompute {

// Decodes the build-generated payload and reports the arm. Returns false if the payload is
// missing or decodes to nothing -- the only correctness check on this decoder anywhere in the
// tree, since the corpus gate sweeps the front end but never runs a decode.
bool run_arm();

}  // namespace rpcompute
