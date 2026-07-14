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

// Force a function OUT of line -- the deliberate counterpart to RP_FLATTEN. Because the generated
// decode() is RP_FLATTEN (which overrides the compiler's inlining heuristics and pulls in every callee
// transitively, regardless of size), a large packed-varint SWAR kernel called from decode() would be
// duplicated at every packed field: code that grows linearly with the field count (a large schema's
// decode() balloons) and, on gcc, degrades register allocation of the surrounding tail. RP_NOINLINE is
// the "flatten everything EXCEPT this" carve-out, so the kernel stays one shared out-of-line copy. gcc's
// flatten obeys it (without it decode() re-inlines the kernel); clang's flatten spares the big callee on
// its own, so there it is belt-and-suspenders. Uniform on both compilers, so it is not compiler-gating.
// Same #ifndef/consumer-override rationale as RP_FLATTEN above.
#ifndef RP_NOINLINE
#if defined(__GNUC__) || defined(__clang__)
#define RP_NOINLINE __attribute__((noinline))
#else
#define RP_NOINLINE
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

// ── SWAR packed-varint decode (word-at-a-time, branchless) ─────────────────────────────────────────
// Decodes one varint by inspecting a whole 8-byte word at once, so the per-byte continuation-bit branch
// that mispredicts on mixed-width data does not exist. Portable: no PEXT/SIMD -- the byte-mask is a
// multiply, the 7-bit gather is shift/mask. Used ONLY for packed repeated varints: the 8-byte word read
// is issued only with >= 8 in-span bytes remaining, so the load stays INSIDE the span (never past `end`);
// the caller scalar-decodes the last < 8 bytes. There is no reliance on readable slack past the span.
namespace swar_detail {
inline constexpr std::uint64_t kMSB = 0x8080808080808080ULL;   // MSB of each byte
inline constexpr std::uint64_t kLow7 = 0x7F7F7F7F7F7F7F7FULL;  // low 7 bits of each byte

// 8 bytes as a uint64 in little-endian logical order (byte i in bits 8i..8i+7). memcpy is a single
// aligned-agnostic load; on a big-endian host it is byte-reversed back to that logical order. The
// reverse prefers the compiler intrinsic (gcc/clang -> a `rev`/`bswap` instruction) but falls back to
// portable shifts for any other compiler, so no supported build is left without a definition.
inline std::uint64_t load64(const std::uint8_t* p) noexcept {
    std::uint64_t w = 0;
    std::memcpy(&w, p, sizeof w);
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#if defined(__GNUC__) || defined(__clang__)
    w = __builtin_bswap64(w);
#else
    w = ((w & 0xFF00000000000000ULL) >> 56U) | ((w & 0x00FF000000000000ULL) >> 40U) |
        ((w & 0x0000FF0000000000ULL) >> 24U) | ((w & 0x000000FF00000000ULL) >> 8U) |
        ((w & 0x00000000FF000000ULL) << 8U) | ((w & 0x0000000000FF0000ULL) << 24U) |
        ((w & 0x000000000000FF00ULL) << 40U) | ((w & 0x00000000000000FFULL) << 56U);
#endif
#endif
    return w;
}
// Count trailing zero bits; precondition x != 0.
inline int ctz64(std::uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#else
    int n = 0;
    while ((x & 1U) == 0U) {
        x >>= 1U;
        ++n;
    }
    return n;
#endif
}
// Gather the low 7 bits of each byte into a contiguous value: three SWAR compaction rounds.
inline std::uint64_t compact7(std::uint64_t x) noexcept {
    x = ((x & 0x7F007F007F007F00ULL) >> 1U) | (x & 0x007F007F007F007FULL);
    x = ((x & 0x3FFF00003FFF0000ULL) >> 2U) | (x & 0x00003FFF00003FFFULL);
    x = ((x & 0x0FFFFFFF00000000ULL) >> 4U) | (x & 0x000000000FFFFFFFULL);
    return x;
}
// movemask without SIMD: gather the MSB of each byte into 8 bits (bit i = MSB of byte i).
inline std::uint64_t bytemask8(std::uint64_t w) noexcept {
    return ((w & kMSB) * 0x0002040810204081ULL) >> 56U;
}
inline int popcount64(std::uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    int n = 0;
    while (x != 0U) {
        x &= x - 1U;
        ++n;
    }
    return n;
#endif
}
// 64-bit terminator mask over the 64 bytes at p (bit i set = byte i terminates a varint). Reads 64 bytes.
inline std::uint64_t stopmask64(const std::uint8_t* p) noexcept {
    std::uint64_t m = 0;
    for (int j = 0; j < 8; ++j) {
        m |= bytemask8(~load64(p + (8 * j))) << (8U * static_cast<unsigned>(j));
    }
    return m;
}
}  // namespace swar_detail

