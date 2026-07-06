// Isolated decode micro-benchmark, built as the standalone `rapidproto_bench` executable at
// -O3 -DNDEBUG, NOT linked into the Catch2 test binary. Why standalone: measuring decoders inside
// the large (3.5MB+) test binary makes results layout-sensitive; an unrelated code change can
// swing even protozero's (unchanged) numbers by 30%+. A small dedicated binary is layout-stable and
// fast to rebuild while iterating on the wire layer.
//
// The harness self-pins to the core it starts on and reaches steady-state frequency before timing.
// On a hybrid (P/E-core) CPU still launch it on a performance core so it does not pin to an E-core:
//   taskset -c 0 ./build/gcc/rapidproto_bench
//
// Each scenario decodes a large buffer that stresses one decode path (focused) or several at once
// (mixed), comparing three decoders over the same bytes:
//   generated: the rapidproto-generated decoder (p2::stream::Scalars; the product)
//   wire:      a hand loop on the WireReader primitives, feature-equivalent (dispatch + validate)
//   protozero: mapbox protozero, an established minimal-overhead pull parser (yardstick; its
//                wire-type checks are protozero_assert()s compiled out under NDEBUG, so it validates
//                marginally less than we do).
// Methodology (see the run() comment): each arm is measured against the baseline (arm 0) as a
// frequency-drift-invariant cost ratio, sampled adaptively until the ratio's 95% CI is tight, and
// reported with a significance verdict; cycles/byte is used where the kernel permits it. All
// decoders must agree on a checksum (guards correctness and stops the loops being optimized away);
// a mismatch is reported and makes the process exit non-zero.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "proto2.rp.stream.hpp"  // generated p2::stream::Scalars; -Itests/streamgen_golden (pulls runtime.hpp)
#include "rapidproto/runtime.hpp"

#if __has_include(<protozero/pbf_reader.hpp>)
#include <protozero/pbf_reader.hpp>
#define RAPIDPROTO_HAVE_PROTOZERO 1
#endif

using namespace rapidproto;  // NOLINT(google-build-using-namespace): bench convenience

