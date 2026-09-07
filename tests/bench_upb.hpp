// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The arena bench's upb baseline arm (roadmap 3.4): upb is the fastest widely-known C protobuf
// parser, so it is the honest yardstick for a materializing decode. Compiled from the corpus's
// pinned protobuf checkout (tests/fetch_corpus.py) -- dev-only, never vendored, protozero's
// model plus compilation; CMakeLists gates everything on RAPIDPROTO_HAVE_UPB.
//
// No upb codegen plugin anywhere: the message schema reaches upb as an embedded
// FileDescriptorSet (protoc --descriptor_set_out at build time, cmake/embed_binary.cmake), a
// upb_DefPool turns it into a MiniTable at startup, and upb_Decode runs off that. Building the
// two codegen plugins would mean building protobuf's compiler from source; the runtime route
// needs only upb's C files plus its pre-generated descriptor bootstrap
// (upb/reflection/cmake/ -- shipped precisely so reflection works without codegen).
//
// What the TIMED arm measures, and the deliberate asymmetry: every other arm's lambda decodes
// AND walks the tree for its checksum; upb's walk would be REFLECTIVE (field-by-name lookups --
// the generated accessors the other arms use don't exist without the plugin), which would bill
// upb for interpreter overhead no real consumer pays. So the reflection walk runs ONCE at
// startup -- it must equal protoc's checksum, same cross-decoder bar as every arm -- and the
// timed lambda is pure decode returning that validated constant. That under-counts upb's total
// by the walk the others include, i.e. it flatters the BASELINE, which is the conservative
// direction for RapidProto's own claims.
//
// The configuration is VALIDATED, not assumed: on google_message1 (the published-numbers shape,
// no maps) this same runtime-MiniTable + fasttable setup measures 2.16x protoc (2026-09-07,
// quiesced box) -- squarely in upb's published 2-3x band -- so a weak upb showing on the
// map/string-heavy Dataset is upb's genuine shape behavior, not a mis-setup. Plugin-generated
// tables might still buy upb a little; docs/benchmarks.md states this beside the numbers.

#ifdef RAPIDPROTO_HAVE_UPB

#include <cstdint>
#include <cstring>

#include "upb/base/descriptor_constants.h"
#include "upb/mem/arena.h"
#include "upb/message/array.h"
#include "upb/message/map.h"
#include "upb/message/message.h"
#include "upb/reflection/def.h"
#include "upb/reflection/message.h"
#include "upb/wire/decode.h"

// The pre-generated bootstrap (upb/reflection/cmake/): FileDescriptorProto's own upb accessors,
// shipped in-tree so reflection works without any codegen plugin.
#include "google/protobuf/descriptor.upb.h"

#include "rapidproto/runtime.hpp"  // ByteView

// cmake/embed_binary.cmake's output: bench.proto's serialized FileDescriptorSet.
extern "C" {
extern const unsigned char rp_upb_bench_desc[];
extern const unsigned rp_upb_bench_desc_len;
}

