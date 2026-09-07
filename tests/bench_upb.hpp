// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The arena bench's upb baseline arm (roadmap 3.4): upb is protobuf upstream's own
// speed-focused C parser (the engine under its dynamic-language runtimes), which makes it the
// honest yardstick for a materializing decode. Compiled from the corpus's
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
// What the TIMED arm measures -- the SAME work as every other arm: decode, then read every
// present field into the checksum. upb pays its walk through upb_MiniTableField handles
// resolved once at init and upb's typed inline accessors -- exactly what its generated .upb.h
// accessors compile to -- so it is neither flattered (walk skipped) nor over-billed
// (reflection's per-field FieldDef dispatch). The REFLECTIVE walk below exists for validation
// only: at startup both upb walks and every other decoder must agree on one checksum, which
// pins the timed walk equivalent before anything is measured.
//
// The configuration is VALIDATED, not assumed: a standalone decode-vs-decode probe (2M
// back-to-back iterations per arm, no checksum walks, aliasing off, quiesced box, 2026-09-07)
// measured this runtime-MiniTable + fasttable setup at 2.16x protoc on google_message1 -- the
// band upstream's own figures put upb in -- so a weak upb showing on the map/string-heavy
// Dataset is upb's genuine shape behavior, not a mis-setup. The IN-TREE rows read lower mainly
// because the timed arm also pays the read-out walk (reading fields OUT of a upb message costs
// more than reading our arena's structs -- see docs/benchmarks.md), plus a smaller
// short-rotated-batch effect on tiny payloads. The probe validates the CONFIGURATION; the
// table is the like-for-like comparison. Plugin-generated tables might still buy upb a little.

#ifdef RAPIDPROTO_HAVE_UPB

#include <cstdint>
#include <cstring>

#include "upb/base/descriptor_constants.h"
#include "upb/mem/arena.h"
#include "upb/message/accessors.h"
#include "upb/message/array.h"
#include "upb/message/map.h"
#include "upb/message/message.h"
#include "upb/mini_table/field.h"
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

// The Dataset walk's pre-resolved accessor handles. upb_MiniTableField pointers looked up ONCE
// at init by name; the timed walk then reads through upb's typed inline accessors -- exactly
// what upb's generated .upb.h accessors compile to, so the walk cost matches what a codegen
// consumer pays, with none of reflection's per-field name lookups.
struct DatasetFields {
    const upb_MiniTableField* name = nullptr;
    const upb_MiniTableField* version = nullptr;
    const upb_MiniTableField* people = nullptr;
    const upb_MiniTableField* p_id = nullptr;
    const upb_MiniTableField* p_name = nullptr;
    const upb_MiniTableField* p_email = nullptr;
    const upb_MiniTableField* p_active = nullptr;
    const upb_MiniTableField* p_score = nullptr;
    const upb_MiniTableField* p_created = nullptr;
    const upb_MiniTableField* p_address = nullptr;
    const upb_MiniTableField* p_tags = nullptr;
    const upb_MiniTableField* p_history = nullptr;
    const upb_MiniTableField* p_attributes = nullptr;
    const upb_MiniTableField* p_counters = nullptr;
    const upb_MiniTableField* a_street = nullptr;
    const upb_MiniTableField* a_city = nullptr;
    const upb_MiniTableField* a_zip = nullptr;
    const upb_MiniTableField* at_key = nullptr;
    const upb_MiniTableField* at_value = nullptr;
};

// Owned for the process lifetime; set up once by init(). Not RAII -- a bench main has no
// teardown worth modeling, and upb's def pool must outlive every MiniTable pointer taken.
struct State {
    upb_DefPool* pool = nullptr;
    const upb_MessageDef* dataset_def = nullptr;
    const upb_MiniTable* dataset_table = nullptr;
    DatasetFields ds;
};

// One field's accessor handle, by name; nullptr (never crashing) when the schema moved.
inline const upb_MiniTableField* field_handle(const upb_MessageDef* md, const char* name) {
    const upb_FieldDef* f = md != nullptr ? upb_MessageDef_FindFieldByName(md, name) : nullptr;
    return f != nullptr ? upb_FieldDef_MiniTable(f) : nullptr;
}

inline State& state() {
    static State s;
    return s;
}

