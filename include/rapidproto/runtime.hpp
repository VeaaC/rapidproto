// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// rapidproto runtime: the single, std-library-only header that generated streaming decoders depend
// on. It amalgamates the wire-format reader and the decode-dispatch machinery into one
// self-contained file with no rapidproto-internal includes, so the generator can drop a copy beside
// its output (<out-dir>/rapidproto/runtime.hpp) and every generated header just does
// `#include "rapidproto/runtime.hpp"`. This is also the runtime the schema library and its tests
// use directly (there is exactly one definition of the wire readers / DecodeStatus, so a TU that
// pulls in both a generated header and the library never sees an ODR clash).
//
// ── Wire reader ────────────────────────────────────────────────────────────────────────────────
// Type-agnostic protobuf binary wire-format reader: it reads the ordered (field_number, wire_type,
// raw_value) records straight from a byte buffer with no dependency on the schema/AST. Interpreting
// a value as a particular protobuf type (zigzag, float bit-cast, packed-array splitting) is the
// caller's job, via the free helpers below. Wire input is UNTRUSTED, so the reader is fully
// validating: every varint-overflow, truncation, length-overrun, reserved-wire-type, and
// group-mismatch case is detected (see WireError). The reader is a set of VALUE-THREADED free
// functions in `namespace wire` (read_varint / read_tag / read_tag_or_end / read_fixed32 /
// read_fixed64 / read_length_delimited / skip_value / read_group / ...): each takes the byte cursor
// as a (cur, end, begin) pointer triple and returns the advanced cursor -- nullptr on failure, with
// the WireError written to a caller-owned slot -- so the cursor stays in registers across reads with
// no escaping `this` (measurably fewer retired instructions than a stateful member cursor that would
// spill). All are inline and allocation-free; the generated arena and streaming decoders drive their
// loops with them. `begin` anchors the absolute fail offset a group walk reports. The input
// ByteView's char bytes are read through a uint8_t*, well-defined because uint8_t is unsigned char
// (the static_assert below pins it).
//
// ── Decode dispatch ────────────────────────────────────────────────────────────────────────────
// The small support a generated `MyMessage::decode(callbacks...)` is written against:
//   - build the overload set:            auto d = rapidproto::combine(callbacks...);
//   - per schema field, at compile time: if some callback exactly handles (Tag, Value) -- an
//                                        exact-typed callback or a generic catch-all (see
//                                        handles_one / specifically_handles below) -- decode the
//                                        value and rapidproto::invoke_field(d, Tag{}, v);
//                                        else  reader.skip(...)   // no matching callback -> skip
//   - a field number not in the schema goes to an explicit [](UnknownField){} handler if one was
//     passed (see specifically_handles_unknown / invoke_unknown below), otherwise it is skipped.
//   - a callback with the wrong value type, or a duplicate, is a compile error (not a silent skip).
//   - propagate the first non-ok DecodeStatus (wire error or callback abort).
// Callbacks fire in wire order as fields decode, repeated/packed fields once per element. On a
// non-ok status mid-stream, callbacks already fired for earlier fields/elements are not
// rolled back; check the returned status and discard partial results on failure.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// Flatten a generated decode(), portably: inline all of its callees (the wire primitives, dispatch
// helpers, sub-message decodes) into one function. In a large translation unit GCC's inliner is far
// more conservative than Clang's and leaves those out-of-line, so its decode loops call per element/
// field where Clang inlines -- ~30% more retired instructions on message/skip-heavy shapes. Flattening
// closes that gap (and Clang, already inlining, is ~neutral). Recognized compilers get the attribute;
// others get nothing (their default inlining, still correct). Generated decoders use RP_FLATTEN, so
// (unlike a purely-internal macro) it stays defined after this header -- an RP_-prefixed part of the
// runtime's surface, not undefined at end. The #ifndef lets a consumer TU pre-define RP_FLATTEN (e.g.
// to empty, or to a stronger attribute) without a redefinition error when it later includes a header.
#ifndef RP_FLATTEN
#if defined(__GNUC__) || defined(__clang__)
#define RP_FLATTEN __attribute__((flatten))
#else
#define RP_FLATTEN
#endif
#endif