namespace rpupb {

// Owned for the process lifetime; set up once by init(). Not RAII -- a bench main has no
// teardown worth modeling, and upb's def pool must outlive every MiniTable pointer taken.
struct State {
    upb_DefPool* pool = nullptr;
    const upb_MessageDef* dataset_def = nullptr;
    const upb_MiniTable* dataset_table = nullptr;
};

inline State& state() {
    static State s;
    return s;
}

// Load the embedded descriptor set and resolve bench.Dataset. False (with the reason on
// stderr) rather than abort: the bench reports the arm as unavailable and measures the rest.
inline bool init() {
    State& s = state();
    s.pool = upb_DefPool_New();
    upb_Arena* tmp = upb_Arena_New();
    // The embedded bytes are a FileDescriptorSet: one length-prefixed FileDescriptorProto per
    // `file` entry (field 1). upb_DefPool_AddFile wants the FileDescriptorProto, not the Set
    // envelope, so strip the tag + length by hand -- bench.proto is standalone, exactly one
    // entry, payload running to the end. Tag 0x0A = (1 << 3) | LEN.
    const unsigned char* p = rp_upb_bench_desc;
    unsigned len = rp_upb_bench_desc_len;
    if (len < 2 || p[0] != 0x0A) {
        std::fprintf(stderr, "upb arm: embedded descriptor set has unexpected framing\n");
        upb_Arena_Free(tmp);
        return false;
    }
    // Varint length after the tag.
    std::uint32_t payload = 0;
    unsigned at = 1;
    int shift = 0;
    while (at < len) {
        const unsigned char b = p[at++];
        payload |= static_cast<std::uint32_t>(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) {
            break;
        }
    }
    if (at + payload != len) {
        std::fprintf(stderr, "upb arm: descriptor framing mismatch (%u + %u != %u)\n", at, payload,
                     len);
        upb_Arena_Free(tmp);
        return false;
    }
    upb_StringView sv;
    sv.data = reinterpret_cast<const char*>(p + at);
    sv.size = payload;
    google_protobuf_FileDescriptorProto* fdp =
        google_protobuf_FileDescriptorProto_parse(sv.data, sv.size, tmp);
    if (fdp == nullptr) {
        std::fprintf(stderr, "upb arm: FileDescriptorProto parse failed\n");
        upb_Arena_Free(tmp);
        return false;
    }
    upb_Status status;
    upb_Status_Clear(&status);
    if (upb_DefPool_AddFile(s.pool, fdp, &status) == nullptr) {
        std::fprintf(stderr, "upb arm: AddFile: %s\n", upb_Status_ErrorMessage(&status));
        upb_Arena_Free(tmp);
        return false;
    }
    upb_Arena_Free(tmp);
    s.dataset_def = upb_DefPool_FindMessageByName(s.pool, "bench.Dataset");
    if (s.dataset_def == nullptr) {
        std::fprintf(stderr, "upb arm: bench.Dataset not found in the added file\n");
        return false;
    }
    s.dataset_table = upb_MessageDef_MiniTable(s.dataset_def);
    return s.dataset_table != nullptr;
}

// ── the one-time reflection checksum (validation only, never timed) ──────────────────────────
// The same per-value contribution rule every arm uses (see bench_arena.cpp's checksums), walked
// generically: strings/bytes add their size, bools 1/0, floats their bit pattern, integers their
// value; messages recurse; maps add per-entry key+value contributions.

inline std::uint64_t value_contrib(upb_CType ct, upb_MessageValue v);

inline std::uint64_t message_sum(const upb_Message* m, const upb_MessageDef* md) {
    std::uint64_t s = 0;
    const int n = upb_MessageDef_FieldCount(md);
    for (int i = 0; i < n; ++i) {
        const upb_FieldDef* f = upb_MessageDef_Field(md, i);
        if (!upb_Message_HasFieldByDef(m, f) && !upb_FieldDef_IsRepeated(f)) {
            // Implicit-presence scalars: read anyway (defaults contribute 0 to every rule
            // below except bool false -> 0, so absent == default == no contribution).
            if (upb_FieldDef_IsSubMessage(f)) {
                continue;
            }
        }
        const upb_MessageValue v = upb_Message_GetFieldByDef(m, f);
        if (upb_FieldDef_IsMap(f)) {
            const upb_Map* map = v.map_val;
            if (map == nullptr) {
                continue;
            }
            const upb_MessageDef* entry = upb_FieldDef_MessageSubDef(f);
            const upb_FieldDef* kf = upb_MessageDef_Field(entry, 0);
            const upb_FieldDef* vf = upb_MessageDef_Field(entry, 1);
            std::size_t it = kUpb_Map_Begin;
            upb_MessageValue mk;
            upb_MessageValue mv;
            while (upb_Map_Next(map, &mk, &mv, &it)) {
                s += value_contrib(upb_FieldDef_CType(kf), mk);
                if (upb_FieldDef_IsSubMessage(vf)) {
                    s += message_sum(mv.msg_val, upb_FieldDef_MessageSubDef(vf));
                } else {
                    s += value_contrib(upb_FieldDef_CType(vf), mv);
                }
            }
        } else if (upb_FieldDef_IsRepeated(f)) {
            const upb_Array* arr = v.array_val;
            if (arr == nullptr) {
                continue;
            }
            const std::size_t count = upb_Array_Size(arr);
            for (std::size_t j = 0; j < count; ++j) {
                const upb_MessageValue e = upb_Array_Get(arr, j);
                if (upb_FieldDef_IsSubMessage(f)) {
                    s += message_sum(e.msg_val, upb_FieldDef_MessageSubDef(f));
                } else {
                    s += value_contrib(upb_FieldDef_CType(f), e);
                }
            }
        } else if (upb_FieldDef_IsSubMessage(f)) {
            if (v.msg_val != nullptr) {
                s += message_sum(v.msg_val, upb_FieldDef_MessageSubDef(f));
            }
        } else {
            s += value_contrib(upb_FieldDef_CType(f), v);
        }
    }
    return s;
}

inline std::uint64_t value_contrib(upb_CType ct, upb_MessageValue v) {
    switch (ct) {
        case kUpb_CType_String:
        case kUpb_CType_Bytes:
            return v.str_val.size;
        case kUpb_CType_Bool:
            return v.bool_val ? 1U : 0U;
        case kUpb_CType_Double: {
            std::uint64_t b = 0;
            std::memcpy(&b, &v.double_val, sizeof b);
            return b;
        }
        case kUpb_CType_Float: {
            const double widened = v.float_val;
            std::uint64_t b = 0;
            std::memcpy(&b, &widened, sizeof b);
            return b;
        }
        case kUpb_CType_Int32:
        case kUpb_CType_Enum:
            return static_cast<std::uint32_t>(v.int32_val);
        case kUpb_CType_UInt32:
            return v.uint32_val;
        case kUpb_CType_Int64:
            return static_cast<std::uint64_t>(v.int64_val);
        case kUpb_CType_UInt64:
            return v.uint64_val;
        default:
            return 0;
    }
}

// Decode + reflective checksum: the startup validation (must equal protoc's checksum).
inline std::uint64_t checksum_dataset(rapidproto::ByteView buf) {
    State& s = state();
    upb_Arena* arena = upb_Arena_New();
    upb_Message* msg = upb_Message_New(s.dataset_table, arena);
    const upb_DecodeStatus st =
        upb_Decode(buf.data(), buf.size(), msg, s.dataset_table, nullptr, 0, arena);
    const std::uint64_t sum =
        st == kUpb_DecodeStatus_Ok ? message_sum(msg, s.dataset_def) : ~std::uint64_t{0};
    upb_Arena_Free(arena);
    return sum;
}

// The timed arm's body: pure decode into a fresh arena (upb has no arena reset-and-reuse, so
// this matches the arena-cold arm's semantics). Returns whether decode succeeded; the arm
// lambda folds in the pre-validated checksum.
inline bool decode_dataset(rapidproto::ByteView buf) {
    State& s = state();
    upb_Arena* arena = upb_Arena_New();
    upb_Message* msg = upb_Message_New(s.dataset_table, arena);
    const upb_DecodeStatus st =
        upb_Decode(buf.data(), buf.size(), msg, s.dataset_table, nullptr, 0, arena);
    upb_Arena_Free(arena);
    return st == kUpb_DecodeStatus_Ok;
}

}  // namespace rpupb

#endif  // RAPIDPROTO_HAVE_UPB