// Add one embedded FileDescriptorSet (cmake/embed_binary.cmake output) to the pool. The bytes
// are the Set ENVELOPE -- one length-prefixed FileDescriptorProto in field 1 (every embedded
// schema here is a single standalone file) -- and upb_DefPool_AddFile wants the bare
// FileDescriptorProto, so strip the tag + varint length by hand. Tag 0x0A = (1 << 3) | LEN.
inline bool add_descriptor_set(const unsigned char* bytes, unsigned len, const char* what) {
    State& s = state();
    if (s.pool == nullptr) {
        s.pool = upb_DefPool_New();
    }
    if (len < 2 || bytes[0] != 0x0A) {
        std::fprintf(stderr, "upb arm: %s: unexpected descriptor framing\n", what);
        return false;
    }
    std::uint32_t payload = 0;
    unsigned at = 1;
    int shift = 0;
    bool terminated = false;
    while (at < len && shift < 32) {  // bounded: a run of continuation bytes must not UB the shift
        const unsigned char b = bytes[at++];
        payload |= static_cast<std::uint32_t>(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) {
            terminated = true;
            break;
        }
    }
    if (!terminated || payload != len - at) {
        std::fprintf(stderr, "upb arm: %s: descriptor framing mismatch (%u + %u != %u)\n", what, at,
                     payload, len);
        return false;
    }
    upb_Arena* tmp = upb_Arena_New();
    google_protobuf_FileDescriptorProto* fdp = google_protobuf_FileDescriptorProto_parse(
        reinterpret_cast<const char*>(bytes + at), payload, tmp);
    bool ok = fdp != nullptr;
    if (!ok) {
        std::fprintf(stderr, "upb arm: %s: FileDescriptorProto parse failed\n", what);
    } else {
        upb_Status status;
        upb_Status_Clear(&status);
        ok = upb_DefPool_AddFile(s.pool, fdp, &status) != nullptr;
        if (!ok) {
            std::fprintf(stderr, "upb arm: %s: AddFile: %s\n", what,
                         upb_Status_ErrorMessage(&status));
        }
    }
    upb_Arena_Free(tmp);
    return ok;
}

// A message's (def, MiniTable) by full name, or {nullptr, nullptr} with the reason on stderr.
struct Found {
    const upb_MessageDef* def = nullptr;
    const upb_MiniTable* table = nullptr;
};
inline Found find_message(const char* full_name) {
    Found f;
    f.def = upb_DefPool_FindMessageByName(state().pool, full_name);
    if (f.def == nullptr) {
        std::fprintf(stderr, "upb arm: %s not found in the added descriptors\n", full_name);
        return f;
    }
    f.table = upb_MessageDef_MiniTable(f.def);
    return f;
}

// Load the embedded bench.proto descriptor and resolve bench.Dataset. False (with the reason on
// stderr) rather than abort: the bench reports the arm as unavailable and measures the rest.
inline bool init() {
    State& s = state();
    if (!add_descriptor_set(rp_upb_bench_desc, rp_upb_bench_desc_len, "bench.desc")) {
        return false;
    }
    const Found f = find_message("bench.Dataset");
    s.dataset_def = f.def;
    s.dataset_table = f.table;
    if (f.table == nullptr) {
        return false;
    }
    DatasetFields& d = s.ds;
    d.name = field_handle(f.def, "name");
    d.version = field_handle(f.def, "version");
    d.people = field_handle(f.def, "people");
    const Found person = find_message("bench.Person");
    const Found address = find_message("bench.Address");
    const Found attribute = find_message("bench.Attribute");
    d.p_id = field_handle(person.def, "id");
    d.p_name = field_handle(person.def, "name");
    d.p_email = field_handle(person.def, "email");
    d.p_active = field_handle(person.def, "active");
    d.p_score = field_handle(person.def, "score");
    d.p_created = field_handle(person.def, "created");
    d.p_address = field_handle(person.def, "address");
    d.p_tags = field_handle(person.def, "tags");
    d.p_history = field_handle(person.def, "history");
    d.p_attributes = field_handle(person.def, "attributes");
    d.p_counters = field_handle(person.def, "counters");
    d.a_street = field_handle(address.def, "street");
    d.a_city = field_handle(address.def, "city");
    d.a_zip = field_handle(address.def, "zip");
    d.at_key = field_handle(attribute.def, "key");
    d.at_value = field_handle(attribute.def, "value");
    for (const upb_MiniTableField* h :
         {d.name, d.version, d.people, d.p_id, d.p_name, d.p_email, d.p_active, d.p_score,
          d.p_created, d.p_address, d.p_tags, d.p_history, d.p_attributes, d.p_counters, d.a_street,
          d.a_city, d.a_zip, d.at_key, d.at_value}) {
        if (h == nullptr) {
            std::fprintf(stderr, "upb arm: a Dataset field handle failed to resolve\n");
            return false;
        }
    }
    return true;
}

