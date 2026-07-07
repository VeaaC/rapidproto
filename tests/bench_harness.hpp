// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// Shared measurement harness for the standalone decode benchmarks (rapidproto_bench,
// rapidproto_arena_bench). Header-only so each bench links its own copy; decoder-agnostic (an arm is
// a nullary closure returning a checksum, so it can carry any state -- a fresh Arena, a protoc
// message, whatever). The methodology, and why it is built this way:
//
// The candidates differ by a few percent, but two environmental effects swamp that in naive
// wall-clock timing: code PLACEMENT (a rebuild relayouts everything, +-20-30%) and CPU FREQUENCY
// (governor / turbo / thermal, unfixable without root). We defeat both:
//   * All candidates run in ONE binary, and each round measures every arm back-to-back, so arm k's
//     cost RATIO to the baseline (arm 0) is taken at one instantaneous frequency -- turbo drift
//     cancels in the ratio (verified: a real -57% delta reads identically idle and under 3 saturated
//     cores). The ratio + its significance, not the absolute rate, is the trustworthy verdict.
//   * Measurement order rotates each round, so first-/second-slot bias cancels (else a null A-vs-A
//     comparison falsely reads ~0.3% "significant").
//   * Sampling is ADAPTIVE: rounds accumulate until every ratio's verdict is confident -- the effect
//     is clearly non-zero, or its CI is tight enough to call it a wash -- or a wall-time budget
//     elapses (then it warns that a ratio stayed ambiguous). The verdict is three-way: noise (CI
//     spans zero) / flat (real but < 0.5%) / SIG (real and meaningful).
//   * Where the kernel permits unprivileged self-monitoring (perf_event_paranoid <= 2) we count CPU
//     cycles and report frequency-invariant cycles/byte; otherwise we fall back to wall time.
//   * We self-pin to the current core (RAPIDPROTO_BENCH_NO_PIN opts out) and spin to steady-state
//     frequency before measuring.
//
// On a hybrid (P/E-core) CPU, still launch on a performance core so it does not pin to an E-core:
//   taskset -c 0 ./build/gcc/rapidproto_bench

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rpbench {

// One benchmark arm: a label and a nullary closure that decodes and returns a checksum (which both
// stops the loop being optimized away and cross-checks correctness against arm 0).
using Work = std::function<std::uint64_t()>;
struct Arm {
    const char* label;
    Work fn;
};

using Clock = std::chrono::steady_clock;
inline double ns_since(Clock::time_point t) {
    return std::chrono::duration<double, std::nano>(Clock::now() - t).count();
}

// One-time (no root): pin to the current core so we are not migrated mid-measurement, then spin to
// a steady-state frequency so we do not measure the turbo ramp.
inline void prepare_env() {
#if defined(__linux__)
    if (std::getenv("RAPIDPROTO_BENCH_NO_PIN") == nullptr) {
        const int cpu = sched_getcpu();
        if (cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            sched_setaffinity(0, sizeof set, &set);
        }
    }
#endif
    const auto t0 = Clock::now();
    volatile std::uint64_t x = 0;
    while (ns_since(t0) < 300e6) {
        ++x;  // ~300 ms: reach sustained frequency before timing anything
    }
}

// Opportunistic per-process hardware counter (cycles or instructions), if the kernel allows
// unprivileged self-monitoring. Returns a perf fd, or -1 to fall back / omit.
inline int open_counter(std::uint64_t config) {
#if defined(__linux__)
    perf_event_attr attr;
    std::memset(&attr, 0, sizeof attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof attr;
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0UL));
#else
    (void)config;
    return -1;
#endif
}