// Defined below; declared here so the packed-varint decoder (a template) can name it.
inline const std::uint8_t* read_varint_packed(const std::uint8_t* p, const std::uint8_t* end,
                                              std::uint64_t* out, WireError* err) noexcept;

// Which kernel to decode a packed-varint span with. Chosen once per field from the first 64-byte
// window; a wrong guess is only slower, never incorrect (every kernel decodes identically).
enum class PackedKernel : std::uint8_t {
    ByteLoop,  // wide/predictable widths, or a small span: the branch-predicted scalar loop wins
    Swar,    // wide-mixed / 1-byte-dominant: branchless word-at-a-time (its 1-byte fast path wins)
    Struct,  // NARROW-mixed (max width <= 5): one stopmask per block, then independent compact7s
    Bulk1,   // homogeneous 1-byte: widen 64 bytes at once (auto-vectorizes)
    Bulk2,   // homogeneous 2-byte: four varints per 8-byte word
    Fixed,   // homogeneous 3..10-byte: fixed-stride decode, no boundary find (beats the byte loop)
};

// Classify the width regime from the first 64-byte window. Reads 64 bytes at p, so is only entered when
// span is comfortably above that. Homogeneous width == equal gaps between terminators. For a Fixed
// (homogeneous 3..10-byte) result, `*width` is that width; it is unspecified otherwise.
inline PackedKernel packed_strategy(const std::uint8_t* p, std::size_t span, int* width) noexcept {
    *width =
        0;  // defined for every return; only a Fixed result overwrites it (localizes the contract)
    if (span < 256) {
        // Only tiny spans skip the kernels: below this the classify scan and kernel-switch aren't
        // amortized. (This floor is about dispatch cost, NOT branch prediction -- a real decode sees each
        // buffer once, so the byte loop mispredicts at its steady-state rate at any element count; the
        // kernels' branch-free win therefore holds down to very small arrays, per the multi-seed probe.)
        return PackedKernel::ByteLoop;
    }
    const std::uint64_t stop =
        swar_detail::stopmask64(p);  // bit i set = byte i terminates a varint
    if (stop == 0) {
        // No terminator in 64 bytes: a single varint longer than the window (malformed/overlong, since a
        // valid varint is <= 10 bytes) -> the validating byte loop rejects it.
        return PackedKernel::ByteLoop;
    }
    // Scan EVERY terminator: track the first varint's width (for the homogeneity test) and the MAX
    // width. A skewed array is mostly 1-byte with an occasional wide value, so a short scan would miss
    // the width change; and the max width separates narrow-mixed (Struct) from wide-mixed (Swar).
    std::uint64_t m = stop;
    int last = -1;  // previous terminator position (-1 => this varint starts at byte 0)
    int g0 = -1;
    int maxw = 0;
    bool homo = true;
    while (m != 0U) {
        const int b = swar_detail::ctz64(m);
        m &= m - 1U;
        const int w = b - last;  // this varint's width (bytes last+1 .. b)
        last = b;
        if (w > maxw) {
            maxw = w;
        }
        if (g0 < 0) {
            g0 = w;
        } else if (w != g0) {
            homo = false;
        }
    }
    if (!homo) {
        // Narrow-mixed (small, dense widths) amortizes one stopmask over the whole block; wide-mixed or
        // 1-byte-dominant is better served by per-element SWAR (its scalar 1-byte fast path). The <= 5
        // boundary is where the structural kernel stops beating SWAR (measured). Both beat the byte loop
        // at every element count on fresh data -- there is no count gate; see the span floor above.
        return (maxw <= 5) ? PackedKernel::Struct : PackedKernel::Swar;
    }
    if (g0 == 1) {
        return PackedKernel::Bulk1;  // homogeneous 1-byte
    }
    if (g0 == 2) {
        return PackedKernel::Bulk2;  // homogeneous 2-byte
    }
    if (g0 >= 3 && g0 <= 10) {
        *width = g0;
        return PackedKernel::Fixed;  // homogeneous 3..10-byte: fixed stride beats the byte loop
    }
    return PackedKernel::
        ByteLoop;  // g0 > 10: an overlong (malformed) varint -> the validating byte loop
}