namespace rapidproto {

// Borrowed raw bytes (not text); the caller owns the underlying buffer's lifetime.
using ByteView = std::string_view;

// The reader reads the ByteView's char bytes through a const std::uint8_t* cursor. That aliasing is
// well-defined only when uint8_t is a character type; pin it (holds on every supported platform).
static_assert(std::is_same_v<std::uint8_t, unsigned char>,
              "rapidproto reads wire bytes through uint8_t*; uint8_t must be unsigned char");

enum class WireType : std::uint8_t {
    Varint = 0,  // int32/64, uint32/64, sint32/64, bool, enum
    I64 = 1,     // fixed64, sfixed64, double
    Len = 2,     // string, bytes, embedded message, packed repeated
    SGroup = 3,  // start group (deprecated, retained)
    EGroup = 4,  // end group
    I32 = 5,     // fixed32, sfixed32, float
};

struct Tag {
    std::uint32_t field_number;
    WireType wire_type;
};

enum class WireError : std::uint8_t {
    None,
    TruncatedVarint,
    VarintOverflow,
    InvalidFieldNumber,
    FieldNumberRange,
    ReservedWireType,
    TruncatedI32,
    TruncatedI64,
    LengthExceedsBuffer,
    LengthTooLarge,
    UnexpectedEndGroup,
    EndGroupMismatch,
    UnterminatedGroup,
    GroupTooDeep,
};

// The whole wire tag for a (field, wire type) as one integer: (field_number << 3) | wire_type. For
// field numbers 1..15 this is a single byte, so generated decoders use it as a switch case label to
// dispatch the common 1-byte-tag fields without splitting field/wire.
constexpr std::uint32_t raw_tag(std::uint32_t field_number, WireType wire_type) noexcept {
    return (field_number << 3U) | static_cast<std::uint32_t>(wire_type);
}

// Largest field number (2^29 - 1) and the cap on group nesting depth for untrusted input.
inline constexpr std::uint32_t kMaxFieldNumber = (std::uint32_t{1} << 29U) - 1U;
inline constexpr int kMaxGroupDepth = 100;

// Build a ByteView from a raw byte array (for tests/embedders holding uint8_t buffers).
inline ByteView byte_view(const std::uint8_t* data, std::size_t size) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes -> char view, no aliasing
    return {reinterpret_cast<const char*>(data), size};
}

// The value-threaded wire readers: free functions that thread the byte cursor as a (cur, end, begin)
// pointer triple and return the advanced cursor, so it stays in registers with no escaping `this`.
// These are the hot path for the generated arena/streaming decoders.
namespace wire {

// The byte cursor over a ByteView's storage (the char* read through a uint8_t*, sound because uint8_t
// is unsigned char -- the static_assert above pins it). For generated hot loops that thread the cursor
// as a value rather than through a member (keeps it in a register across reads).
inline const std::uint8_t* byte_ptr(ByteView v) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): char view -> byte cursor, no aliasing
    return reinterpret_cast<const std::uint8_t*>(v.data());
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters, cppcoreguidelines-avoid-magic-numbers,
// readability-magic-numbers, cppcoreguidelines-pro-bounds-pointer-arithmetic): every wire:: reader takes
// the wire-reading triple (cursor, end, begin) then typed out-params (a fixed, deliberate convention),
// and reads the byte buffer with wire-format masks/shifts + cursor arithmetic.
// Varint read for generated hot loops: the cursor is passed in and returned BY VALUE (stays in a
// register across the call, with no escaping `this` that would force it to memory). Writes the raw
// varint to *out and returns the advanced cursor; on a truncated or overflowing varint returns nullptr
// and writes the WireError to *err. The caller derives the fail offset from the cursor it passed in.
inline const std::uint8_t* read_varint(const std::uint8_t* p, const std::uint8_t* end,
                                       std::uint64_t* out, WireError* err) noexcept {
    if (p < end && (*p & 0x80U) == 0U) {  // 1-byte fast path
        *out = *p;
        return p + 1;
    }
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 64U; shift += 7U) {
        if (p >= end) {
            *err = WireError::TruncatedVarint;
            return nullptr;
        }
        const std::uint8_t b = *p;
        if (shift == 63U && b > 1U) {  // 10th byte may only contribute bit 63
            *err = WireError::VarintOverflow;
            return nullptr;
        }
        ++p;
        result |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U) {
            *out = result;
            return p;
        }
    }
    *err = WireError::VarintOverflow;  // continuation bit still set after 10 bytes
    return nullptr;
}