// Cycles are frequency-invariant but NOT placement-invariant (a function's cyc/B shifts with its
// address/alignment across builds). Retired INSTRUCTIONS are deterministic and placement-invariant:
// if two arms differ in ins/B the code genuinely differs; if only cyc/B differs, suspect placement.
struct Counters {
    int cyc = -1;
    int instr = -1;
};
// Prepared once (function-local static), shared by every run() call in the process.
inline Counters metric_fds() {
    static const Counters c = [] {
        prepare_env();
        return Counters{open_counter(PERF_COUNT_HW_CPU_CYCLES),
                        open_counter(PERF_COUNT_HW_INSTRUCTIONS)};
    }();
    return c;
}

struct Cost {
    double ns;
    double cyc;    // -1 when no perf counter
    double instr;  // -1 when no instructions counter
};
inline Cost sample(Counters cnt, const Work& f, std::uint64_t& out) {
#if defined(__linux__)
    if (cnt.cyc >= 0) {
        ioctl(cnt.cyc, PERF_EVENT_IOC_RESET, 0);
        ioctl(cnt.instr, PERF_EVENT_IOC_RESET, 0);  // ioctl on -1 is a harmless no-op (EBADF)
        ioctl(cnt.cyc, PERF_EVENT_IOC_ENABLE, 0);
        ioctl(cnt.instr, PERF_EVENT_IOC_ENABLE, 0);
        const auto t0 = Clock::now();
        out = f();
        const double ns = ns_since(t0);
        ioctl(cnt.cyc, PERF_EVENT_IOC_DISABLE, 0);
        ioctl(cnt.instr, PERF_EVENT_IOC_DISABLE, 0);
        long long c = 0;
        if (read(cnt.cyc, &c, sizeof c) != static_cast<ssize_t>(sizeof c)) {
            c = 0;
        }
        long long ins = 0;
        if (cnt.instr < 0 ||
            read(cnt.instr, &ins, sizeof ins) != static_cast<ssize_t>(sizeof ins)) {
            ins = -1;
        }
        return {ns, static_cast<double>(c), static_cast<double>(ins)};
    }
#endif
    (void)cnt;
    const auto t0 = Clock::now();
    out = f();
    return {ns_since(t0), -1.0, -1.0};
}

struct Stat {
    double mean;
    double median;
    double ci_half;  // 95% half-width of the mean
};
inline Stat stat(std::vector<double> v) {
    const auto n = static_cast<double>(v.size());
    double mean = 0;
    for (const double x : v) {
        mean += x;
    }
    mean /= n;
    double var = 0;
    for (const double x : v) {
        var += (x - mean) * (x - mean);
    }
    var /= (v.size() > 1 ? n - 1 : 1);
    std::sort(v.begin(), v.end());
    return {mean, v[v.size() / 2], 1.96 * std::sqrt(var / n)};
}

