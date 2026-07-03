// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// rapidproto runtime: the single, std-library-only header that generated streaming decoders depend
// on. It amalgamates the wire-format reader and the decode-dispatch machinery into one
// self-contained file with no rapidproto-internal includes, so the generator can drop a copy beside
// its output (<out-dir>/rapidproto/runtime.hpp) and every generated header just does
// `#include "rapidproto/runtime.hpp"`. This is also the runtime the schema library and its tests
// use directly (there is exactly one definition of WireReader / DecodeStatus, so a TU that pulls in
// both a generated header and the library never sees an ODR clash).
//
// ── Wire reader ────────────────────────────────────────────────────────────────────────────────
// Type-agnostic protobuf binary wire-format reader: it reads the ordered (field_number, wire_type,
// raw_value) records straight from a byte buffer with no dependency on the schema/AST. Interpreting
// a value as a particular protobuf type (zigzag, float bit-cast, packed-array splitting) is the
// caller's job, via the free helpers below. Wire input is UNTRUSTED, so the reader is fully
// validating: every varint-overflow, truncation, length-overrun, reserved-wire-type, and
// group-mismatch case is detected (see WireError). The pull-style primitives (read_tag +
// read_varint/read_fixed*/...) are the hot path: inline, allocation-free, returning
// std::optional<T>; failure records a WireError code + byte offset on the reader. The reader holds
// three raw const std::uint8_t* pointers: the hot cursor pair (m_cur/m_end) kept in registers across
// the decode loop, plus m_begin, which anchors position()/fail offsets. The input ByteView's char
// bytes are read through a uint8_t*, well-defined because uint8_t is unsigned char
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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

// One structural record. The payload variant is keyed by wire type:
//   Varint -> uint64_t (raw)        I64 -> uint64_t (raw bits)   I32 -> uint32_t (raw bits)
//   Len    -> ByteView (opaque)     SGroup -> ByteView (the group body span)
struct WireField {
    std::uint32_t field_number;
    WireType wire_type;
    std::variant<std::uint64_t, std::uint32_t, ByteView> payload;
};

// Largest field number (2^29 - 1) and the cap on group nesting depth for untrusted input.
inline constexpr std::uint32_t kMaxFieldNumber = (std::uint32_t{1} << 29U) - 1U;
inline constexpr int kMaxGroupDepth = 100;

// A pull cursor over a byte buffer. Non-owning (same lifetime contract as ByteView). On the
// first failure it records a WireError + offset and parks the cursor at end so loops stop and
// later reads also fail; query with failed()/error_code()/fail_offset().
class WireReader {
public:
    explicit WireReader(ByteView input) noexcept
        : m_begin(as_bytes(input.data())), m_cur(m_begin), m_end(m_begin + input.size()) {}

    bool at_end() const noexcept { return m_cur >= m_end; }
    std::size_t position() const noexcept { return static_cast<std::size_t>(m_cur - m_begin); }
    bool failed() const noexcept { return m_error != WireError::None; }
    WireError error_code() const noexcept { return m_error; }
    std::size_t fail_offset() const noexcept { return m_fail_offset; }

    // Hot primitives: inline, allocation-free. nullopt => failed (code/offset on the reader).
    std::optional<std::uint64_t> read_varint() noexcept;  // <=10 bytes, 1-byte fast path
    std::optional<Tag> read_tag() noexcept;
    std::optional<std::uint32_t> read_fixed32() noexcept;
    std::optional<std::uint64_t> read_fixed64() noexcept;
    std::optional<ByteView> read_length_delimited() noexcept;
    std::optional<WireField> read_field() noexcept;  // SGROUP -> body span

    // Read the body of a group whose SGROUP tag was already consumed (for `field_number`): the
    // bytes between it and the matching EGROUP, leaving the cursor just after the EGROUP. For
    // decoders that read the tag separately from the value.
    std::optional<ByteView> read_group(std::uint32_t field_number) noexcept {
        return read_group_body(field_number);
    }