namespace {

using Fn = std::uint64_t (*)(ByteView);
struct Arm {
    const char* label;
    Fn fn;
};

// Encoder-side buffer builders (the decoders under test are the SUT, never these).
void put_varint(std::string& b, std::uint64_t v) {
    while (v >= 0x80U) {
        b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
        v >>= 7U;
    }
    b.push_back(static_cast<char>(v));
}
void put_tag(std::string& b, std::uint32_t field, std::uint32_t wire) {
    put_varint(b, (static_cast<std::uint64_t>(field) << 3U) | wire);
}
void put_fixed32(std::string& b, std::uint32_t v) {
    for (unsigned i = 0; i < 4U; ++i) {
        b.push_back(static_cast<char>((v >> (8U * i)) & 0xFFU));
    }
}
void put_fixed64(std::string& b, std::uint64_t v) {
    for (unsigned i = 0; i < 8U; ++i) {
        b.push_back(static_cast<char>((v >> (8U * i)) & 0xFFU));
    }
}
std::uint64_t zigzag64(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1U) ^ static_cast<std::uint64_t>(v >> 63);
}
std::uint64_t double_bits(double d) {
    std::uint64_t b = 0;
    std::memcpy(&b, &d, sizeof b);
    return b;
}
std::uint64_t float_bits(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

// Measurement harness. The decode loops differ by a few percent, but two environmental effects
// swamp that signal in naive wall-clock timing: code PLACEMENT (a rebuild relayouts everything,
// +-20-30%) and CPU FREQUENCY (governor / turbo / thermal, unfixable without root). We defeat both:
//   * All candidates run in ONE binary, and each round measures every arm back-to-back, so arm k's
//     cost RATIO to the baseline (arm 0) is taken at one instantaneous frequency -- turbo drift
//     cancels in the ratio (verified: a real -57% delta reads identically idle and under 3 saturated
//     cores). The ratio + its significance, not the absolute rate, is the trustworthy verdict.
//   * Measurement order rotates each round, so first-/second-slot bias cancels (else a null A-vs-A
//     comparison falsely reads ~0.3% "significant").
//   * Sampling is ADAPTIVE: rounds accumulate until every ratio's verdict is confident -- the effect
//     is clearly non-zero, or its CI is tight enough to call it a wash -- or a wall-time budget
//     elapses (then it warns that a ratio stayed ambiguous). The reported verdict is three-way:
//     noise (CI spans zero) / flat (real but < 0.5%) / SIG (real and meaningful).
//   * Where the kernel permits unprivileged self-monitoring (perf_event_paranoid <= 2) we count CPU
//     cycles and report frequency-invariant cycles/byte; otherwise we fall back to wall time.
//   * We self-pin to the current core (RAPIDPROTO_BENCH_NO_PIN opts out) and spin to steady-state
//     frequency before measuring.

using Clock = std::chrono::steady_clock;
double ns_since(Clock::time_point t) {
    return std::chrono::duration<double, std::nano>(Clock::now() - t).count();
}

// One-time (no root): pin to the current core so we are not migrated mid-measurement, then spin to
// a steady-state frequency so we do not measure the turbo ramp.
void bench_prepare_env() {
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

// Opportunistic per-process CPU-cycle counter, if the kernel allows unprivileged self-monitoring.
// Returns a perf fd, or -1 to fall back to wall time.
int bench_open_cycles() {
#if defined(__linux__)
    perf_event_attr attr;
    std::memset(&attr, 0, sizeof attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof attr;
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0UL));
#else
    return -1;
#endif
}

// Prepared once, shared by every run<N> instantiation (a template-local static would re-run per N).
int bench_metric_fd() {
    static const int fd = (bench_prepare_env(), bench_open_cycles());
    return fd;
}

struct Cost {
    double ns;
    double cyc;  // -1 when no perf counter
};
Cost bench_sample(int fd, Fn f, ByteView b, std::uint64_t& out) {
#if defined(__linux__)
    if (fd >= 0) {
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        const auto t0 = Clock::now();
        out = f(b);
        const double ns = ns_since(t0);
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        long long c = 0;
        if (read(fd, &c, sizeof c) != static_cast<ssize_t>(sizeof c)) {
            c = 0;
        }
        return {ns, static_cast<double>(c)};
    }
#endif
    (void)fd;
    const auto t0 = Clock::now();
    out = f(b);
    return {ns_since(t0), -1.0};
}

struct Stat {
    double mean;
    double median;
    double ci_half;  // 95% half-width of the mean
};
Stat bench_stat(std::vector<double> v) {
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

// Measure every arm over `bytes` and print a table. Arm 0 is the BASELINE; each other arm is
// reported as an absolute rate AND as a drift-invariant cost ratio to the baseline with a
// significance verdict. Returns the count of arms whose checksum disagreed with arm 0.
template <std::size_t N>
int run(const char* scenario, ByteView bytes, const Arm (&arms)[N]) {
    const int fd = bench_metric_fd();
    const bool cyc = fd >= 0;
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

    std::array<std::vector<double>, N> ns_s;   // per-arm wall-time samples (one per round)
    std::array<std::vector<double>, N> cyc_s;  // per-arm cycle samples
    std::array<std::vector<double>, N> ratio;  // per-round primary-metric ratio arm[k]/arm[0]
    std::array<std::uint64_t, N> sums{};

    std::uint64_t junk = 0;
    for (int w = 0; w < 5; ++w) {  // warm caches / predictors for every arm
        for (const auto& a : arms) {
            bench_sample(fd, a.fn, bytes, junk);
        }
    }

    const auto start = Clock::now();
    bool converged = false;
    for (std::size_t round = 0; !converged; ++round) {
        std::array<double, N> prim{};  // this round's primary cost per arm
        for (std::size_t j = 0; j < N; ++j) {
            const std::size_t k = (j + round) % N;  // rotate start each round: cancels slot bias
            std::uint64_t s = 0;
            const Cost cost = bench_sample(fd, arms[k].fn, bytes, s);
            sums[k] = s;
            ns_s[k].push_back(cost.ns);
            if (cyc) {
                cyc_s[k].push_back(cost.cyc);
            }
            prim[k] = cyc ? cost.cyc : cost.ns;
        }
        for (std::size_t k = 1; k < N; ++k) {
            if (prim[0] > 0) {
                ratio[k].push_back(prim[k] / prim[0]);
            }
        }
        if (round + 1 >= kMinRounds && round % 8 == 0) {
            converged = true;
            for (std::size_t k = 1; k < N; ++k) {
                converged = converged && confident(bench_stat(ratio[k]));
            }
        }
        if (ns_since(start) > kBudgetNs) {
            break;
        }
    }

    (void)converged;
    const auto sz = static_cast<double>(bytes.size());
    int bad = 0;
    bool ambiguous = false;
    for (std::size_t k = 0; k < N; ++k) {
        const bool ok = sums[k] == sums[0];
        bad += ok ? 0 : 1;
        const Stat sns = bench_stat(ns_s[k]);
        char rate[48];
        if (cyc) {
            std::snprintf(rate, sizeof rate, "%6.3f GB/s %5.2f cyc/B", sz / sns.median,
                          bench_stat(cyc_s[k]).median / sz);
        } else {
            std::snprintf(rate, sizeof rate, "%6.3f GB/s", sz / sns.median);
        }
        char cmp[80];
        if (k == 0) {
            std::snprintf(cmp, sizeof cmp, "(baseline)");
        } else {
            const Stat sr = bench_stat(ratio[k]);
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
        std::printf("  %-16s %-10s %-26s %-34s%s\n", scenario, arms[k].label, rate, cmp,
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

constexpr int kN = 2'000'000;

// Shared wire/protozero arms reused by the simple single-handled-field scenarios. Each scenario
// differs only in which field/type it builds and decodes, so the arm bodies are written per case.

int scenario_varint_1byte() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, static_cast<std::uint64_t>(i) & 0x7FU);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 1 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(*v));
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("varint-1byte", ByteView(buf), arms);
}

int scenario_varint_multibyte() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, 0xF000'0000ULL + (static_cast<std::uint64_t>(i) & 0xFFFFU));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 1 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(*v));
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("varint-multibyte", ByteView(buf), arms);
}

int scenario_zigzag() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        const std::int64_t v = static_cast<std::int64_t>(i) * ((i & 1) != 0 ? -1003 : 1003);
        put_tag(buf, 6, 0);
        put_varint(buf, zigzag64(v));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::s64, std::int64_t v) {
                 s += static_cast<std::uint64_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 6 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint64_t>(zigzag_decode_64(*v));
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 6) {
                     s += static_cast<std::uint64_t>(r.get_sint64());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("zigzag-sint64", ByteView(buf), arms);
}

int scenario_fixed32() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 14, 5);
        put_fixed32(buf, float_bits(static_cast<float>(i) * 1.5F));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::fl, float v) { s += float_bits(v); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 14 && t->wire_type == WireType::I32) {
                     const auto v = r.read_fixed32();
                     if (!v) {
                         break;
                     }
                     s += *v;
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 14) {
                     s += float_bits(r.get_float());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("fixed32-float", ByteView(buf), arms);
}