// Measure every arm over one op of `byte_size` bytes and print a table. Arm 0 is the BASELINE; each
// other arm is reported as an absolute rate AND as a drift-invariant cost ratio to the baseline with
// a significance verdict. Returns the count of arms whose checksum disagreed with arm 0.
inline int run(const char* scenario, double byte_size, const std::vector<Arm>& arms) {
    const std::size_t n = arms.size();
    const Counters cnt = metric_fds();
    const bool cyc = cnt.cyc >= 0;
    const bool ins = cnt.instr >= 0;
    constexpr std::size_t kMinRounds =
        30;  // floor; a null (wash) result self-extends for tighter CI
    constexpr double kBudgetNs = 3.0e9;
    // A ratio's verdict is CONFIDENT once the effect is clearly non-zero (|mean-1| > 3 CI, so the
    // sign and rough size are settled) OR the CI is tight enough to confidently call it a wash. We
    // sample until every arm is confident, then stop; we only warn "noisy" if the budget runs out
    // while some arm is still ambiguous. (Chasing an absolute CI floor over-samples clear wins and
    // false-flags them as noisy.)
    constexpr double kNullTol = 0.003;  // CI half-width under 0.3% => confidently a wash
    const auto confident = [](const Stat& sr) {
        return std::fabs(sr.mean - 1.0) > 3.0 * sr.ci_half || sr.ci_half / sr.mean < kNullTol;
    };

    std::vector<std::vector<double>> ns_s(n), cyc_s(n), instr_s(n), ratio(n);
    std::vector<std::uint64_t> sums(n, 0);

    std::uint64_t junk = 0;
    for (int w = 0; w < 5; ++w) {  // warm caches / predictors for every arm
        for (const auto& a : arms) {
            sample(cnt, a.fn, junk);
        }
    }

    const auto start = Clock::now();
    bool converged = false;
    for (std::size_t round = 0; !converged; ++round) {
        std::vector<double> prim(n);  // this round's primary cost per arm
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t k = (j + round) % n;  // rotate start each round: cancels slot bias
            std::uint64_t s = 0;
            const Cost cost = sample(cnt, arms[k].fn, s);
            sums[k] = s;
            ns_s[k].push_back(cost.ns);
            if (cyc) {
                cyc_s[k].push_back(cost.cyc);
            }
            if (ins) {
                instr_s[k].push_back(cost.instr);
            }
            prim[k] = cyc ? cost.cyc : cost.ns;
        }
        for (std::size_t k = 1; k < n; ++k) {
            if (prim[0] > 0) {
                ratio[k].push_back(prim[k] / prim[0]);
            }
        }
        if (round + 1 >= kMinRounds && round % 8 == 0) {
            converged = true;
            for (std::size_t k = 1; k < n; ++k) {
                converged = converged && confident(stat(ratio[k]));
            }
        }
        if (ns_since(start) > kBudgetNs) {
            break;
        }
    }

    int bad = 0;
    bool ambiguous = false;
    for (std::size_t k = 0; k < n; ++k) {
        const bool ok = sums[k] == sums[0];
        bad += ok ? 0 : 1;
        const Stat sns = stat(ns_s[k]);
        char rate[72];
        if (cyc && ins) {
            std::snprintf(rate, sizeof rate, "%6.3f GB/s %5.2f cyc/B %6.1f ins/B",
                          byte_size / sns.median, stat(cyc_s[k]).median / byte_size,
                          stat(instr_s[k]).median / byte_size);
        } else if (cyc) {
            std::snprintf(rate, sizeof rate, "%6.3f GB/s %5.2f cyc/B", byte_size / sns.median,
                          stat(cyc_s[k]).median / byte_size);
        } else {
            std::snprintf(rate, sizeof rate, "%6.3f GB/s", byte_size / sns.median);
        }
        char cmp[80];
        if (k == 0) {
            std::snprintf(cmp, sizeof cmp, "(baseline)");
        } else {
            const Stat sr = stat(ratio[k]);
            const double faster = (1.0 / sr.mean - 1.0) * 100.0;  // + => arm k out-throughputs base
            const double band = (sr.ci_half / sr.mean) * 100.0;
            ambiguous = ambiguous || !confident(sr);
            // Three-way verdict: "noise" = the CI spans zero (indistinguishable); "flat" = real but
            // under 0.5% (statistically settled, practically nil -- not worth shipping); "SIG" = a
            // real, meaningful difference. A bare statistical test would stamp a 0.1% delta "SIG".
            const char* verdict =
                std::fabs(faster) <= band ? "noise" : (std::fabs(faster) < 0.5 ? "flat" : "SIG");
            std::snprintf(cmp, sizeof cmp, "vs %s %+6.1f%% +-%.1f%% [%s]", arms[0].label, faster,
                          band, verdict);
        }
        std::printf("  %-16s %-11s %-37s %-34s%s\n", scenario, arms[k].label, rate, cmp,
                    ok ? "" : "  !! CHECKSUM MISMATCH");
    }
    if (ambiguous) {
        std::printf(
            "  %-16s (!) noisy: a ratio stayed ambiguous after %.0fs. Close background load;"
            " on a laptop use AC power + let it cool; optionally pin governor=performance /"
            " disable turbo (root).\n",
            scenario, kBudgetNs / 1e9);
    }
    return bad;
}

}  // namespace rpbench