// ── Packed-varint output sinks ──────────────────────────────────────────────────────────────────
// decode_packed_varints is templated on a SINK so BOTH generated decoders share one implementation:
// the arena decoder's sink stores conv(raw) into its pre-allocated array; the streaming decoder's
// invokes the field callback. A sink exposes put(index, raw) for one element and put2(index, r0, r1)
// for two adjacent ones (the fixed 2-per-load kernel -- its own method so the two array stores fold to
// a base+offset pair, which they don't through two separate put() calls). A void-returning put never
// aborts (arena); a bool-returning put stops decoding when it returns false (a callback abort). These
// two helpers hide that void/bool distinction: they always return "keep going".
template <class Sink>
inline bool sink_put(Sink& sink, std::size_t i, std::uint64_t raw) {
    if constexpr (std::is_void_v<decltype(sink.put(i, raw))>) {
        sink.put(i, raw);
        return true;
    } else {
        return sink.put(i, raw);
    }
}
template <class Sink>
inline bool sink_put2(Sink& sink, std::size_t i, std::uint64_t r0, std::uint64_t r1) {
    if constexpr (std::is_void_v<decltype(sink.put2(i, r0, r1))>) {
        sink.put2(i, r0, r1);
        return true;
    } else {
        return sink.put2(i, r0, r1);
    }
}

// Arena sink: store conv(raw) into a pre-allocated array (base `out`). Never aborts (void put), so the
// generic kernels' abort checks fold away and the bulk stores stay vectorizable / fused.
template <class Elem, class Conv>
struct array_sink {
    Elem* out;
    void put(std::size_t i, std::uint64_t raw) const noexcept { out[i] = Conv{}(raw); }
    void put2(std::size_t i, std::uint64_t r0, std::uint64_t r1) const noexcept {
        out[i] = Conv{}(r0);
        out[i + 1] = Conv{}(r1);
    }
};

