// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The MICRO tier of the two-tier streaming benchmark (see bench_two_tier.hpp): the comprehensive
// Scalars decode compiled ALONE in this small translation unit, so the compiler inlines the hot wire
// primitives (read_varint etc.) -- the code's ceiling. bench_streamgen.cpp compiles the identical
// decode amid the whole bench TU (the realistic, out-of-line-primitive number) and compares the two.

#include "bench_two_tier.hpp"

// The generated symbol the large-TU bench compares against.
RP_BENCH_DEFINE_SCALARS_DECODE(rp_bench_decode_scalars_micro)