// Decode + ACCESSOR-walk checksum: the timed arm's whole body, doing the same work as every
// other arm (decode, then read every present field into the checksum). Same contribution rule
// as bench_arena.cpp's walks; validated at startup against both the reflective walk and
// protoc's checksum.
inline std::uint64_t decode_and_sum_dataset(rapidproto::ByteView buf) {
    State& s = state();
    const DatasetFields& d = s.ds;
    upb_Arena* arena = upb_Arena_New();
    upb_Message* msg = upb_Message_New(s.dataset_table, arena);
    if (upb_Decode(buf.data(), buf.size(), msg, s.dataset_table, nullptr,
                   kUpb_DecodeOption_AliasString, arena) != kUpb_DecodeStatus_Ok) {
        upb_Arena_Free(arena);
        return ~std::uint64_t{0};
    }
    const upb_StringView kNoStr = {nullptr, 0};
    std::uint64_t sum = upb_Message_GetString(msg, d.name, kNoStr).size +
                        static_cast<std::uint64_t>(upb_Message_GetInt64(msg, d.version, 0));
    if (const upb_Array* people = upb_Message_GetArray(msg, d.people)) {
        const std::size_t n = upb_Array_Size(people);
        for (std::size_t i = 0; i < n; ++i) {
            const upb_Message* p = upb_Array_Get(people, i).msg_val;
            sum += static_cast<std::uint64_t>(upb_Message_GetInt64(p, d.p_id, 0));
            sum += upb_Message_GetString(p, d.p_name, kNoStr).size;
            sum += upb_Message_GetString(p, d.p_email, kNoStr).size;
            sum += upb_Message_GetBool(p, d.p_active, false) ? 1U : 0U;
            const double score = upb_Message_GetDouble(p, d.p_score, 0.0);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &score, sizeof bits);
            sum += bits;
            sum += upb_Message_GetUInt64(p, d.p_created, 0);
            if (const upb_Message* a = upb_Message_GetMessage(p, d.p_address)) {
                sum += upb_Message_GetString(a, d.a_street, kNoStr).size;
                sum += upb_Message_GetString(a, d.a_city, kNoStr).size;
                sum += upb_Message_GetUInt32(a, d.a_zip, 0);
            }
            if (const upb_Array* tags = upb_Message_GetArray(p, d.p_tags)) {
                const std::size_t tn = upb_Array_Size(tags);
                for (std::size_t j = 0; j < tn; ++j) {
                    sum += upb_Array_Get(tags, j).str_val.size;
                }
            }
            if (const upb_Array* hist = upb_Message_GetArray(p, d.p_history)) {
                const std::size_t hn = upb_Array_Size(hist);
                for (std::size_t j = 0; j < hn; ++j) {
                    sum += static_cast<std::uint32_t>(upb_Array_Get(hist, j).int32_val);
                }
            }
            if (const upb_Array* attrs = upb_Message_GetArray(p, d.p_attributes)) {
                const std::size_t an = upb_Array_Size(attrs);
                for (std::size_t j = 0; j < an; ++j) {
                    const upb_Message* at = upb_Array_Get(attrs, j).msg_val;
                    sum += upb_Message_GetString(at, d.at_key, kNoStr).size;
                    sum += upb_Message_GetString(at, d.at_value, kNoStr).size;
                }
            }
            if (const upb_Map* counters = upb_Message_GetMap(p, d.p_counters)) {
                std::size_t it = kUpb_Map_Begin;
                upb_MessageValue mk;
                upb_MessageValue mv;
                while (upb_Map_Next(counters, &mk, &mv, &it)) {
                    sum += mk.str_val.size + static_cast<std::uint32_t>(mv.int32_val);
                }
            }
        }
    }
    upb_Arena_Free(arena);
    return sum;
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
        // PRESENT-FIELDS-ONLY, the one convention every walk can implement: the streaming
        // decoder is wire-driven and cannot see an absent field's default, and proto2 defaults
        // are non-zero (google_message1's field129 defaults to a 21-char string). A field with
        // presence that is absent contributes nothing; implicit-presence scalars (proto3
        // Dataset) are read unconditionally -- absent means zero means no contribution.
        if (upb_FieldDef_HasPresence(f) && !upb_Message_HasFieldByDef(m, f)) {
            continue;
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
    const upb_DecodeStatus st = upb_Decode(buf.data(), buf.size(), msg, s.dataset_table, nullptr,
                                           kUpb_DecodeOption_AliasString, arena);
    const std::uint64_t sum =
        st == kUpb_DecodeStatus_Ok ? message_sum(msg, s.dataset_def) : ~std::uint64_t{0};
    upb_Arena_Free(arena);
    return sum;
}

}  // namespace rpupb

#endif  // RAPIDPROTO_HAVE_UPB