// Fixed-stride decode of homogeneous W-byte varints (W a COMPILE-TIME constant, so the mask/stride/
// guard specialize). Guards each element with a FULL W-byte continuation-pattern compare -- bytes 0..W-2
// must all continue and byte W-1 must terminate (i.e. EXACTLY one W-byte varint); checking only the last
// two bytes would accept a shorter varint followed by others merged into the window. Stops on the first
// width change (or a sink abort), leaving the rest to the caller's byte-loop tail. Advances `*np`.
template <int W, class Sink>
inline const std::uint8_t* fixed_fill(const std::uint8_t* q, const std::uint8_t* end,
                                      std::size_t* np, bool* aborted, Sink& sink) noexcept {
    constexpr std::uint64_t kLenMask =
        (W >= 8) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (8U * static_cast<unsigned>(W))) - 1U);
    constexpr std::uint64_t kMsbW = swar_detail::kMSB & kLenMask;  // MSB of each of the low W bytes
    // The one accepted pattern: bytes 0..W-2 continue (MSB set), byte W-1 terminates (MSB clear).
    constexpr std::uint64_t kOneVarint =
        kMsbW ^ (std::uint64_t{0x80} << (8U * static_cast<unsigned>(W - 1)));
    std::size_t n = *np;
    if constexpr (W <= 4) {
        // Two W-byte varints (2W <= 8 bytes) fit in one 8-byte load, so decode both per load with a
        // full 2W-byte pattern compare -- ~1.2-1.3x over one-per-load on fx3/fx4 (common id/count widths).
        constexpr std::uint64_t kMsb2 = kMsbW | (kMsbW << (8U * static_cast<unsigned>(W)));
        constexpr std::uint64_t kTwoVarints =
            kOneVarint | (kOneVarint << (8U * static_cast<unsigned>(W)));
        while (static_cast<std::size_t>(end - q) >= 8) {
            const std::uint64_t w = swar_detail::load64(q);
            if ((w & kMsb2) == kTwoVarints) {  // two back-to-back W-byte varints
                if (!sink_put2(sink, n, swar_detail::compact7(w & kLenMask & swar_detail::kLow7),
                               swar_detail::compact7((w >> (8U * static_cast<unsigned>(W))) &
                                                     kLenMask & swar_detail::kLow7))) {
                    *aborted = true;
                    *np = n;
                    return q;
                }
                n += 2;
                q += 2 * W;
            } else if ((w & kMsbW) == kOneVarint) {  // just one, then a width change
                if (!sink_put(sink, n, swar_detail::compact7(w & kLenMask & swar_detail::kLow7))) {
                    *aborted = true;
                    *np = n;
                    return q;
                }
                ++n;
                q += W;
            } else {
                break;  // not a W-byte varint (wider, narrower, or an interior terminator)
            }
        }
    } else {
        while (static_cast<std::size_t>(end - q) >= 8) {
            const std::uint64_t w = swar_detail::load64(q);
            if ((w & kMsbW) != kOneVarint) {
                break;  // not exactly one W-byte varint (wider, narrower, or an interior terminator)
            }
            if (!sink_put(sink, n, swar_detail::compact7(w & kLenMask & swar_detail::kLow7))) {
                *aborted = true;
                *np = n;
                return q;
            }
            ++n;
            q += W;
        }
    }
    *np = n;
    return q;
}

// Fixed-stride decode of homogeneous 9/10-byte varints (W a COMPILE-TIME 9 or 10). These span more than
// one word, so the value is assembled in two parts: compact7 of the low 8 bytes (bits 0..55) plus the
// high byte(s). Matches read_varint's overflow rule -- a 10-byte varint's last byte may only be 0 or 1
// (bit 63) -- by bailing anything else to the validating byte-loop tail, which reports the overflow.
// Guards each element's exact width and stops on the first width change (rest -> caller's tail).
template <int W, class Sink>
inline const std::uint8_t* fixed_fill_wide(const std::uint8_t* q, const std::uint8_t* end,
                                           std::size_t* np, bool* aborted, Sink& sink) noexcept {
    static_assert(W == 9 || W == 10, "fixed_fill_wide handles only 9/10-byte varints");
    std::size_t n = *np;
    while (static_cast<std::size_t>(end - q) >= W) {
        const std::uint64_t w0 =
            swar_detail::load64(q);  // bytes 0..7 (all continue for a >= 9-byte one)
        if ((w0 & swar_detail::kMSB) != swar_detail::kMSB) {
            break;  // a byte in 0..7 terminates -> narrower than W
        }
        std::uint64_t value = swar_detail::compact7(w0 & swar_detail::kLow7);  // bits 0..55
        const std::uint8_t b8 = q[8];
        if constexpr (W == 9) {
            if ((b8 & 0x80U) != 0U) {
                break;  // byte 8 continues -> wider than 9
            }
            value |= static_cast<std::uint64_t>(b8 & 0x7FU) << 56U;  // bits 56..62
        } else {                                                     // W == 10
            const std::uint8_t b9 = q[9];
            if ((b8 & 0x80U) == 0U || (b9 & 0x80U) != 0U) {
                break;  // not exactly 10 bytes (byte 8 must continue, byte 9 must terminate)
            }
            if (b9 > 1U) {
                break;  // 10th byte > 1 encodes bits above 63 -> overflow; let the tail report it
            }
            value |= static_cast<std::uint64_t>(b8 & 0x7FU) << 56U;  // bits 56..62
            value |= static_cast<std::uint64_t>(b9) << 63U;          // bit 63
        }
        if (!sink_put(sink, n, value)) {
            *aborted = true;
            *np = n;
            return q;
        }
        ++n;
        q += W;
    }
    *np = n;
    return q;
}