// Tag read: fused 1-byte fast path, with InvalidFieldNumber / FieldNumberRange / ReservedWireType
// rejects. Returns the advanced cursor and writes *out; on a malformed tag returns nullptr and writes
// *err. The fail position is the passed-in cursor, so the caller reports offset = (entry cursor -
// buffer begin).
inline const std::uint8_t* read_tag(const std::uint8_t* p, const std::uint8_t* end, Tag* out,
                                    WireError* err) noexcept {
    if (p < end && (*p & 0x80U) == 0U) {  // fused 1-byte tag
        const std::uint32_t byte = *p;
        const std::uint32_t field = byte >> 3U;
        const std::uint32_t wire = byte & 0x07U;
        if (field != 0U && wire != 6U && wire != 7U) {
            *out = Tag{field, static_cast<WireType>(wire)};
            return p + 1;
        }
    }
    std::uint64_t raw =
        0;  // multi-byte: read the full 64-bit varint so over-range reports FieldNumberRange
    const std::uint8_t* const np = read_varint(p, end, &raw, err);
    if (np == nullptr) {
        return nullptr;
    }
    const auto wire = static_cast<std::uint32_t>(raw & 0x07U);
    const std::uint64_t field = raw >> 3U;
    if (field == 0) {
        *err = WireError::InvalidFieldNumber;
        return nullptr;
    }
    if (field > kMaxFieldNumber) {
        *err = WireError::FieldNumberRange;
        return nullptr;
    }
    if (wire == 6U || wire == 7U) {
        *err = WireError::ReservedWireType;
        return nullptr;
    }
    *out = Tag{static_cast<std::uint32_t>(field), static_cast<WireType>(wire)};
    return np;
}

// Outcome of read_tag_or_end: a decoded tag, a clean end-of-buffer, or a malformed tag.
enum class TagState : std::uint8_t { Tag, End, Error };

// Fused tag/end read: ONE bounds check distinguishes clean end / tag / error, so the decode loop
// drops the separate `p < end` the loop condition would otherwise duplicate against read_tag's own
// fast-path check. Writes *state;
// on Tag returns the advanced cursor (and writes *out), otherwise returns the passed-in cursor.
inline const std::uint8_t* read_tag_or_end(const std::uint8_t* p, const std::uint8_t* end, Tag* out,
                                           WireError* err, TagState* state) noexcept {
    if (p >= end) {
        *state = TagState::End;
        return p;
    }
    if ((*p & 0x80U) == 0U) {  // fused 1-byte tag
        const std::uint32_t byte = *p;
        const std::uint32_t field = byte >> 3U;
        const std::uint32_t wire = byte & 0x07U;
        if (field != 0U && wire != 6U && wire != 7U) {
            *out = Tag{field, static_cast<WireType>(wire)};
            *state = TagState::Tag;
            return p + 1;
        }
    } else if (static_cast<std::size_t>(end - p) >= 2U &&
               static_cast<std::uint8_t>(p[1] - 1U) < 0x7FU) {
        // Fused 2-byte tag (field 16..2047, the common >15-field case). Reached via `else`, so p[0]
        // continues; p[1]-1 < 0x7F means the second byte terminates and is nonzero (p[1] in 1..127 ->
        // value >= 128 -> field 16..2047, never 0 and within kMaxFieldNumber). Decoding it here, inline
        // in the loop driver, avoids the read_tag path clang compiles poorly for multibyte tags
        // (measured -31% on a >15-field decode). Only a reserved wire type can still reject; a 3+ byte,
        // non-canonical, or invalid tag falls through to the general read_tag below.
        const std::uint32_t value =
            (static_cast<std::uint32_t>(p[0]) & 0x7FU) | (static_cast<std::uint32_t>(p[1]) << 7U);
        const std::uint32_t wire = value & 0x07U;
        if (wire != 6U && wire != 7U) {
            *out = Tag{value >> 3U, static_cast<WireType>(wire)};
            *state = TagState::Tag;
            return p + 2;
        }
    }
    const std::uint8_t* const np = read_tag(p, end, out, err);  // rare: 3+ byte or invalid
    if (np == nullptr) {
        *state = TagState::Error;
        return p;
    }
    *state = TagState::Tag;
    return np;
}