int scenario_fixed64() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 15, 1);
        put_fixed64(buf, double_bits(static_cast<double>(i) * -2.25));
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::db, double v) { s += double_bits(v); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 15 && t->wire_type == WireType::I64) {
                     const auto v = r.read_fixed64();
                     if (!v) {
                         break;
                     }
                     s += *v;
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 15) {
                     s += double_bits(r.get_double());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("fixed64-double", ByteView(buf), arms);
}

int scenario_string() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 12, 2);
        put_varint(buf, 8);
        buf.append("abcdefgh", 8);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::s, std::string_view v) { s += v.size(); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 12 && t->wire_type == WireType::Len) {
                     const auto v = r.read_length_delimited();
                     if (!v) {
                         break;
                     }
                     s += v->size();
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 12) {
                     s += r.get_view().size();
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("len-string", ByteView(buf), arms);
}

int scenario_packed() {
    std::string payload;
    for (int i = 0; i < kN; ++i) {
        put_varint(payload, static_cast<std::uint64_t>(i) & 0x3FFFFU);
    }
    std::string buf;
    put_tag(buf, 17, 2);
    put_varint(buf, payload.size());
    buf += payload;
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::packed_nums, std::int32_t v) {
                     s += static_cast<std::uint32_t>(v);
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 17 && t->wire_type == WireType::Len) {
                     const auto span = r.read_length_delimited();
                     if (!span) {
                         break;
                     }
                     WireReader inner{*span};
                     while (!inner.at_end()) {
                         const auto v = inner.read_varint();
                         if (!v) {
                             break;
                         }
                         s += static_cast<std::uint32_t>(varint_to_int32(*v));
                     }
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 17) {
                     auto packed = r.get_packed_int32();
                     for (auto v : packed) {
                         s += static_cast<std::uint32_t>(v);
                     }
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("packed-int32", ByteView(buf), arms);
}

int scenario_skip_heavy() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 11, 0);  // field 11 (bool), no callback so it gets skipped
        put_varint(buf, static_cast<std::uint64_t>(i) & 1U);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 1 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(*v));
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("skip-heavy", ByteView(buf), arms);
}