// Decode a whole packed-varint span, delivering each element to `sink` (see the sink docs above:
// array_sink stores to the arena array, a callback sink invokes a streaming field callback). Returns
// the element count; SIZE_MAX on a MALFORMED varint (with *err/*fail_off set); or the count decoded so
// far if a bool-returning sink aborted (the sink records what to report). A void-returning sink never
// aborts, so its abort machinery folds away and the kernels keep their vectorized/fused stores.
template <class Sink>
RP_FLATTEN inline std::size_t decode_packed_varints(const std::uint8_t* p, const std::uint8_t* end,
                                                    const std::uint8_t* begin, WireError* err,
                                                    std::size_t* fail_off, Sink sink) noexcept {
    std::size_t n = 0;
    const std::uint8_t* q = p;
    // Tiny spans skip the classifier/dispatch scaffold entirely and go straight to the byte-loop tail: a
    // field of a few packed elements (a small repeated-int array in a record-heavy payload) must cost no
    // more than the plain per-element loop, not a classify + 5-way kernel dispatch. packed_strategy would
    // return ByteLoop here anyway; the guard just elides the scaffold that a tiny array can't amortize.
    if (static_cast<std::size_t>(end - p) >= 256) {
        for (;;) {
            const std::uint8_t* const q_start = q;
            int width = 0;
            const PackedKernel kern = packed_strategy(q, static_cast<std::size_t>(end - q), &width);
            if (kern == PackedKernel::Bulk1) {
                while (static_cast<std::size_t>(end - q) >= 64) {
                    std::uint64_t any = 0;
                    for (int j = 0; j < 8; ++j) {
                        any |= swar_detail::load64(q + (8 * j)) & swar_detail::kMSB;
                    }
                    if (any == 0) {  // all 64 bytes are 1-byte varints -> plain widen (vectorizes)
                        for (int i = 0; i < 64; ++i) {
                            if (!sink_put(sink, n + static_cast<std::size_t>(i), q[i])) {
                                return n + static_cast<std::size_t>(i);
                            }
                        }
                        q += 64;
                        n += 64;
                    } else {
                        break;  // not all 1-byte after all (unrepresentative first window) -> byte-loop tail
                    }
                }
            } else if (kern == PackedKernel::Bulk2) {
                while (static_cast<std::size_t>(end - q) >= 8) {
                    const std::uint64_t w = swar_detail::load64(q);
                    if ((w & swar_detail::kMSB) ==
                        0x0080008000800080ULL) {  // exactly four 2-byte varints
                        const std::uint64_t x = w & swar_detail::kLow7;
                        const std::uint64_t y =
                            (x & 0x007F007F007F007FULL) | ((x & 0x7F007F007F007F00ULL) >> 1U);
                        if (!sink_put(sink, n + 0, static_cast<std::uint16_t>(y))) {
                            return n + 0;
                        }
                        if (!sink_put(sink, n + 1, static_cast<std::uint16_t>(y >> 16U))) {
                            return n + 1;
                        }
                        if (!sink_put(sink, n + 2, static_cast<std::uint16_t>(y >> 32U))) {
                            return n + 2;
                        }
                        if (!sink_put(sink, n + 3, static_cast<std::uint16_t>(y >> 48U))) {
                            return n + 3;
                        }
                        q += 8;
                        n += 4;
                    } else {
                        break;  // not four 2-byte varints (unrepresentative first window) -> byte-loop tail
                    }
                }
            } else if (kern == PackedKernel::Fixed) {
                // Homogeneous 3..10-byte: fixed stride, no per-element boundary find. Dispatched on a COMPILE-TIME
                // width so the mask/shifts specialize (a runtime width was much slower for the narrow cases). 9/10
                // byte varints span > 1 word (fixed_fill_wide). The guard bails to the byte-loop tail on any width
                // change, so it is correct on any data.
                bool aborted = false;
                switch (width) {
                    case 3:
                        q = fixed_fill<3>(q, end, &n, &aborted, sink);
                        break;
                    case 4:
                        q = fixed_fill<4>(q, end, &n, &aborted, sink);
                        break;
                    case 5:
                        q = fixed_fill<5>(q, end, &n, &aborted, sink);
                        break;
                    case 6:
                        q = fixed_fill<6>(q, end, &n, &aborted, sink);
                        break;
                    case 7:
                        q = fixed_fill<7>(q, end, &n, &aborted, sink);
                        break;
                    case 8:
                        q = fixed_fill<8>(q, end, &n, &aborted, sink);
                        break;
                    case 9:
                        q = fixed_fill_wide<9>(q, end, &n, &aborted, sink);
                        break;
                    default:  // width == 10 (classifier guarantees 3..10)
                        q = fixed_fill_wide<10>(q, end, &n, &aborted, sink);
                        break;
                }
                if (aborted) {
                    return n;  // sink aborted mid-fixed-run (streaming); never taken for a void arena sink
                }
            } else if (kern == PackedKernel::Struct) {
                // Narrow-mixed: one stopmask per 64-byte block finds every terminator, then each varint is an
                // independent compact7 (no per-element stop compute) in a single fused pass over the mask. The
                // 8-byte load past byte `start` needs slack, so process while >= 72 in-span bytes remain (the
                // byte-loop tail finishes the last < 72). A > 8-byte varint in the block is decoded in place by
                // the scalar reader -- NOT bailed on -- so a lone outlier can't drop the whole span to the tail.
                while (static_cast<std::size_t>(end - q) >= 72) {
                    std::uint64_t t = swar_detail::stopmask64(q);
                    if (t == 0U) {
                        break;  // all-continuation (a varint spanning the block) -> byte-loop tail
                    }
                    int start = 0;
                    do {
                        const int b = swar_detail::ctz64(t);
                        t &= t - 1U;
                        const int len = b - start + 1;
                        if (len <= 8) {
                            const std::uint64_t lenmask =
                                (len >= 8)
                                    ? ~std::uint64_t{0}
                                    : ((std::uint64_t{1} << (8U * static_cast<unsigned>(len))) -
                                       1U);
                            if (!sink_put(sink, n,
                                          swar_detail::compact7(swar_detail::load64(q + start) &
                                                                lenmask & swar_detail::kLow7))) {
                                return n;
                            }
                            ++n;
                        } else {  // 9/10-byte varint: > 1 word, so decode (and validate) with the scalar reader
                            std::uint64_t raw = 0;
                            const std::uint8_t* const np =
                                read_varint_packed(q + start, end, &raw, err);
                            if (np == nullptr) {
                                *fail_off = static_cast<std::size_t>((q + start) - begin);
                                return SIZE_MAX;
                            }
                            if (!sink_put(sink, n, raw)) {
                                return n;
                            }
                            ++n;
                        }
                        start = b + 1;
                    } while (t != 0U);
                    q += static_cast<std::size_t>(
                        start);  // start = one past the last terminator in the block
                }
            } else if (kern == PackedKernel::Swar) {
                while (static_cast<std::size_t>(end - q) >= 8) {
                    std::uint64_t raw = 0;
                    const std::uint8_t* const np = read_varint_packed(q, end, &raw, err);
                    if (np == nullptr) {
                        *fail_off = static_cast<std::size_t>(q - begin);
                        return SIZE_MAX;
                    }
                    q = np;
                    if (!sink_put(sink, n, raw)) {
                        return n;
                    }
                    ++n;
                }
            }
            if (static_cast<std::size_t>(end - q) < 512U) {
                break;  // little left -> the scalar tail finishes it
            }
            if (static_cast<std::size_t>(q - q_start) < 4096U) {
                break;  // kernel got no traction (churny data, or a ByteLoop pick) -> byte-loop the rest
            }
            // a homogeneous kernel ran a long stretch then hit a width change: re-classify the remainder
        }
    }
    while (q < end) {  // validating scalar tail (the whole span for ByteLoop)
        std::uint64_t raw = 0;
        const std::uint8_t* const np = read_varint(q, end, &raw, err);
        if (np == nullptr) {
            *fail_off = static_cast<std::size_t>(q - begin);
            return SIZE_MAX;
        }
        q = np;
        if (!sink_put(sink, n, raw)) {
            return n;
        }
        ++n;
    }
    return n;
}