// fixed32/fixed64: little-endian load, truncation reject. Fail position is the passed-in cursor.
inline const std::uint8_t* read_fixed32(const std::uint8_t* p, const std::uint8_t* end,
                                        std::uint32_t* out, WireError* err) noexcept {
    if (static_cast<std::size_t>(end - p) < 4U) {
        *err = WireError::TruncatedI32;
        return nullptr;
    }
    *out = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
    return p + 4;
}
inline const std::uint8_t* read_fixed64(const std::uint8_t* p, const std::uint8_t* end,
                                        std::uint64_t* out, WireError* err) noexcept {
    if (static_cast<std::size_t>(end - p) < 8U) {
        *err = WireError::TruncatedI64;
        return nullptr;
    }
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8U; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (i * 8U);
    }
    *out = v;
    return p + 8;
}

// Length-delimited read: reads the length varint, bounds-checks the payload, returns the payload span
// in *out and the cursor past it. Fail position is the passed-in cursor (the length varint start).
inline const std::uint8_t* read_length_delimited(const std::uint8_t* p, const std::uint8_t* end,
                                                 ByteView* out, WireError* err) noexcept {
    std::uint64_t len = 0;
    const std::uint8_t* const np = read_varint(p, end, &len, err);
    if (np == nullptr) {
        return nullptr;
    }
    if constexpr (sizeof(std::size_t) <
                  sizeof(std::uint64_t)) {  // 32-bit hosts only (see read_length_delimited)
        if (len > std::numeric_limits<std::size_t>::max()) {
            *err = WireError::LengthTooLarge;
            return nullptr;
        }
    }
    const auto avail = static_cast<std::uint64_t>(end - np);
    if (len > avail) {
        *err = WireError::LengthExceedsBuffer;
        return nullptr;
    }
    const auto n = static_cast<std::size_t>(len);
    *out = byte_view(np, n);
    return np + n;
}

// Field skip. `begin` is the buffer start: leaf failures happen at the passed-in cursor, but a nested
// group walk fails deep inside the recursion, so on ANY failure these write the absolute fail offset
// (fail position - begin) to *fail_off and the caller reports (*err, *fail_off). Cold path
// (unknown/group fields); the extra params never touch the hot cursor.
inline const std::uint8_t* scan_group_end(const std::uint8_t* p, const std::uint8_t* end,
                                          const std::uint8_t* begin, std::uint32_t field_number,
                                          int depth, WireError* err, std::size_t* fail_off,
                                          const std::uint8_t** egroup_tag = nullptr) noexcept;

inline const std::uint8_t* skip_value(const std::uint8_t* p, const std::uint8_t* end,
                                      const std::uint8_t* begin, Tag tag, int depth, WireError* err,
                                      std::size_t* fail_off) noexcept {
    switch (tag.wire_type) {
        case WireType::Varint: {
            std::uint64_t discard = 0;
            const std::uint8_t* const np = read_varint(p, end, &discard, err);
            if (np == nullptr) {
                *fail_off = static_cast<std::size_t>(p - begin);
            }
            return np;
        }
        case WireType::I64: {
            std::uint64_t discard = 0;
            const std::uint8_t* const np = read_fixed64(p, end, &discard, err);
            if (np == nullptr) {
                *fail_off = static_cast<std::size_t>(p - begin);
            }
            return np;
        }
        case WireType::I32: {
            std::uint32_t discard = 0;
            const std::uint8_t* const np = read_fixed32(p, end, &discard, err);
            if (np == nullptr) {
                *fail_off = static_cast<std::size_t>(p - begin);
            }
            return np;
        }
        case WireType::Len: {
            ByteView discard;
            const std::uint8_t* const np = read_length_delimited(p, end, &discard, err);
            if (np == nullptr) {
                *fail_off = static_cast<std::size_t>(p - begin);
            }
            return np;
        }
        case WireType::SGroup:
            return scan_group_end(p, end, begin, tag.field_number, depth + 1, err, fail_off);
        case WireType::EGroup:
            *err = WireError::UnexpectedEndGroup;
            *fail_off = static_cast<std::size_t>(p - begin);
            return nullptr;
    }
    return nullptr;  // unreachable: wire_type came from a validated tag
}