    // Skip the value of a field whose tag (wire_type + field_number) was already read.
    bool skip(WireType wire_type, std::uint32_t field_number) noexcept;

private:
    // ByteView holds const char*; the cursor reads bytes through const std::uint8_t* (sound: the
    // static_assert above pins uint8_t == unsigned char). reinterpret_cast is the only way to retype
    // the borrowed pointer; no object is created, so this is purely an aliased read of the same bytes.
    static const std::uint8_t* as_bytes(const char* p) noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): char bytes -> byte pointer
        return reinterpret_cast<const std::uint8_t*>(p);
    }
    // Cold paths (defined inline below): record an error and park the cursor; group walking.
    void fail(WireError code, const std::uint8_t* at) noexcept;
    std::optional<ByteView> read_group_body(std::uint32_t field_number) noexcept;
    // Advance past fields to the matching EGROUP, returning that EGROUP tag's offset (with the
    // cursor left just after it), or nullopt on error.
    std::optional<std::size_t> scan_group_end(std::uint32_t field_number, int depth) noexcept;
    bool skip_value(Tag tag, int depth) noexcept;

    const std::uint8_t* m_begin;  // buffer start (anchors position() and fail offsets)
    const std::uint8_t* m_cur;    // read cursor; advances as bytes are consumed
    const std::uint8_t* m_end;    // one past the buffer end
    std::size_t m_fail_offset = 0;
    WireError m_error = WireError::None;
};

// Convenience (non-hot): collect a whole buffer/region's fields in declared order (no merge /
// last-wins). On failure returns nullopt and, if out_error is non-null, writes the WireError.
inline std::optional<std::vector<WireField>> read_message(ByteView input,
                                                          WireError* out_error = nullptr);

// Build a ByteView from a raw byte array (for tests/embedders holding uint8_t buffers).
inline ByteView byte_view(const std::uint8_t* data, std::size_t size) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes -> char view, no aliasing
    return ByteView(reinterpret_cast<const char*>(data), size);
}

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

// --- inline hot primitives --------------------------------------------------

inline std::optional<std::uint64_t> WireReader::read_varint() noexcept {
    const std::uint8_t* const start = m_cur;
    if (m_cur < m_end && (*m_cur & 0x80U) == 0U) {  // 1-byte fast path
        return static_cast<std::uint64_t>(*m_cur++);
    }
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 64U; shift += 7U) {
        if (m_cur >= m_end) {
            fail(WireError::TruncatedVarint, start);
            return std::nullopt;
        }
        const std::uint8_t b = *m_cur;
        if (shift == 63U && b > 1U) {  // 10th byte may only contribute bit 63
            fail(WireError::VarintOverflow, start);
            return std::nullopt;
        }
        ++m_cur;
        result |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U) {
            return result;
        }
    }
    fail(WireError::VarintOverflow, start);  // continuation bit still set after 10 bytes
    return std::nullopt;
}

inline std::optional<Tag> WireReader::read_tag() noexcept {
    const std::uint8_t* const start = m_cur;
    // Read as a full 64-bit varint so an over-range field number reports FieldNumberRange
    // (a 6+ byte tag varint) rather than a generic overflow.
    const std::optional<std::uint64_t> raw = read_varint();
    if (!raw) {
        return std::nullopt;
    }
    const std::uint64_t value = *raw;
    const std::uint32_t wire = static_cast<std::uint32_t>(value & 0x07U);
    const std::uint64_t field = value >> 3U;
    if (field == 0) {
        fail(WireError::InvalidFieldNumber, start);
        return std::nullopt;
    }
    if (field > kMaxFieldNumber) {
        fail(WireError::FieldNumberRange, start);
        return std::nullopt;
    }
    if (wire == 6U || wire == 7U) {
        fail(WireError::ReservedWireType, start);
        return std::nullopt;
    }
    return Tag{static_cast<std::uint32_t>(field), static_cast<WireType>(wire)};
}

