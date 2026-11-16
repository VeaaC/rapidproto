// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The `records sparse (dropped)` scenario (see bench_arm_records_sparse.hpp).
#include "bench_arm_records_sparse.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "bench.rp.hpp"         // the arena tree under records_sparse.modes (its own OUT_DIR)
#include "bench_baselines.hpp"  // rpbaseline::protoc_record_sparse, in its own TU
#include "bench_harness.hpp"
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

namespace rprecords_sparse {
namespace {

std::uint64_t checksum(const rp::arena::bench::RecordSet* d) {
    if (d == nullptr) {
        return 0;
    }
    std::uint64_t s = 0;
    for (const rp::arena::bench::Record& r : d->records()) {
        s += static_cast<std::uint64_t>(r.f1()) + static_cast<std::uint64_t>(r.f10()) +
             static_cast<std::uint64_t>(r.f20());
    }
    return s;
}

}  // namespace

int run_arm(const std::string& buf) {
    const rapidproto::ByteView v(buf);
    rapidproto::Arena w;
    const std::vector<rpbench::Arm> a = {
        {"protoc", [&]() { return rpbaseline::protoc_record_sparse(buf); }},
        {"arena-warm",
         [&]() {
             w.reset();
             return checksum(rp::arena::bench::RecordSet::decode(v, w));
         }},
    };
    return rpbench::run("records sparse (dropped)", static_cast<double>(buf.size()), a);
}

}  // namespace rprecords_sparse