inline const std::uint8_t* scan_group_end(const std::uint8_t* p, const std::uint8_t* end,
                                          const std::uint8_t* begin, std::uint32_t field_number,
                                          int depth, WireError* err, std::size_t* fail_off,
                                          const std::uint8_t** egroup_tag) noexcept {
    if (depth > kMaxGroupDepth) {
        *err = WireError::GroupTooDeep;
        *fail_off = static_cast<std::size_t>(p - begin);
        return nullptr;
    }
    while (true) {
        const std::uint8_t* const tag_start = p;
        if (p >= end) {
            *err = WireError::UnterminatedGroup;
            *fail_off = static_cast<std::size_t>(tag_start - begin);
            return nullptr;
        }
        Tag tag{};
        const std::uint8_t* const np = read_tag(p, end, &tag, err);
        if (np == nullptr) {
            *fail_off = static_cast<std::size_t>(tag_start - begin);  // read_tag fails at its start
            return nullptr;
        }
        p = np;
        if (tag.wire_type == WireType::EGroup) {
            if (tag.field_number != field_number) {
                *err = WireError::EndGroupMismatch;
                *fail_off = static_cast<std::size_t>(tag_start - begin);
                return nullptr;
            }
            if (egroup_tag != nullptr) {
                *egroup_tag = tag_start;  // caller bounds the group body at the EGROUP tag
            }
            return p;  // cursor just past the matching EGROUP tag
        }
        const std::uint8_t* const sp = skip_value(p, end, begin, tag, depth, err, fail_off);
        if (sp == nullptr) {
            return nullptr;  // skip_value already set *fail_off
        }
        p = sp;
    }
}

// Group-body read: the SGROUP tag is already consumed; returns the body span (bytes up to the
// matching EGROUP tag, exclusive) in *body and
// the cursor just past the EGROUP. On a malformed/unterminated group returns nullptr and writes
// (*err, *fail_off) at the deep failure position.
inline const std::uint8_t* read_group(const std::uint8_t* p, const std::uint8_t* end,
                                      const std::uint8_t* begin, std::uint32_t field_number,
                                      ByteView* body, WireError* err,
                                      std::size_t* fail_off) noexcept {
    const std::uint8_t* egroup_tag = nullptr;
    const std::uint8_t* const np =
        scan_group_end(p, end, begin, field_number, 1, err, fail_off, &egroup_tag);
    if (np == nullptr) {
        return nullptr;
    }
    *body = byte_view(p, static_cast<std::size_t>(egroup_tag - p));
    return np;
}
// NOLINTEND(bugprone-easily-swappable-parameters, cppcoreguidelines-avoid-magic-numbers,
// readability-magic-numbers, cppcoreguidelines-pro-bounds-pointer-arithmetic)

}  // namespace wire

// --- caller-applied interpretation helpers (cannot fail; pure bit ops) ------

constexpr std::int32_t zigzag_decode_32(std::uint32_t v) noexcept {
    return static_cast<std::int32_t>((v >> 1U) ^ (0U - (v & 1U)));
}
constexpr std::int64_t zigzag_decode_64(std::uint64_t v) noexcept {
    return static_cast<std::int64_t>((v >> 1U) ^ (0ULL - (v & 1ULL)));
}
inline float bit_cast_float(std::uint32_t bits) noexcept {
    float out = 0.0F;
    std::memcpy(&out, &bits, sizeof out);  // C++17 has no std::bit_cast
    return out;
}
inline double bit_cast_double(std::uint64_t bits) noexcept {
    double out = 0.0;
    std::memcpy(&out, &bits, sizeof out);  // C++17 has no std::bit_cast
    return out;
}
constexpr bool varint_to_bool(std::uint64_t v) noexcept {
    return v != 0;
}
constexpr std::int32_t varint_to_int32(std::uint64_t v) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
}
constexpr std::int64_t varint_to_int64(std::uint64_t v) noexcept {
    return static_cast<std::int64_t>(v);
}

// === Decode dispatch =======================================================

// Outcome of a decode. Default-constructed == success. Lean: a WireError + offset for a wire-level
// failure, or the `aborted` flag for a callback that returned an error. No allocation.
// Invariant: at most one of {wire != None, aborted} is set — the factories below are mutually
// exclusive. A generated decoder builds a wire-level failure directly ({wire, false, offset}).
struct [[nodiscard]] DecodeStatus {
    WireError wire = WireError::None;  // wire-level failure (None when ok or aborted)
    bool aborted = false;              // a callback returned an error
    std::size_t offset = 0;            // byte offset of a wire-level failure

    [[nodiscard]] constexpr bool ok() const noexcept { return wire == WireError::None && !aborted; }
    explicit constexpr operator bool() const noexcept { return ok(); }

    static constexpr DecodeStatus success() noexcept { return {}; }
    static constexpr DecodeStatus abort() noexcept { return {WireError::None, true, 0}; }
};

