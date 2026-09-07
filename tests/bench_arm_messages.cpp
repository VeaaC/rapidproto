// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
//
// The google_message1/2 scenarios' own translation unit (see bench_arm_messages.hpp for why it
// is separate, the checksum convention, and the dataset provenance).

#include "bench_arm_messages.hpp"

#ifdef RAPIDPROTO_BENCH_MESSAGES

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "benchmark_message1_proto2.pb.h"
#include "benchmark_message1_proto2.rp.hpp"
#include "benchmark_message1_proto2.rp.stream.hpp"
#include "benchmark_message2.pb.h"
#include "benchmark_message2.rp.hpp"
#include "benchmark_message2.rp.stream.hpp"

#include "bench_harness.hpp"
#include "bench_upb.hpp"
#include "rapidproto/arena_runtime.hpp"

#ifdef RAPIDPROTO_HAVE_UPB
extern "C" {
extern const unsigned char rp_upb_gm1_desc[];
extern const unsigned rp_upb_gm1_desc_len;
extern const unsigned char rp_upb_gm2_desc[];
extern const unsigned rp_upb_gm2_desc_len;
}
#endif

namespace rpmessages {

namespace am1 = rp::arena::benchmarks::proto2;
namespace sm1 = rp::stream::benchmarks::proto2;
namespace pm = benchmarks::proto2;

inline std::uint64_t bits64(double d) {
    std::uint64_t b = 0;
    std::memcpy(&b, &d, sizeof b);
    return b;
}
inline std::uint64_t fbits(float f) {  // float contributions widen to double, like every walk
    return bits64(static_cast<double>(f));
}

// payload[0] out of the BenchmarkDataset wrapper (field 3, LEN). Empty string on any surprise.
inline std::string load_payload(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string raw = ss.str();
    const auto* p = reinterpret_cast<const unsigned char*>(raw.data());
    std::size_t i = 0;
    const std::size_t n = raw.size();
    auto varint = [&](std::uint64_t& v) {
        v = 0;
        int shift = 0;
        while (i < n) {
            const unsigned char b = p[i++];
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            shift += 7;
            if ((b & 0x80) == 0) {
                return true;
            }
        }
        return false;
    };
    while (i < n) {
        std::uint64_t tag = 0;
        if (!varint(tag)) {
            break;
        }
        const unsigned wt = tag & 7U;
        if (wt == 2) {
            std::uint64_t len = 0;
            if (!varint(len) || i + len > n) {
                break;
            }
            if ((tag >> 3) == 3) {
                return raw.substr(i, len);
            }
            i += len;
        } else if (wt == 0) {
            std::uint64_t skip = 0;
            if (!varint(skip)) {
                break;
            }
        } else {
            break;  // no other wire type appears in these wrappers
        }
    }
    return {};
}

// ── google_message1: full walk, present fields only, all four decoders ───────────────────────

inline std::uint64_t gm1_protoc(const std::string& buf) {
    google::protobuf::Arena arena;
    auto* m = google::protobuf::Arena::CreateMessage<pm::GoogleMessage1>(&arena);
    if (!m->ParseFromString(buf)) {
        return ~std::uint64_t{0};
    }
    std::uint64_t s = 0;
    s += m->field1().size();  // required
    if (m->has_field9()) {
        s += m->field9().size();
    }
    if (m->has_field18()) {
        s += m->field18().size();
    }
    if (m->has_field80()) {
        s += m->field80() ? 1U : 0U;
    }
    if (m->has_field81()) {
        s += m->field81() ? 1U : 0U;
    }
    s += static_cast<std::uint32_t>(m->field2());  // required
    s += static_cast<std::uint32_t>(m->field3());  // required
    if (m->has_field280()) {
        s += static_cast<std::uint32_t>(m->field280());
    }
    if (m->has_field6()) {
        s += static_cast<std::uint32_t>(m->field6());
    }
    if (m->has_field22()) {
        s += static_cast<std::uint64_t>(m->field22());
    }
    if (m->has_field4()) {
        s += m->field4().size();
    }
    for (const auto v : m->field5()) {
        s += v;
    }
    if (m->has_field59()) {
        s += m->field59() ? 1U : 0U;
    }
    if (m->has_field7()) {
        s += m->field7().size();
    }
    if (m->has_field16()) {
        s += static_cast<std::uint32_t>(m->field16());
    }
    if (m->has_field130()) {
        s += static_cast<std::uint32_t>(m->field130());
    }
    if (m->has_field12()) {
        s += m->field12() ? 1U : 0U;
    }
    if (m->has_field17()) {
        s += m->field17() ? 1U : 0U;
    }
    if (m->has_field13()) {
        s += m->field13() ? 1U : 0U;
    }
    if (m->has_field14()) {
        s += m->field14() ? 1U : 0U;
    }
    if (m->has_field104()) {
        s += static_cast<std::uint32_t>(m->field104());
    }
    if (m->has_field100()) {
        s += static_cast<std::uint32_t>(m->field100());
    }
    if (m->has_field101()) {
        s += static_cast<std::uint32_t>(m->field101());
    }
    if (m->has_field102()) {
        s += m->field102().size();
    }
    if (m->has_field103()) {
        s += m->field103().size();
    }
    if (m->has_field29()) {
        s += static_cast<std::uint32_t>(m->field29());
    }
    if (m->has_field30()) {
        s += m->field30() ? 1U : 0U;
    }
    if (m->has_field60()) {
        s += static_cast<std::uint32_t>(m->field60());
    }
    if (m->has_field271()) {
        s += static_cast<std::uint32_t>(m->field271());
    }
    if (m->has_field272()) {
        s += static_cast<std::uint32_t>(m->field272());
    }
    if (m->has_field150()) {
        s += static_cast<std::uint32_t>(m->field150());
    }
    if (m->has_field23()) {
        s += static_cast<std::uint32_t>(m->field23());
    }
    if (m->has_field24()) {
        s += m->field24() ? 1U : 0U;
    }
    if (m->has_field25()) {
        s += static_cast<std::uint32_t>(m->field25());
    }
    if (m->has_field15()) {
        const auto& sub = m->field15();
        if (sub.has_field1()) {
            s += static_cast<std::uint32_t>(sub.field1());
        }
        if (sub.has_field2()) {
            s += static_cast<std::uint32_t>(sub.field2());
        }
        if (sub.has_field3()) {
            s += static_cast<std::uint32_t>(sub.field3());
        }
        if (sub.has_field15()) {
            s += sub.field15().size();
        }
        if (sub.has_field12()) {
            s += sub.field12() ? 1U : 0U;
        }
        if (sub.has_field13()) {
            s += static_cast<std::uint64_t>(sub.field13());
        }
        if (sub.has_field14()) {
            s += static_cast<std::uint64_t>(sub.field14());
        }
        if (sub.has_field16()) {
            s += static_cast<std::uint32_t>(sub.field16());
        }
        if (sub.has_field19()) {
            s += static_cast<std::uint32_t>(sub.field19());
        }
        if (sub.has_field20()) {
            s += sub.field20() ? 1U : 0U;
        }
        if (sub.has_field28()) {
            s += sub.field28() ? 1U : 0U;
        }
        if (sub.has_field21()) {
            s += sub.field21();
        }
        if (sub.has_field22()) {
            s += static_cast<std::uint32_t>(sub.field22());
        }
        if (sub.has_field23()) {
            s += sub.field23() ? 1U : 0U;
        }
        if (sub.has_field206()) {
            s += sub.field206() ? 1U : 0U;
        }
        if (sub.has_field203()) {
            s += sub.field203();
        }
        if (sub.has_field204()) {
            s += static_cast<std::uint32_t>(sub.field204());
        }
        if (sub.has_field205()) {
            s += sub.field205().size();
        }
        if (sub.has_field207()) {
            s += sub.field207();
        }
        if (sub.has_field300()) {
            s += sub.field300();
        }
    }
    if (m->has_field78()) {
        s += m->field78() ? 1U : 0U;
    }
    if (m->has_field67()) {
        s += static_cast<std::uint32_t>(m->field67());
    }
    if (m->has_field68()) {
        s += static_cast<std::uint32_t>(m->field68());
    }
    if (m->has_field128()) {
        s += static_cast<std::uint32_t>(m->field128());
    }
    if (m->has_field129()) {
        s += m->field129().size();
    }
    if (m->has_field131()) {
        s += static_cast<std::uint32_t>(m->field131());
    }
    return s;
}

template <class M1>
std::uint64_t gm1_arena_walk(const M1* m) {
    if (m == nullptr) {
        return ~std::uint64_t{0};
    }
    std::uint64_t s = 0;
    s += m->field1().size();
    if (const auto v_field9 = m->field9()) {
        s += v_field9->size();
    }
    if (const auto v_field18 = m->field18()) {
        s += v_field18->size();
    }
    if (const auto v_field80 = m->field80()) {
        s += *v_field80 ? 1U : 0U;
    }
    if (const auto v_field81 = m->field81()) {
        s += *v_field81 ? 1U : 0U;
    }
    s += static_cast<std::uint32_t>(m->field2());
    s += static_cast<std::uint32_t>(m->field3());
    if (const auto v_field280 = m->field280()) {
        s += static_cast<std::uint32_t>(*v_field280);
    }
    if (const auto v_field6 = m->field6()) {
        s += static_cast<std::uint32_t>(*v_field6);
    }
    if (const auto v_field22 = m->field22()) {
        s += static_cast<std::uint64_t>(*v_field22);
    }
    if (const auto v_field4 = m->field4()) {
        s += v_field4->size();
    }
    for (const auto v : m->field5()) {
        s += v;
    }
    if (const auto v_field59 = m->field59()) {
        s += *v_field59 ? 1U : 0U;
    }
    if (const auto v_field7 = m->field7()) {
        s += v_field7->size();
    }
    if (const auto v_field16 = m->field16()) {
        s += static_cast<std::uint32_t>(*v_field16);
    }
    if (const auto v_field130 = m->field130()) {
        s += static_cast<std::uint32_t>(*v_field130);
    }
    if (const auto v_field12 = m->field12()) {
        s += *v_field12 ? 1U : 0U;
    }
    if (const auto v_field17 = m->field17()) {
        s += *v_field17 ? 1U : 0U;
    }
    if (const auto v_field13 = m->field13()) {
        s += *v_field13 ? 1U : 0U;
    }
    if (const auto v_field14 = m->field14()) {
        s += *v_field14 ? 1U : 0U;
    }
    if (const auto v_field104 = m->field104()) {
        s += static_cast<std::uint32_t>(*v_field104);
    }
    if (const auto v_field100 = m->field100()) {
        s += static_cast<std::uint32_t>(*v_field100);
    }
    if (const auto v_field101 = m->field101()) {
        s += static_cast<std::uint32_t>(*v_field101);
    }
    if (const auto v_field102 = m->field102()) {
        s += v_field102->size();
    }
    if (const auto v_field103 = m->field103()) {
        s += v_field103->size();
    }
    if (const auto v_field29 = m->field29()) {
        s += static_cast<std::uint32_t>(*v_field29);
    }
    if (const auto v_field30 = m->field30()) {
        s += *v_field30 ? 1U : 0U;
    }
    if (const auto v_field60 = m->field60()) {
        s += static_cast<std::uint32_t>(*v_field60);
    }
    if (const auto v_field271 = m->field271()) {
        s += static_cast<std::uint32_t>(*v_field271);
    }
    if (const auto v_field272 = m->field272()) {
        s += static_cast<std::uint32_t>(*v_field272);
    }
    if (const auto v_field150 = m->field150()) {
        s += static_cast<std::uint32_t>(*v_field150);
    }
    if (const auto v_field23 = m->field23()) {
        s += static_cast<std::uint32_t>(*v_field23);
    }
    if (const auto v_field24 = m->field24()) {
        s += *v_field24 ? 1U : 0U;
    }
    if (const auto v_field25 = m->field25()) {
        s += static_cast<std::uint32_t>(*v_field25);
    }
    if (const auto* sub = m->field15()) {
        if (const auto v_field1 = sub->field1()) {
            s += static_cast<std::uint32_t>(*v_field1);
        }
        if (const auto v_field2 = sub->field2()) {
            s += static_cast<std::uint32_t>(*v_field2);
        }
        if (const auto v_field3 = sub->field3()) {
            s += static_cast<std::uint32_t>(*v_field3);
        }
        if (const auto v_field15 = sub->field15()) {
            s += v_field15->size();
        }
        if (const auto v_field12 = sub->field12()) {
            s += *v_field12 ? 1U : 0U;
        }
        if (const auto v_field13 = sub->field13()) {
            s += static_cast<std::uint64_t>(*v_field13);
        }
        if (const auto v_field14 = sub->field14()) {
            s += static_cast<std::uint64_t>(*v_field14);
        }
        if (const auto v_field16 = sub->field16()) {
            s += static_cast<std::uint32_t>(*v_field16);
        }
        if (const auto v_field19 = sub->field19()) {
            s += static_cast<std::uint32_t>(*v_field19);
        }
        if (const auto v_field20 = sub->field20()) {
            s += *v_field20 ? 1U : 0U;
        }
        if (const auto v_field28 = sub->field28()) {
            s += *v_field28 ? 1U : 0U;
        }
        if (const auto v_field21 = sub->field21()) {
            s += *v_field21;
        }
        if (const auto v_field22 = sub->field22()) {
            s += static_cast<std::uint32_t>(*v_field22);
        }
        if (const auto v_field23 = sub->field23()) {
            s += *v_field23 ? 1U : 0U;
        }
        if (const auto v_field206 = sub->field206()) {
            s += *v_field206 ? 1U : 0U;
        }
        if (const auto v_field203 = sub->field203()) {
            s += *v_field203;
        }
        if (const auto v_field204 = sub->field204()) {
            s += static_cast<std::uint32_t>(*v_field204);
        }
        if (const auto v_field205 = sub->field205()) {
            s += v_field205->size();
        }
        if (const auto v_field207 = sub->field207()) {
            s += *v_field207;
        }
        if (const auto v_field300 = sub->field300()) {
            s += *v_field300;
        }
    }
    if (const auto v_field78 = m->field78()) {
        s += *v_field78 ? 1U : 0U;
    }
    if (const auto v_field67 = m->field67()) {
        s += static_cast<std::uint32_t>(*v_field67);
    }
    if (const auto v_field68 = m->field68()) {
        s += static_cast<std::uint32_t>(*v_field68);
    }
    if (const auto v_field128 = m->field128()) {
        s += static_cast<std::uint32_t>(*v_field128);
    }
    if (const auto v_field129 = m->field129()) {
        s += v_field129->size();
    }
    if (const auto v_field131 = m->field131()) {
        s += static_cast<std::uint32_t>(*v_field131);
    }
    return s;
}

inline std::uint64_t gm1_stream(rapidproto::ByteView buf) {
    using M = sm1::GoogleMessage1;
    using Sub = sm1::GoogleMessage1SubMessage;
    std::uint64_t s = 0;
    const M m{buf};
    const rapidproto::DecodeStatus st = m.decode(
        [&](M::field1, std::string_view v) { s += v.size(); },
        [&](M::field9, std::string_view v) { s += v.size(); },
        [&](M::field18, std::string_view v) { s += v.size(); },
        [&](M::field80, bool v) { s += v ? 1U : 0U; },
        [&](M::field81, bool v) { s += v ? 1U : 0U; },
        [&](M::field2, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field3, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field280, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field6, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field22, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
        [&](M::field4, std::string_view v) { s += v.size(); },
        [&](M::field5, std::uint64_t v) { s += v; }, [&](M::field59, bool v) { s += v ? 1U : 0U; },
        [&](M::field7, std::string_view v) { s += v.size(); },
        [&](M::field16, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field130, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field12, bool v) { s += v ? 1U : 0U; },
        [&](M::field17, bool v) { s += v ? 1U : 0U; },
        [&](M::field13, bool v) { s += v ? 1U : 0U; },
        [&](M::field14, bool v) { s += v ? 1U : 0U; },
        [&](M::field104, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field100, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field101, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field102, std::string_view v) { s += v.size(); },
        [&](M::field103, std::string_view v) { s += v.size(); },
        [&](M::field29, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field30, bool v) { s += v ? 1U : 0U; },
        [&](M::field60, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field271, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field272, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field150, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field23, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field24, bool v) { s += v ? 1U : 0U; },
        [&](M::field25, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field15, Sub sub) -> rapidproto::DecodeStatus {
            return sub.decode(
                [&](Sub::field1, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Sub::field2, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Sub::field3, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Sub::field15, std::string_view v) { s += v.size(); },
                [&](Sub::field12, bool v) { s += v ? 1U : 0U; },
                [&](Sub::field13, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
                [&](Sub::field14, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
                [&](Sub::field16, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Sub::field19, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Sub::field20, bool v) { s += v ? 1U : 0U; },
                [&](Sub::field28, bool v) { s += v ? 1U : 0U; },
                [&](Sub::field21, std::uint64_t v) { s += v; },
                [&](Sub::field22, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Sub::field23, bool v) { s += v ? 1U : 0U; },
                [&](Sub::field206, bool v) { s += v ? 1U : 0U; },
                [&](Sub::field203, std::uint32_t v) { s += v; },
                [&](Sub::field204, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                [&](Sub::field205, std::string_view v) { s += v.size(); },
                [&](Sub::field207, std::uint64_t v) { s += v; },
                [&](Sub::field300, std::uint64_t v) { s += v; });
        },
        [&](M::field78, bool v) { s += v ? 1U : 0U; },
        [&](M::field67, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field68, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field128, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field129, std::string_view v) { s += v.size(); },
        [&](M::field131, std::int32_t v) { s += static_cast<std::uint32_t>(v); });
    return st.ok() ? s : 0;
}

// ── google_message2: full walk, present fields only ──────────────────────────────────────────

inline std::uint64_t gm2_protoc(const std::string& buf) {
    google::protobuf::Arena arena;
    auto* m = google::protobuf::Arena::CreateMessage<pm::GoogleMessage2>(&arena);
    if (!m->ParseFromString(buf)) {
        return ~std::uint64_t{0};
    }
    std::uint64_t s = 0;
    if (m->has_field1()) {
        s += m->field1().size();
    }
    if (m->has_field3()) {
        s += static_cast<std::uint64_t>(m->field3());
    }
    if (m->has_field4()) {
        s += static_cast<std::uint64_t>(m->field4());
    }
    if (m->has_field30()) {
        s += static_cast<std::uint64_t>(m->field30());
    }
    if (m->has_field75()) {
        s += m->field75() ? 1U : 0U;
    }
    if (m->has_field6()) {
        s += m->field6().size();
    }
    if (m->has_field2()) {
        s += m->field2().size();
    }
    if (m->has_field21()) {
        s += static_cast<std::uint32_t>(m->field21());
    }
    if (m->has_field71()) {
        s += static_cast<std::uint32_t>(m->field71());
    }
    if (m->has_field25()) {
        s += fbits(m->field25());
    }
    if (m->has_field109()) {
        s += static_cast<std::uint32_t>(m->field109());
    }
    if (m->has_field210()) {
        s += static_cast<std::uint32_t>(m->field210());
    }
    if (m->has_field211()) {
        s += static_cast<std::uint32_t>(m->field211());
    }
    if (m->has_field212()) {
        s += static_cast<std::uint32_t>(m->field212());
    }
    if (m->has_field213()) {
        s += static_cast<std::uint32_t>(m->field213());
    }
    if (m->has_field216()) {
        s += static_cast<std::uint32_t>(m->field216());
    }
    if (m->has_field217()) {
        s += static_cast<std::uint32_t>(m->field217());
    }
    if (m->has_field218()) {
        s += static_cast<std::uint32_t>(m->field218());
    }
    if (m->has_field220()) {
        s += static_cast<std::uint32_t>(m->field220());
    }
    if (m->has_field221()) {
        s += static_cast<std::uint32_t>(m->field221());
    }
    if (m->has_field222()) {
        s += fbits(m->field222());
    }
    if (m->has_field63()) {
        s += static_cast<std::uint32_t>(m->field63());
    }
    for (const auto& g : m->group1()) {
        s += fbits(g.field11());  // required
        if (g.has_field26()) {
            s += fbits(g.field26());
        }
        if (g.has_field12()) {
            s += g.field12().size();
        }
        if (g.has_field13()) {
            s += g.field13().size();
        }
        for (const auto& v : g.field14()) {
            s += v.size();
        }
        s += g.field15();  // required
        if (g.has_field5()) {
            s += static_cast<std::uint32_t>(g.field5());
        }
        if (g.has_field27()) {
            s += g.field27().size();
        }
        if (g.has_field28()) {
            s += static_cast<std::uint32_t>(g.field28());
        }
        if (g.has_field29()) {
            s += g.field29().size();
        }
        if (g.has_field16()) {
            s += g.field16().size();
        }
        for (const auto& v : g.field22()) {
            s += v.size();
        }
        for (const auto v : g.field73()) {
            s += static_cast<std::uint32_t>(v);
        }
        if (g.has_field20()) {
            s += static_cast<std::uint32_t>(g.field20());
        }
        if (g.has_field24()) {
            s += g.field24().size();
        }
        if (g.has_field31()) {
            const auto& gm = g.field31();
            if (gm.has_field1()) {
                s += fbits(gm.field1());
            }
            if (gm.has_field2()) {
                s += fbits(gm.field2());
            }
            if (gm.has_field3()) {
                s += fbits(gm.field3());
            }
            if (gm.has_field4()) {
                s += gm.field4() ? 1U : 0U;
            }
            if (gm.has_field5()) {
                s += gm.field5() ? 1U : 0U;
            }
            if (gm.has_field6()) {
                s += gm.field6() ? 1U : 0U;
            }
            if (gm.has_field7()) {
                s += gm.field7() ? 1U : 0U;
            }
            if (gm.has_field8()) {
                s += fbits(gm.field8());
            }
            if (gm.has_field9()) {
                s += gm.field9() ? 1U : 0U;
            }
            if (gm.has_field10()) {
                s += fbits(gm.field10());
            }
            if (gm.has_field11()) {
                s += static_cast<std::uint64_t>(gm.field11());
            }
        }
    }
    for (const auto& v : m->field128()) {
        s += v.size();
    }
    if (m->has_field131()) {
        s += static_cast<std::uint64_t>(m->field131());
    }
    for (const auto& v : m->field127()) {
        s += v.size();
    }
    if (m->has_field129()) {
        s += static_cast<std::uint32_t>(m->field129());
    }
    for (const auto v : m->field130()) {
        s += static_cast<std::uint64_t>(v);
    }
    if (m->has_field205()) {
        s += m->field205() ? 1U : 0U;
    }
    if (m->has_field206()) {
        s += m->field206() ? 1U : 0U;
    }
    return s;
}

template <class M2>
std::uint64_t gm2_arena_walk(const M2* m) {
    if (m == nullptr) {
        return ~std::uint64_t{0};
    }
    std::uint64_t s = 0;
    if (const auto v_field1 = m->field1()) {
        s += v_field1->size();
    }
    if (const auto v_field3 = m->field3()) {
        s += static_cast<std::uint64_t>(*v_field3);
    }
    if (const auto v_field4 = m->field4()) {
        s += static_cast<std::uint64_t>(*v_field4);
    }
    if (const auto v_field30 = m->field30()) {
        s += static_cast<std::uint64_t>(*v_field30);
    }
    if (const auto v_field75 = m->field75()) {
        s += *v_field75 ? 1U : 0U;
    }
    if (const auto v_field6 = m->field6()) {
        s += v_field6->size();
    }
    if (const auto v_field2 = m->field2()) {
        s += v_field2->size();
    }
    if (const auto v_field21 = m->field21()) {
        s += static_cast<std::uint32_t>(*v_field21);
    }
    if (const auto v_field71 = m->field71()) {
        s += static_cast<std::uint32_t>(*v_field71);
    }
    if (const auto v_field25 = m->field25()) {
        s += fbits(*v_field25);
    }
    if (const auto v_field109 = m->field109()) {
        s += static_cast<std::uint32_t>(*v_field109);
    }
    if (const auto v_field210 = m->field210()) {
        s += static_cast<std::uint32_t>(*v_field210);
    }
    if (const auto v_field211 = m->field211()) {
        s += static_cast<std::uint32_t>(*v_field211);
    }
    if (const auto v_field212 = m->field212()) {
        s += static_cast<std::uint32_t>(*v_field212);
    }
    if (const auto v_field213 = m->field213()) {
        s += static_cast<std::uint32_t>(*v_field213);
    }
    if (const auto v_field216 = m->field216()) {
        s += static_cast<std::uint32_t>(*v_field216);
    }
    if (const auto v_field217 = m->field217()) {
        s += static_cast<std::uint32_t>(*v_field217);
    }
    if (const auto v_field218 = m->field218()) {
        s += static_cast<std::uint32_t>(*v_field218);
    }
    if (const auto v_field220 = m->field220()) {
        s += static_cast<std::uint32_t>(*v_field220);
    }
    if (const auto v_field221 = m->field221()) {
        s += static_cast<std::uint32_t>(*v_field221);
    }
    if (const auto v_field222 = m->field222()) {
        s += fbits(*v_field222);
    }
    if (const auto v_field63 = m->field63()) {
        s += static_cast<std::uint32_t>(*v_field63);
    }
    for (const auto& g : m->group1()) {
        s += fbits(g.field11());
        if (const auto v_field26 = g.field26()) {
            s += fbits(*v_field26);
        }
        if (const auto v_field12 = g.field12()) {
            s += v_field12->size();
        }
        if (const auto v_field13 = g.field13()) {
            s += v_field13->size();
        }
        for (const auto v : g.field14()) {
            s += v.size();
        }
        s += g.field15();
        if (const auto v_field5 = g.field5()) {
            s += static_cast<std::uint32_t>(*v_field5);
        }
        if (const auto v_field27 = g.field27()) {
            s += v_field27->size();
        }
        if (const auto v_field28 = g.field28()) {
            s += static_cast<std::uint32_t>(*v_field28);
        }
        if (const auto v_field29 = g.field29()) {
            s += v_field29->size();
        }
        if (const auto v_field16 = g.field16()) {
            s += v_field16->size();
        }
        for (const auto v : g.field22()) {
            s += v.size();
        }
        for (const auto v : g.field73()) {
            s += static_cast<std::uint32_t>(v);
        }
        if (const auto v_field20 = g.field20()) {
            s += static_cast<std::uint32_t>(*v_field20);
        }
        if (const auto v_field24 = g.field24()) {
            s += v_field24->size();
        }
        if (const auto* gm = g.field31()) {
            if (const auto v_field1 = gm->field1()) {
                s += fbits(*v_field1);
            }
            if (const auto v_field2 = gm->field2()) {
                s += fbits(*v_field2);
            }
            if (const auto v_field3 = gm->field3()) {
                s += fbits(*v_field3);
            }
            if (const auto v_field4 = gm->field4()) {
                s += *v_field4 ? 1U : 0U;
            }
            if (const auto v_field5 = gm->field5()) {
                s += *v_field5 ? 1U : 0U;
            }
            if (const auto v_field6 = gm->field6()) {
                s += *v_field6 ? 1U : 0U;
            }
            if (const auto v_field7 = gm->field7()) {
                s += *v_field7 ? 1U : 0U;
            }
            if (const auto v_field8 = gm->field8()) {
                s += fbits(*v_field8);
            }
            if (const auto v_field9 = gm->field9()) {
                s += *v_field9 ? 1U : 0U;
            }
            if (const auto v_field10 = gm->field10()) {
                s += fbits(*v_field10);
            }
            if (const auto v_field11 = gm->field11()) {
                s += static_cast<std::uint64_t>(*v_field11);
            }
        }
    }
    for (const auto v : m->field128()) {
        s += v.size();
    }
    if (const auto v_field131 = m->field131()) {
        s += static_cast<std::uint64_t>(*v_field131);
    }
    for (const auto v : m->field127()) {
        s += v.size();
    }
    if (const auto v_field129 = m->field129()) {
        s += static_cast<std::uint32_t>(*v_field129);
    }
    for (const auto v : m->field130()) {
        s += static_cast<std::uint64_t>(v);
    }
    if (const auto v_field205 = m->field205()) {
        s += *v_field205 ? 1U : 0U;
    }
    if (const auto v_field206 = m->field206()) {
        s += *v_field206 ? 1U : 0U;
    }
    return s;
}

inline std::uint64_t gm2_stream(rapidproto::ByteView buf) {
    using M = sm1::GoogleMessage2;
    using G = sm1::GoogleMessage2::Group1;
    using GM = sm1::GoogleMessage2GroupedMessage;
    std::uint64_t s = 0;
    const M m{buf};
    const rapidproto::DecodeStatus st = m.decode(
        [&](M::field1, std::string_view v) { s += v.size(); },
        [&](M::field3, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
        [&](M::field4, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
        [&](M::field30, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
        [&](M::field75, bool v) { s += v ? 1U : 0U; },
        [&](M::field6, std::string_view v) { s += v.size(); },
        [&](M::field2, std::string_view v) { s += v.size(); },
        [&](M::field21, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field71, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field25, float v) { s += fbits(v); },
        [&](M::field109, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field210, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field211, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field212, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field213, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field216, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field217, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field218, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field220, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field221, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field222, float v) { s += fbits(v); },
        [&](M::field63, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::group1, G g) -> rapidproto::DecodeStatus {
            return g.decode([&](G::field11, float v) { s += fbits(v); },
                            [&](G::field26, float v) { s += fbits(v); },
                            [&](G::field12, std::string_view v) { s += v.size(); },
                            [&](G::field13, std::string_view v) { s += v.size(); },
                            [&](G::field14, std::string_view v) { s += v.size(); },
                            [&](G::field15, std::uint64_t v) { s += v; },
                            [&](G::field5, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                            [&](G::field27, std::string_view v) { s += v.size(); },
                            [&](G::field28, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                            [&](G::field29, std::string_view v) { s += v.size(); },
                            [&](G::field16, std::string_view v) { s += v.size(); },
                            [&](G::field22, std::string_view v) { s += v.size(); },
                            [&](G::field73, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                            [&](G::field20, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
                            [&](G::field24, std::string_view v) { s += v.size(); },
                            [&](G::field31, GM gm) -> rapidproto::DecodeStatus {
                                return gm.decode([&](GM::field1, float v) { s += fbits(v); },
                                                 [&](GM::field2, float v) { s += fbits(v); },
                                                 [&](GM::field3, float v) { s += fbits(v); },
                                                 [&](GM::field4, bool v) { s += v ? 1U : 0U; },
                                                 [&](GM::field5, bool v) { s += v ? 1U : 0U; },
                                                 [&](GM::field6, bool v) { s += v ? 1U : 0U; },
                                                 [&](GM::field7, bool v) { s += v ? 1U : 0U; },
                                                 [&](GM::field8, float v) { s += fbits(v); },
                                                 [&](GM::field9, bool v) { s += v ? 1U : 0U; },
                                                 [&](GM::field10, float v) { s += fbits(v); },
                                                 [&](GM::field11, std::int64_t v) {
                                                     s += static_cast<std::uint64_t>(v);
                                                 });
                            });
        },
        [&](M::field128, std::string_view v) { s += v.size(); },
        [&](M::field131, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
        [&](M::field127, std::string_view v) { s += v.size(); },
        [&](M::field129, std::int32_t v) { s += static_cast<std::uint32_t>(v); },
        [&](M::field130, std::int64_t v) { s += static_cast<std::uint64_t>(v); },
        [&](M::field205, bool v) { s += v ? 1U : 0U; },
        [&](M::field206, bool v) { s += v ? 1U : 0U; });
    return st.ok() ? s : 0;
}

}  // namespace rpmessages
#include <cstdio>

namespace rpmessages {
namespace {

// One scenario: cross-validate every decoder's checksum on `buf`, then measure the arms.
// `name` follows the harness's scenario naming; upb joins when its descriptors loaded.
template <class ArenaMsg, class ProtocFn, class ArenaWalk, class StreamFn>
int scenario(const char* name, const std::string& buf, ProtocFn protoc_sum, ArenaWalk arena_walk,
             StreamFn stream_sum, const char* upb_name) {
    const rapidproto::ByteView view(buf);

    rapidproto::Arena setup_arena;
    const ArenaMsg* doc = ArenaMsg::decode(view, setup_arena);
    const std::uint64_t c_arena = arena_walk(doc);
    const std::uint64_t c_protoc = protoc_sum(buf);
    const std::uint64_t c_stream = stream_sum(view);
    bool mismatch = c_arena != c_protoc || c_arena != c_stream;
#ifdef RAPIDPROTO_HAVE_UPB
    rpupb::Found upb_msg = rpupb::find_message(upb_name);
    std::uint64_t c_upb = 0;
    if (upb_msg.table != nullptr) {
        upb_Arena* ua = upb_Arena_New();
        upb_Message* um = upb_Message_New(upb_msg.table, ua);
        if (upb_Decode(buf.data(), buf.size(), um, upb_msg.table, nullptr, 0, ua) !=
            kUpb_DecodeStatus_Ok) {
            mismatch = true;
        } else {
            c_upb = rpupb::message_sum(um, upb_msg.def);
            mismatch = mismatch || c_arena != c_upb;
        }
        upb_Arena_Free(ua);
    }
#else
    (void)upb_name;
#endif
    if (mismatch) {
        std::fprintf(stderr, "CHECKSUM MISMATCH (%s): arena=%llu protoc=%llu stream=%llu\n", name,
                     static_cast<unsigned long long>(c_arena),
                     static_cast<unsigned long long>(c_protoc),
                     static_cast<unsigned long long>(c_stream));
        return -1;
    }

    rapidproto::Arena warm;
    std::vector<rpbench::Arm> arms = {
        {"protoc", [&]() { return protoc_sum(buf); }},
        {"arena-cold",
         [&]() {
             rapidproto::Arena a;
             return arena_walk(ArenaMsg::decode(view, a));
         }},
        {"arena-warm",
         [&]() {
             warm.reset();
             return arena_walk(ArenaMsg::decode(view, warm));
         }},
        {"streamgen", [&]() { return stream_sum(view); }},
    };
#ifdef RAPIDPROTO_HAVE_UPB
    if (upb_msg.table != nullptr) {
        // Pure decode returning the validated constant, exactly like the Dataset upb arm (the
        // reflective walk would bill upb for interpreter overhead no real consumer pays).
        const upb_MiniTable* table = upb_msg.table;
        arms.push_back({"upb", [table, c_upb, &buf]() {
                            upb_Arena* a = upb_Arena_New();
                            upb_Message* um = upb_Message_New(table, a);
                            const bool ok = upb_Decode(buf.data(), buf.size(), um, table, nullptr,
                                                       0, a) == kUpb_DecodeStatus_Ok;
                            upb_Arena_Free(a);
                            return ok ? c_upb : ~std::uint64_t{0};
                        }});
    }
#endif
    return rpbench::run(name, static_cast<double>(buf.size()), arms);
}

}  // namespace

int run_arm() {
    const std::string gm1 = load_payload(RAPIDPROTO_GM1_DATASET);
    const std::string gm2 = load_payload(RAPIDPROTO_GM2_DATASET);
    if (gm1.empty() || gm2.empty()) {
        std::fprintf(stderr,
                     "google_message scenarios: dataset payloads not readable "
                     "(corpus moved?)\n");
        return -1;
    }
#ifdef RAPIDPROTO_HAVE_UPB
    // Best effort: a failed load degrades to upb-less scenarios (find_message then fails and
    // the arm is skipped), consistent with the Dataset arm's degrade-not-abort rule.
    (void)rpupb::add_descriptor_set(rp_upb_gm1_desc, rp_upb_gm1_desc_len, "gm1.desc");
    (void)rpupb::add_descriptor_set(rp_upb_gm2_desc, rp_upb_gm2_desc_len, "gm2.desc");
#endif
    int bad = 0;
    const int r1 = scenario<am1::GoogleMessage1>("google_message1", gm1, gm1_protoc,
                                                 gm1_arena_walk<am1::GoogleMessage1>, gm1_stream,
                                                 "benchmarks.proto2.GoogleMessage1");
    if (r1 < 0) {
        return -1;
    }
    bad += r1;
    const int r2 = scenario<am1::GoogleMessage2>("google_message2", gm2, gm2_protoc,
                                                 gm2_arena_walk<am1::GoogleMessage2>, gm2_stream,
                                                 "benchmarks.proto2.GoogleMessage2");
    if (r2 < 0) {
        return -1;
    }
    bad += r2;
    return bad;
}

}  // namespace rpmessages

#endif  // RAPIDPROTO_BENCH_MESSAGES