inline std::optional<std::uint32_t> WireReader::read_fixed32() noexcept {
    const std::uint8_t* const start = m_cur;
    if (static_cast<std::size_t>(m_end - m_cur) < 4U) {
        fail(WireError::TruncatedI32, start);
        return std::nullopt;
    }
    const std::uint32_t v = static_cast<std::uint32_t>(m_cur[0]) |
                            (static_cast<std::uint32_t>(m_cur[1]) << 8U) |
                            (static_cast<std::uint32_t>(m_cur[2]) << 16U) |
                            (static_cast<std::uint32_t>(m_cur[3]) << 24U);
    m_cur += 4U;
    return v;
}

inline std::optional<std::uint64_t> WireReader::read_fixed64() noexcept {
    const std::uint8_t* const start = m_cur;
    if (static_cast<std::size_t>(m_end - m_cur) < 8U) {
        fail(WireError::TruncatedI64, start);
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8U; ++i) {
        v |= static_cast<std::uint64_t>(m_cur[i]) << (i * 8U);
    }
    m_cur += 8U;
    return v;
}

inline std::optional<ByteView> WireReader::read_length_delimited() noexcept {
    const std::uint8_t* const start = m_cur;
    const std::optional<std::uint64_t> len = read_varint();
    if (!len) {
        return std::nullopt;
    }
    // 32-bit hosts only: a length beyond size_t can't be addressed. This branch is `if
    // constexpr`-ed out on a 64-bit host, so it is unreachable (and therefore untested) on the
    // 64-bit CI; it exists for 32-bit correctness.
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (*len > std::numeric_limits<std::size_t>::max()) {
            fail(WireError::LengthTooLarge, start);
            return std::nullopt;
        }
    }
    const std::uint64_t avail = static_cast<std::uint64_t>(m_end - m_cur);
    if (*len > avail) {
        fail(WireError::LengthExceedsBuffer, start);
        return std::nullopt;
    }
    const std::size_t n = static_cast<std::size_t>(*len);
    const ByteView span = byte_view(m_cur, n);
    m_cur += n;
    return span;
}

inline std::optional<WireField> WireReader::read_field() noexcept {
    const std::uint8_t* const start = m_cur;
    const std::optional<Tag> tag = read_tag();
    if (!tag) {
        return std::nullopt;
    }
    switch (tag->wire_type) {
        case WireType::Varint: {
            const std::optional<std::uint64_t> v = read_varint();
            if (!v) {
                return std::nullopt;
            }
            return WireField{tag->field_number, tag->wire_type, *v};
        }
        case WireType::I64: {
            const std::optional<std::uint64_t> v = read_fixed64();
            if (!v) {
                return std::nullopt;
            }
            return WireField{tag->field_number, tag->wire_type, *v};
        }
        case WireType::I32: {
            const std::optional<std::uint32_t> v = read_fixed32();
            if (!v) {
                return std::nullopt;
            }
            return WireField{tag->field_number, tag->wire_type, *v};
        }
        case WireType::Len: {
            const std::optional<ByteView> v = read_length_delimited();
            if (!v) {
                return std::nullopt;
            }
            return WireField{tag->field_number, tag->wire_type, *v};
        }
        case WireType::SGroup: {
            const std::optional<ByteView> body = read_group_body(tag->field_number);
            if (!body) {
                return std::nullopt;
            }
            return WireField{tag->field_number, tag->wire_type, *body};
        }
        case WireType::EGroup:
            fail(WireError::UnexpectedEndGroup, start);
            return std::nullopt;
    }
    fail(WireError::ReservedWireType, start);  // unreachable: read_tag validated the wire type
    return std::nullopt;
}

inline bool WireReader::skip(WireType wire_type, std::uint32_t field_number) noexcept {
    return skip_value(Tag{field_number, wire_type}, 0);
}

