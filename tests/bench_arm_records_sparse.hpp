// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The `records sparse (dropped)` scenario: the `records` payload decoded by an arena tree generated
// under tests/bench/records_sparse.modes, which keeps f1, f10 and f20 of each Record and drops the
// other 17 fields. A dropped field has no storage and is skipped, its wire still validated. The
// protoc baseline parses every field and sums the same three.
//
// Declaration only: the profiled tree lives in its own translation unit
// (bench_arm_records_sparse.cpp), because a TU includes ONE generated tree's runtime copy, and this
// tree's bench.rp.hpp has the same name as the unprofiled one.

#include <string>

namespace rprecords_sparse {

// Measure the scenario over `buf`, the `records` payload. Returns the harness's per-arm mismatch
// count (0 = clean).
int run_arm(const std::string& buf);

}  // namespace rprecords_sparse