// The overload-set pattern: inherit every callback's operator() so the right one is selected by
// the (Tag, Value...) arguments. `combine` builds it from a callback pack (decayed, owned).
template <class... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};

template <class... Fs>
constexpr overloaded<std::decay_t<Fs>...> combine(Fs&&... fs) {
    return overloaded<std::decay_t<Fs>...>{std::forward<Fs>(fs)...};
}

// The callback dispatch gate (compile-time only). A callback claims a field only when its parameter
// types are exactly the field's (Tag, Value...) — not merely convertible.
// `is_invocable`-with-probes would accept any convertible parameter (a `(Tag, float)` lambda
// silently narrowing a `double` field, or a `(Tag, std::optional<int32_t>)` lambda wrapping an
// int32 field), so instead the gate compares the callback's actual parameter types against the
// expected ones. A generic `[](auto, auto)` catch-all (a templated operator()) has no fixed
// parameter list and matches every field.
//
// A callback must be either fully concrete `(Tag, Value...)` or a fully generic `(auto, auto...)`
// catch-all. A partially-generic callback (`(auto, int32_t)` or `(Foo::id, auto)`) is rejected by a
// generated static_assert (it would silently convert or pin a value); only those two shapes reach
// this gate.

// The value-slot probe: `generic_probe` converts to no protobuf value type, so only a generic
// (templated) value parameter accepts it. This distinguishes a true catch-all (`(auto, auto)`, which
// accepts it) from a partially-generic callback (`(auto, int32_t)`, which does not). That is a
// distinction a permissive "convertible to anything" probe could not make.
struct generic_probe {};
template <class>
using as_generic_probe = generic_probe;  // map each value slot to a probe (preserves arity)

// The tag-slot probe: a stand-in for a generated field tag that mirrors its compile-time identity
// surface (kNumber / kName). A fixed-arity catch-all may read these off its `auto tag` parameter (the
// documented `tag.kName` / `tag.kNumber` logging pattern); probing the tag slot with a tag-shaped
// type (rather than the empty generic_probe) lets such a catch-all instantiate cleanly during
// classification, so it is recognized as a catch-all instead of being mis-flagged partially-generic.
// It declares no value conversions, so value-slot strictness (partial-generic rejection) is intact.
struct tag_probe {
    static constexpr std::uint32_t kNumber = 0;
    static constexpr std::string_view kName = {};
};

// Supported callback forms: a lambda, or a functor with a single, non-ref-qualified operator()
// (const or mutable). A functor with a ref-qualified or overloaded operator() cannot be
// introspected (call_params falls back to generic_params) and is reported as partially-generic --
// use separate lambdas instead. The catch-all/partial distinction also assumes a field Value type
// is not implicitly constructible from an unrelated empty struct (generic_probe); every generated
// Value type holds this (scalars, string_view, open enums, and sub-decoders whose only ctor is
// explicit ByteView).

// Sentinel for a callable with a templated/`auto` operator() (a catch-all) — no fixed parameters.
struct generic_params {};

// The decayed parameter types of a callable's lone operator(), as a tuple. (Callbacks are functors
// or lambdas; a raw function pointer cannot be a callback since `overloaded` inherits from each, so
// no function-pointer overload is needed.)
template <class R, class C, class... A>
std::tuple<std::decay_t<A>...> param_tuple_of(R (C::*)(A...));
template <class R, class C, class... A>
std::tuple<std::decay_t<A>...> param_tuple_of(R (C::*)(A...) const);
template <class R, class C, class... A>
std::tuple<std::decay_t<A>...> param_tuple_of(R (C::*)(A...) noexcept);
template <class R, class C, class... A>
std::tuple<std::decay_t<A>...> param_tuple_of(R (C::*)(A...) const noexcept);

// `generic_params` when the operator() is a template (a catch-all) or otherwise not extractable
// (e.g. a ref-qualified or overloaded operator()). The SFINAE probe is on the whole
// `param_tuple_of(...)` call, so an operator() that exists but no overload matches falls back to
// `generic_params` rather than hard-erroring.
template <class F, class = void>
struct call_params {
    using type = generic_params;
};
template <class F>
struct call_params<F, std::void_t<decltype(param_tuple_of(&F::operator()))>> {
    using type = decltype(param_tuple_of(&F::operator()));
};

