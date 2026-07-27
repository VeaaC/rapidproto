// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// See bench_arm_compute.hpp for why this arm is compiled separately.
//
// Measured on this translation unit at -O3 -DNDEBUG: 646,334 bytes of .text against 163,864
// when built with -DRP_FLATTEN= (3.9x), and 99s against 9s to compile. (A TU instantiating only
// Instance::decode gives a different ratio -- these numbers describe this file.)
#include "bench_arm_compute.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bench_harness.hpp"
#include "google/cloud/compute/v1beta/compute.rp.hpp"
#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/runtime.hpp"

namespace rpcompute {
namespace {

// Touch every repeated/map field the payload populates, so no part of the decode can be elided
// and the checksum is a real function of the decoded tree.
// googleapis marks essentially every scalar/string field `optional`, so these accessors return
// std::optional; repeated sub-messages arrive as ArrayView VALUES (not pointers), and a map is
// a MapView whose entries expose key()/value().
std::uint64_t checksum_compute(const rp::google::cloud::compute::v1beta::Instance* inst) {
    if (inst == nullptr) {
        return 0;
    }
    std::uint64_t s = inst->name().value_or("").size() + inst->description().value_or("").size();
    for (const auto& disk : inst->disks()) {
        s += disk.device_name().value_or("").size() +
             static_cast<std::uint64_t>(disk.disk_size_gb().value_or(0)) +
             disk.type().value_or("").size();
    }
    for (const auto& nic : inst->network_interfaces()) {
        s += nic.name().value_or("").size() + nic.network().value_or("").size();
    }
    for (const auto& entry : inst->labels()) {
        s += entry.key().size() + entry.value().size();
    }
    // Reach the rest of the populated closure. decode() materializes the whole tree regardless,
    // so this is not what makes those sub-decoders run -- it keeps the checksum sensitive to
    // them, so a future field-mode or payload change cannot quietly stop measuring them.
    for (const auto& acct : inst->service_accounts()) {
        s += acct.email().value_or("").size() + acct.scopes().size();
    }
    for (const auto& acc : inst->guest_accelerators()) {
        s += static_cast<std::uint64_t>(acc.accelerator_count().value_or(0));
    }
    if (const auto* meta = inst->metadata()) {
        for (const auto& item : meta->items()) {
            s += item.key().value_or("").size() + item.value().value_or("").size();
        }
    }
    if (const auto* sched = inst->scheduling()) {
        s += static_cast<std::uint64_t>(sched->automatic_restart().value_or(false));
    }
    return s;
}

// Read the build-time-encoded payload. A missing file is fatal rather than a skipped arm: the
// arm is compiled in only when CMake found the corpus, so its absence means the build lied.
std::string read_compute_payload() {
    std::FILE* file = std::fopen(RAPIDPROTO_COMPUTE_PAYLOAD, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", RAPIDPROTO_COMPUTE_PAYLOAD);
        std::abort();
    }
    std::string out;
    char chunk[8192];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
        out.append(chunk, got);
    }
    const bool bad = std::ferror(file) != 0;
    std::fclose(file);
    if (bad) {
        std::fprintf(stderr, "error reading %s\n", RAPIDPROTO_COMPUTE_PAYLOAD);
        std::abort();
    }
    return out;
}

}  // namespace

bool run_arm() {
    // Large real-world schema: one compute.proto Instance. No protoc arm -- protoc C++ for a
    // 103k-line schema is impractical to build, and the question here is RapidProto against
    // itself: does the flattened decoder's ~4x code size cost throughput? Compare this arm
    // between a normal build and one built with -DRP_FLATTEN=. Read the flatten knob in
    // architecture.md first: the baselines are split out so the control is valid, but every
    // RapidProto arm still shares this TU, so a delta under ~5% here is layout noise.
    {
        const std::string cbuf = read_compute_payload();
        const rapidproto::ByteView cview(cbuf);
        rapidproto::Arena setup;
        const std::uint64_t expect =
            checksum_compute(rp::google::cloud::compute::v1beta::Instance::decode(cview, setup));
        // Load-bearing, not defensive: this is the only correctness check on the compute
        // decoder anywhere in the tree (the corpus gate sweeps the front end but never runs a
        // decode), and it catches a truncated or stale payload as a loud failure.
        if (expect == 0) {
            std::fprintf(stderr,
                         "compute Instance decode produced an empty checksum (payload %s "
                         "empty or stale?)\n",
                         RAPIDPROTO_COMPUTE_PAYLOAD);
            return false;
        }
        rapidproto::Arena cwarm;
        std::vector<rpbench::Arm> carms = {
            {"arena-cold",
             [&]() {
                 rapidproto::Arena a;
                 return checksum_compute(
                     rp::google::cloud::compute::v1beta::Instance::decode(cview, a));
             }},
            {"arena-warm",
             [&]() {
                 cwarm.reset();
                 return checksum_compute(
                     rp::google::cloud::compute::v1beta::Instance::decode(cview, cwarm));
             }},
        };
        (void)rpbench::run("compute Instance", static_cast<double>(cbuf.size()), carms);
    }

    return true;
}

}  // namespace rpcompute
