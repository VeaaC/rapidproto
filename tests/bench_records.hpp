// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The hand-built wire for the `scalar records (dispatch-bound)` scenario, shared by BOTH
// benches. NOT byte-identical to protoc's serialization: proto3 omits zero-valued implicit
// fields this builder writes explicitly (a wire shape protoc parsers accept; the decoded VALUES
// are equal). bench_arena.cpp asserts that value equality at startup by re-serializing a protoc
// parse of these bytes -- before that check the two constructions were an unpinned two-binary
// mirror kept equal by care alone, under a comment that wrongly claimed byte identity.

#include <cstdint>
#include <string>

namespace rpbench {
namespace records_detail {
inline void put_varint(std::string& b, std::uint64_t v) {
    while (v >= 0x80U) {
        b.push_back(static_cast<char>(0x80U | (v & 0x7FU)));
        v >>= 7U;
    }
    b.push_back(static_cast<char>(v));
}
inline void put_tag(std::string& b, std::uint32_t field, std::uint32_t wire) {
    put_varint(b, (static_cast<std::uint64_t>(field) << 3U) | wire);
}
inline std::uint64_t zigzag64(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1U) ^ static_cast<std::uint64_t>(v >> 63);
}
}  // namespace records_detail

inline std::string make_records_wire(int count) {
    const auto sample = [](std::string& r, int base) {
        records_detail::put_tag(r, 1, 0);
        records_detail::put_varint(r, static_cast<std::uint64_t>(base & 63));  // a int32
        records_detail::put_tag(r, 2, 0);
        records_detail::put_varint(r, static_cast<std::uint64_t>((base + 1) & 63));  // b int32
        records_detail::put_tag(r, 3, 0);
        records_detail::put_varint(r, static_cast<std::uint64_t>((base + 2) & 63));  // c int64
        records_detail::put_tag(r, 4, 0);
        records_detail::put_varint(r, records_detail::zigzag64((base + 3) & 63));  // d sint32
        records_detail::put_tag(r, 5, 0);
        records_detail::put_varint(
            r, static_cast<std::uint64_t>((base & 1) != 0 ? 1U : 0U));  // e bool
        records_detail::put_tag(r, 6, 0);
        records_detail::put_varint(r, static_cast<std::uint64_t>((base + 4) & 63));  // f uint32
    };
    std::string buf;
    for (int i = 0; i < count; ++i) {
        std::string rec;
        records_detail::put_tag(rec, 1, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>(i & 63));  // f1 int32
        records_detail::put_tag(rec, 2, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 1) & 63));  // f2
        records_detail::put_tag(rec, 3, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 2) & 63));  // f3
        records_detail::put_tag(rec, 4, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 3) & 63));  // f4 int64
        records_detail::put_tag(rec, 5, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 4) & 63));  // f5 uint32
        records_detail::put_tag(rec, 6, 0);
        records_detail::put_varint(rec, records_detail::zigzag64((i + 5) & 63));  // f6 sint32
        records_detail::put_tag(rec, 7, 0);
        records_detail::put_varint(rec,
                                   static_cast<std::uint64_t>((i & 1) != 0 ? 1U : 0U));  // f7 bool
        records_detail::put_tag(rec, 8, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 6) & 63));  // f8 uint64
        records_detail::put_tag(rec, 9, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 7) & 63));  // f9
        records_detail::put_tag(rec, 10, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 8) & 63));  // f10
        records_detail::put_tag(rec, 11, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 9) & 63));  // f11
        records_detail::put_tag(rec, 12, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 10) & 63));  // f12
        records_detail::put_tag(rec, 13, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 11) & 63));  // f13
        records_detail::put_tag(rec, 14, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 12) & 63));  // f14
        records_detail::put_tag(rec, 15, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 13) & 63));  // f15
        std::string s1;
        sample(s1, i + 20);
        records_detail::put_tag(rec, 16, 2);  // s1 Sample (LEN)
        records_detail::put_varint(rec, s1.size());
        rec += s1;
        std::string s2;
        sample(s2, i + 30);
        records_detail::put_tag(rec, 17, 2);  // s2 Sample (LEN)
        records_detail::put_varint(rec, s2.size());
        rec += s2;
        records_detail::put_tag(rec, 18, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 14) & 63));  // f18
        records_detail::put_tag(rec, 19, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 15) & 63));  // f19
        records_detail::put_tag(rec, 20, 0);
        records_detail::put_varint(rec, static_cast<std::uint64_t>((i + 16) & 63));  // f20
        records_detail::put_tag(buf, 1, 2);  // RecordSet::records (LEN)
        records_detail::put_varint(buf, rec.size());
        buf += rec;
    }
    return buf;
}

}  // namespace rpbench