// True when `Cb` is a generic callback: its operator() is a template (or otherwise not
// introspectable) — a catch-all or a partial generic, distinguished by the traits below.
template <class Cb>
inline constexpr bool is_generic_callback =
    std::is_same_v<typename call_params<std::decay_t<Cb>>::type, generic_params>;

// True when a single callback `Cb` specifically accepts (Tag, Vs...): its parameter types decay to
// exactly (Tag, Vs...). Folded over the callback pack to count specific handlers, so duplicates are
// a compile error — and a `(Tag, std::optional<V>)`-style wrapper is NOT counted as handling.
template <class Cb, class Tag, class... Vs>
inline constexpr bool specifically_handles =
    std::is_same_v<typename call_params<std::decay_t<Cb>>::type, std::tuple<Tag, Vs...>>;

// True when `Cb` is a generic catch-all at the right arity: a templated operator() that accepts a
// generic argument in every position. The tag slot is probed with `tag_probe` (so a catch-all may
// introspect the tag's kName/kNumber), each value slot with the empty `generic_probe` (so a callback
// that pins a value type — a partial generic — is rejected). A callback that pins the tag is
// concrete there, so its operator() is not a template and it never reaches this trait (call_params
// extracts a real tuple, routing it through specifically_handles / targets instead).
template <class Cb, class Tag, class... Vs>
inline constexpr bool is_catch_all =
    is_generic_callback<Cb> &&
    std::is_invocable_v<std::decay_t<Cb>&, tag_probe, as_generic_probe<Vs>...>;

// True when `Cb` handles this field: exact-typed or a viable catch-all. Folded over the pack, it
// drives the decode-vs-skip choice.
template <class Cb, class Tag, class... Vs>
inline constexpr bool handles_one =
    specifically_handles<Cb, Tag, Vs...> || is_catch_all<Cb, Tag, Vs...>;

// True when `Cb` is a partially-generic callback that applies to this field: a templated operator()
// invocable with the field's (Tag, Vs...) but NOT a full catch-all (it pins the tag or a value
// type, e.g. `(auto, int32_t)` or `(Foo::id, auto)`). Folded per callback into a generated
// static_assert. Such a callback would silently convert or pin a value, so it is a compile error.
// The invocability check covers both prvalue and lvalue value arguments, so a generic callback with
// a non-const lvalue-ref value (`(auto, auto&)`, which a catch-all probe rejects because the
// decoded value is a prvalue) is reported here rather than silently skipped.
template <class Cb, class Tag, class... Vs>
inline constexpr bool is_partial_generic =
    is_generic_callback<Cb> &&
    (std::is_invocable_v<std::decay_t<Cb>&, Tag, Vs...> ||
     std::is_invocable_v<std::decay_t<Cb>&, Tag, std::add_lvalue_reference_t<Vs>...>) &&
    !is_catch_all<Cb, Tag, Vs...>;

// The first parameter type of a callable (void for a catch-all / non-tuple).
template <class T>
struct first_param {
    using type = void;
};
template <class First, class... Rest>
struct first_param<std::tuple<First, Rest...>> {
    using type = First;
};

// True when a single callback `Cb` names this Tag — its first parameter is exactly the Tag,
// whatever the value parameters. Folded over the pack: a field named by a callback that does not
// handle it (wrong value type or wrong arity, e.g. a map callback missing its value) becomes a
// compile error instead of a silent skip. Per-callback (not a check on the combined dispatcher).
template <class Cb, class Tag, class... Vs>
inline constexpr bool targets =
    std::is_same_v<typename first_param<typename call_params<std::decay_t<Cb>>::type>::type, Tag>;

// Invoke the dispatcher for one field occurrence, normalizing a void or DecodeStatus return into a
// DecodeStatus. Precondition: some callback handles_one<Cb, Tag, Vs...> (the caller checked).
template <class D, class Tag, class... Vs>
constexpr DecodeStatus invoke_field(D& dispatcher, Tag tag, Vs&&... vs) {
    using Ret = std::invoke_result_t<D&, Tag, Vs...>;
    static_assert(std::is_void_v<Ret> || std::is_same_v<Ret, DecodeStatus>,
                  "a field callback must return void or rapidproto::DecodeStatus");
    if constexpr (std::is_void_v<Ret>) {
        dispatcher(tag, std::forward<Vs>(vs)...);
        return DecodeStatus::success();
    } else {
        return dispatcher(tag, std::forward<Vs>(vs)...);
    }
}