int scenario_mixed() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);
        put_varint(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 6, 0);
        put_varint(buf, zigzag64(static_cast<std::int64_t>(i) * ((i & 1) != 0 ? -7 : 7)));
        put_tag(buf, 15, 1);
        put_fixed64(buf, double_bits(static_cast<double>(i) * 0.5));
        put_tag(buf, 12, 2);
        put_varint(buf, 4);
        buf.append("wxyz", 4);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::i32, std::int32_t v) {
                     s += static_cast<std::uint32_t>(v);
                 },
                 [&](p2::stream::Scalars::s64, std::int64_t v) {
                     s += static_cast<std::uint64_t>(v);
                 },
                 [&](p2::stream::Scalars::db, double v) { s += double_bits(v); },
                 [&](p2::stream::Scalars::s, std::string_view v) { s += v.size(); });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 1 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(*v));
                 } else if (t->field_number == 6 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint64_t>(zigzag_decode_64(*v));
                 } else if (t->field_number == 15 && t->wire_type == WireType::I64) {
                     const auto v = r.read_fixed64();
                     if (!v) {
                         break;
                     }
                     s += *v;
                 } else if (t->field_number == 12 && t->wire_type == WireType::Len) {
                     const auto v = r.read_length_delimited();
                     if (!v) {
                         break;
                     }
                     s += v->size();
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 switch (r.tag()) {
                     case 1:
                         s += static_cast<std::uint32_t>(r.get_int32());
                         break;
                     case 6:
                         s += static_cast<std::uint64_t>(r.get_sint64());
                         break;
                     case 15:
                         s += double_bits(r.get_double());
                         break;
                     case 12:
                         s += r.get_view().size();
                         break;
                     default:
                         r.skip();
                         break;
                 }
             }
             return s;
         }},
#endif
    };
    return run("mixed", ByteView(buf), arms);
}

// --- multi-byte tags: field 18 (expanded_nums) -> tag is 2 bytes (0x90 0x01). Real schemas have
// field numbers >15 constantly; the 1-byte-tag micro-scenarios are an unrealistic best case. ------
int scenario_multibyte_tag() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 18, 0);  // (18<<3)|0 = 144 -> 0x90 0x01
        put_varint(buf, static_cast<std::uint64_t>(i) & 0x7FU);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode(
                 [&](p2::stream::Scalars::expanded_nums, std::int32_t v) {
                     s += static_cast<std::uint32_t>(v);
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 18 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(*v));
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 18) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("multibyte-tag", ByteView(buf), arms);
}

// --- nested sub-messages: Container{ repeated Nested items = 5; Nested{ int32 x = 1; } }. Exercises
// the real streaming path -- read a LEN payload, construct a sub-decoder, recurse. -----------------
int scenario_nested() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        std::string nested;
        put_tag(nested, 1, 0);  // Nested::x
        put_varint(nested, static_cast<std::uint64_t>(i) & 0xFFFFU);
        put_tag(buf, 5, 2);  // Container::items (LEN)
        put_varint(buf, nested.size());
        buf += nested;
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Container{b}.decode(
                 [&](p2::stream::Container::items, p2::stream::Container::Nested sub) {
                     (void)sub.decode([&](p2::stream::Container::Nested::x, std::int32_t v) {
                         s += static_cast<std::uint32_t>(v);
                     });
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 5 && t->wire_type == WireType::Len) {
                     const auto span = r.read_length_delimited();
                     if (!span) {
                         break;
                     }
                     WireReader sub{*span};
                     while (!sub.at_end()) {
                         const auto st = sub.read_tag();
                         if (!st) {
                             break;
                         }
                         if (st->field_number == 1 && st->wire_type == WireType::Varint) {
                             const auto v = sub.read_varint();
                             if (!v) {
                                 break;
                             }
                             s += static_cast<std::uint32_t>(varint_to_int32(*v));
                         } else if (!sub.skip(st->wire_type, st->field_number)) {
                             break;
                         }
                     }
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 5) {
                     auto sub = r.get_message();
                     while (sub.next()) {
                         if (sub.tag() == 1) {
                             s += static_cast<std::uint32_t>(sub.get_int32());
                         } else {
                             sub.skip();
                         }
                     }
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("nested-msg", ByteView(buf), arms);
}

// --- groups: WithGroup{ group MyGroup = 1 { int32 a = 2; } }. Exercises the recursive group-decode
// path (SGROUP..EGROUP, scan_group_end). protozero does NOT support groups (it throws on wire type
// 3/4), so this scenario has no protozero arm -- generated vs wire only. -------------------------
int scenario_groups() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 3);  // SGROUP, field 1 -> (1<<3)|3 = 0x0b
        put_tag(buf, 2, 0);  // MyGroup::a
        put_varint(buf, static_cast<std::uint64_t>(i) & 0xFFFFU);
        put_tag(buf, 1, 4);  // EGROUP, field 1 -> (1<<3)|4 = 0x0c
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::WithGroup{b}.decode(
                 [&](p2::stream::WithGroup::mygroup, p2::stream::WithGroup::MyGroup sub) {
                     (void)sub.decode([&](p2::stream::WithGroup::MyGroup::a, std::int32_t v) {
                         s += static_cast<std::uint32_t>(v);
                     });
                 });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 1 && t->wire_type == WireType::SGroup) {
                     const auto body = r.read_group(t->field_number);
                     if (!body) {
                         break;
                     }
                     WireReader sub{*body};
                     while (!sub.at_end()) {
                         const auto st = sub.read_tag();
                         if (!st) {
                             break;
                         }
                         if (st->field_number == 2 && st->wire_type == WireType::Varint) {
                             const auto v = sub.read_varint();
                             if (!v) {
                                 break;
                             }
                             s += static_cast<std::uint32_t>(varint_to_int32(*v));
                         } else if (!sub.skip(st->wire_type, st->field_number)) {
                             break;
                         }
                     }
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
    };
    return run("groups", ByteView(buf), arms);
}