// One packed-varint element via SWAR. PRECONDITION: >= 8 readable bytes at `p` (packed-span caller
// guarantees it). A 1-byte value takes a straight-line fast path; a 2..8-byte value is decoded
// branchlessly; a 9/10-byte value (all 8 low bytes continue) falls to the validating byte reader --
// which also anchors overflow/truncation checks, so a <=8-byte value here never needs them.
inline const std::uint8_t* read_varint_packed(const std::uint8_t* p, const std::uint8_t* end,
                                              std::uint64_t* out, WireError* err) noexcept {
    if (*p < 0x80U) {  // 1-byte fast path (the common case)
        *out = *p;
        return p + 1;
    }
    const std::uint64_t w = swar_detail::load64(p);
    const std::uint64_t stop = ~w & swar_detail::kMSB;  // MSB set at each terminator byte
    if (stop == 0U) {                                   // 9/10-byte: terminator past this word
        return read_varint(p, end, out, err);
    }
    const int len = (swar_detail::ctz64(stop) >> 3U) + 1;
    const std::uint64_t mask =
        stop ^ (stop - 1U);  // low bits up to & including the first terminator
    *out = swar_detail::compact7(w & mask & swar_detail::kLow7);
    return p + len;
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

namespace wire {

// Named per-proto-type conversions for the packed-varint decode. decode_packed_varints is templated on
// the raw-varint -> element conversion; passing a NAMED functor type (one of these) instead of a fresh
// per-field lambda means the template -- and the whole kernel set it flattens in -- instantiates ONCE
// per proto scalar type, shared across every field of that type and de-duplicated across translation
// units (COMDAT-folded), rather than once per packed field. The generator selects one via
// codegen::ScalarWire::packed_conv.
struct conv_int32 {
    std::int32_t operator()(std::uint64_t r) const noexcept { return varint_to_int32(r); }
};
struct conv_int64 {
    std::int64_t operator()(std::uint64_t r) const noexcept { return varint_to_int64(r); }
};
struct conv_uint32 {
    std::uint32_t operator()(std::uint64_t r) const noexcept {
        return static_cast<std::uint32_t>(r);
    }
};
struct conv_uint64 {
    std::uint64_t operator()(std::uint64_t r) const noexcept { return r; }
};
struct conv_sint32 {
    std::int32_t operator()(std::uint64_t r) const noexcept {
        return zigzag_decode_32(static_cast<std::uint32_t>(r));
    }
};
struct conv_sint64 {
    std::int64_t operator()(std::uint64_t r) const noexcept { return zigzag_decode_64(r); }
};
struct conv_bool {
    bool operator()(std::uint64_t r) const noexcept { return varint_to_bool(r); }
};
// Packed enum element: cast the raw varint (low 32 bits) to the generated enum type, matching the
// per-element read. Open-enum semantics -- every int32 value is stored as-is, no filtering. Templated
// on the enum type, so decode_packed_varints instantiates once per enum type (shared across its fields).
template <class E>
struct conv_enum {
    E operator()(std::uint64_t r) const noexcept { return static_cast<E>(varint_to_int32(r)); }
};

}  // namespace wire

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

// Streaming packed-varint sink for wire::decode_packed_varints (the streaming decoder's element
// consumer; the arena's is wire::array_sink). Converts each raw varint via Conv and delivers it to the
// field callback through the combined dispatcher; a callback that returns a non-ok DecodeStatus aborts
// the decode, recording the status in *status for the caller to return. The bool return (vs array_sink's
// void put) keeps the kernels' abort checks live. The index argument is unused -- the callback fires in
// wire order -- but present so the one sink interface serves both decoders.
template <class Dispatch, class FieldTag, class Conv>
struct callback_sink {
    Dispatch* dispatch;
    DecodeStatus* status;
    [[nodiscard]] bool put(std::size_t /*i*/, std::uint64_t raw) const {
        const DecodeStatus st = invoke_field(*dispatch, FieldTag{}, Conv{}(raw));
        if (!st.ok()) {
            *status = st;
            return false;
        }
        return true;
    }
    [[nodiscard]] bool put2(std::size_t /*i*/, std::uint64_t r0, std::uint64_t r1) const {
        return put(0, r0) && put(0, r1);
    }
};

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