// Invoke the dispatcher for a decoded oneof member (the arena model's reader). Unlike a streaming
// field callback, a oneof handler cannot abort — the message is already decoded — so a returned
// DecodeStatus (or anything else) would be silently discarded; require void instead of discarding.
template <class D, class Tag, class... Vs>
constexpr void invoke_handler(D& dispatcher, Tag tag, Vs&&... vs) {
    static_assert(std::is_void_v<std::invoke_result_t<D&, Tag, Vs...>>,
                  "a oneof handler must return void (the message is already decoded; there is"
                  " nothing to abort)");
    dispatcher(tag, std::forward<Vs>(vs)...);
}

// A field whose number is not in the schema (it hit the generated switch's `default`). If `decode()`
// is given a callback that takes a single UnknownField (a concrete
// `[](rapidproto::UnknownField){}` or a 1-arg generic `[](auto){}`), each such field is delivered
// to it (otherwise it is skipped). `bytes` is the raw value bytes after the tag, exactly as they
// appear on the wire: for a LEN field that includes the length prefix, and for a group it is the
// body plus the trailing EGROUP marker; the tag is `varint((field_number << 3) | wire_type)`. (See
// README.md for the re-serialize / logging patterns.) Known schema fields you simply did not give a
// callback for are NOT "unknown"; use a generic `[](auto, auto)` catch-all to receive those, typed.
struct UnknownField {
    std::uint32_t field_number;
    WireType wire_type;
    ByteView bytes;
};

// True when a single callback `Cb` is specifically an unknown-field handler: invocable with one
// UnknownField but NOT as a (Tag, Value...) field handler. A generic field catch-all
// (`(auto, auto)` / `(auto, auto&&...)`) also accepts a lone UnknownField, but it is a field
// handler (known fields only) and is excluded here, so unknown fields require an explicit
// `[](UnknownField)` (or 1-arg generic) handler. Folded over the callback pack by the generated
// `default` arm. std::conjunction short-circuits: the second probe is not instantiated unless the
// callback accepts a lone UnknownField, so a fixed-arity field catch-all (which does not) is never
// instantiated against UnknownField, which matters when its body introspects the tag (UnknownField
// has no kName/kNumber).
template <class Cb>
inline constexpr bool specifically_handles_unknown = std::conjunction_v<
    std::is_invocable<std::decay_t<Cb>&, UnknownField>,
    std::negation<std::is_invocable<std::decay_t<Cb>&, UnknownField, generic_probe>>>;

// True when `Cb` names one of `Tags` as its first parameter, whatever the rest of its signature
// (the per-tag guards then enforce the exact shape). False for a generic callback (no fixed first
// parameter) and over an empty tag list.
template <class Cb, class... Tags>
inline constexpr bool names_some_tag =
    (... || std::is_same_v<typename first_param<typename call_params<std::decay_t<Cb>>::type>::type,
                           Tags>);

// True when `Cb` can never fire in the arena model's oneof reader, whose tag types are `Tags` (the
// member tags plus the std::monostate unset state): it is concrete and names none of them. Folded
// per callback into a generated static_assert — the classic mistake is a callback pasted from
// ANOTHER oneof's reader (that tag type exists, so no other guard fires), which would otherwise
// compile and silently never be called.
template <class Cb, class... Tags>
inline constexpr bool is_stray_handler = !is_generic_callback<Cb> && !names_some_tag<Cb, Tags...>;

// The streaming decode() analog: same rule, except decode() additionally accepts the unknown-field
// handler (which names no tag).
template <class Cb, class... Tags>
inline constexpr bool is_stray_callback =
    is_stray_handler<Cb, Tags...> && !specifically_handles_unknown<Cb>;

// Invoke the unknown-field handler, normalizing a void or DecodeStatus return. Precondition: some
// callback specifically_handles_unknown (the generated `default` arm checks the pack with
// `if constexpr`); the combined dispatcher's `[](UnknownField)` overload wins by overload
// resolution.
template <class D>
constexpr DecodeStatus invoke_unknown(D& dispatcher, UnknownField field) {
    using Ret = std::invoke_result_t<D&, UnknownField>;
    static_assert(std::is_void_v<Ret> || std::is_same_v<Ret, DecodeStatus>,
                  "an unknown-field callback must return void or rapidproto::DecodeStatus");
    if constexpr (std::is_void_v<Ret>) {
        dispatcher(field);
        return DecodeStatus::success();
    } else {
        return dispatcher(field);
    }
}

}  // namespace rapidproto