// --- sparse: each record has one handled field (i32) + three unhandled fields of different wire
// types (varint, fixed64, len), so the skip dispatch over wire types is genuinely hot. -----------
int scenario_sparse() {
    std::string buf;
    for (int i = 0; i < kN; ++i) {
        put_tag(buf, 1, 0);  // i32 (handled)
        put_varint(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 2, 0);  // i64 (skipped, varint)
        put_varint(buf, static_cast<std::uint64_t>(i) * 3U);
        put_tag(buf, 8, 1);  // f64 (skipped, fixed64)
        put_fixed64(buf, static_cast<std::uint64_t>(i));
        put_tag(buf, 12, 2);  // s (skipped, len)
        put_varint(buf, 4);
        buf.append("abcd", 4);
    }
    const Arm arms[] = {
        {"generated",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             (void)p2::stream::Scalars{b}.decode([&](p2::stream::Scalars::i32, std::int32_t v) {
                 s += static_cast<std::uint32_t>(v);
             });
             return s;
         }},
        {"wire",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             WireReader r{b};
             while (!r.at_end()) {
                 const auto t = r.read_tag();
                 if (!t) {
                     break;
                 }
                 if (t->field_number == 1 && t->wire_type == WireType::Varint) {
                     const auto v = r.read_varint();
                     if (!v) {
                         break;
                     }
                     s += static_cast<std::uint32_t>(varint_to_int32(*v));
                 } else if (!r.skip(t->wire_type, t->field_number)) {
                     break;
                 }
             }
             return s;
         }},
#ifdef RAPIDPROTO_HAVE_PROTOZERO
        {"protozero",
         [](ByteView b) -> std::uint64_t {
             std::uint64_t s = 0;
             protozero::pbf_reader r{b.data(), b.size()};
             while (r.next()) {
                 if (r.tag() == 1) {
                     s += static_cast<std::uint32_t>(r.get_int32());
                 } else {
                     r.skip();
                 }
             }
             return s;
         }},
#endif
    };
    return run("sparse-skip", ByteView(buf), arms);
}

}  // namespace

int main() {
    const int fd = bench_metric_fd();  // pin + steady-state warmup + open the cycle counter, once
#ifdef RAPIDPROTO_HAVE_PROTOZERO
    std::puts("rapidproto decode bench (generated / wire / protozero). Each arm vs the baseline:");
#else
    std::puts(
        "rapidproto decode bench (generated / wire; protozero ABSENT). Each arm vs baseline:");
#endif
    std::printf(
        "  metric: %s; adaptive, self-pinned. '+X%%' = arm out-throughputs baseline;"
        " verdict SIG=real&>=0.5%% / flat=real&<0.5%% / noise=CI spans 0.\n",
        fd >= 0 ? "wall-clock GB/s + CPU cycles/byte (perf)"
                : "wall-clock GB/s (perf unavailable: kernel.perf_event_paranoid > 2)");
    int bad = 0;
    bad += scenario_varint_1byte();
    bad += scenario_varint_multibyte();
    bad += scenario_zigzag();
    bad += scenario_fixed32();
    bad += scenario_fixed64();
    bad += scenario_string();
    bad += scenario_packed();
    bad += scenario_skip_heavy();
    bad += scenario_mixed();
    bad += scenario_multibyte_tag();
    bad += scenario_nested();
    bad += scenario_groups();
    bad += scenario_sparse();
    if (bad != 0) {
        std::printf("\nFAIL: %d checksum mismatch(es)\n", bad);
        return 1;
    }
    std::puts("\nall checksums agree.");
    return 0;
}