// --- cold paths (error recording, the group-skip walk, and read_message) ----

inline void WireReader::fail(WireError code, const std::uint8_t* at) noexcept {
    if (m_error == WireError::None) {  // keep the first error
        m_error = code;
        m_fail_offset = static_cast<std::size_t>(at - m_begin);
    }
    m_cur = m_end;  // park at end so loops stop and later reads also fail
}

inline bool WireReader::skip_value(Tag tag, int depth) noexcept {
    switch (tag.wire_type) {
        case WireType::Varint:
            return read_varint().has_value();
        case WireType::I64:
            return read_fixed64().has_value();
        case WireType::I32:
            return read_fixed32().has_value();
        case WireType::Len:
            return read_length_delimited().has_value();
        case WireType::SGroup:
            return scan_group_end(tag.field_number, depth + 1).has_value();
        case WireType::EGroup:
            fail(WireError::UnexpectedEndGroup, m_cur);
            return false;
    }
    return false;  // unreachable: wire_type came from a validated tag
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): group-nesting walk
inline std::optional<std::size_t> WireReader::scan_group_end(std::uint32_t field_number,
                                                             int depth) noexcept {
    if (depth > kMaxGroupDepth) {
        fail(WireError::GroupTooDeep, m_cur);
        return std::nullopt;
    }
    while (true) {
        const std::uint8_t* const tag_start = m_cur;
        if (m_cur >= m_end) {
            fail(WireError::UnterminatedGroup, tag_start);
            return std::nullopt;
        }
        const std::optional<Tag> tag = read_tag();
        if (!tag) {
            return std::nullopt;
        }
        if (tag->wire_type == WireType::EGroup) {
            if (tag->field_number != field_number) {
                fail(WireError::EndGroupMismatch, tag_start);
                return std::nullopt;
            }
            // offset of the matching EGROUP tag; the cursor is now just past it
            return static_cast<std::size_t>(tag_start - m_begin);
        }
        if (!skip_value(*tag, depth)) {
            return std::nullopt;
        }
    }
}

inline std::optional<ByteView> WireReader::read_group_body(std::uint32_t field_number) noexcept {
    const std::size_t body_start = position();
    const std::optional<std::size_t> end_tag = scan_group_end(field_number, 1);
    if (!end_tag) {
        return std::nullopt;
    }
    return byte_view(m_begin + body_start, *end_tag - body_start);
}

inline std::optional<std::vector<WireField>> read_message(ByteView input, WireError* out_error) {
    WireReader reader(input);
    std::vector<WireField> fields;
    while (!reader.at_end()) {
        const std::optional<WireField> field = reader.read_field();
        if (!field) {
            if (out_error != nullptr) {
                *out_error = reader.error_code();
            }
            return std::nullopt;
        }
        fields.push_back(*field);
    }
    return fields;
}

// === Decode dispatch =======================================================

// Outcome of a decode. Default-constructed == success. Lean: a WireError + offset for a wire-level
// failure, or the `aborted` flag for a callback that returned an error. No allocation.
// Invariant: at most one of {wire != None, aborted} is set — the factories below are mutually
// exclusive, and from_reader always clears `aborted`.
struct [[nodiscard]] DecodeStatus {
    WireError wire = WireError::None;  // wire-level failure (None when ok or aborted)
    bool aborted = false;              // a callback returned an error
    std::size_t offset = 0;            // byte offset of a wire-level failure

    constexpr bool ok() const noexcept { return wire == WireError::None && !aborted; }
    explicit constexpr operator bool() const noexcept { return ok(); }

    static constexpr DecodeStatus success() noexcept { return {}; }
    static constexpr DecodeStatus abort() noexcept { return {WireError::None, true, 0}; }
    static DecodeStatus from_reader(const WireReader& reader) noexcept {
        return {reader.error_code(), false, reader.fail_offset()};
    }
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
