// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// rapidproto arena runtime: the std-library-only support that generated arena decoders depend on. An
// arena decoder materializes a protobuf message into a fully-allocated, READ-ONLY object tree whose
// every node lives in a bump Arena. The tree BORROWS the input: strings and bytes are {ptr,len} views
// into the input wire buffer (no copy), so the tree is valid only while BOTH the Arena and the input
// outlive it. Callers who want a self-contained result use decode_owned, which bundles the input and a
// default Arena into one owning handle. This header amalgamates:
//   - Arena       : a SINGLE-REGION bump allocator holding the input followed by the nodes.
//   - ArenaString : a read-only string cell holding a 40-bit BACKWARD offset to its bytes in the input.
//   - ArenaArray  : the in-node storage for repeated/map fields (40-bit FORWARD offset + count).
//   - ArrayView / MapView : the ephemeral pointer views the generated accessors return.
//   - ArenaDecodeError : the failure detail a decode() reports.
//   - decode_owned : a self-contained decode returning a shared_ptr that owns the input + Arena.
// It builds on the wire reader (runtime.hpp: the rapidproto::wire readers, WireError, ByteView, the
// value helpers).
//
// INVARIANT: only trivially-destructible objects are placed in the Arena, so no destructor ever runs:
// reset() is a pointer rewind, and dropping the Arena frees everything at once.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "rapidproto/runtime.hpp"

namespace rapidproto {

// Max message-nesting depth honored by generated decoders on untrusted input (mirrors kMaxGroupDepth).
inline constexpr int kMaxDecodeDepth = 100;

// ── region-absolute offsets recovered by masking ─────────────────────────────────────────────────
// EXPERIMENTAL (arena-offset masking prototype). The input and the arena live in ONE contiguous
// region that is ALIGNED TO ITS OWN SIZE CLASS, so a reference can be a 32-bit offset from the region
// BASE, and the base is recovered from the address of the cell itself:
//
//     base   = reinterpret_cast<uintptr_t>(cell) & ~(kRegionSize - 1)
//     target = base + offset
//
// Why this and not self-relative (`target - &cell`, the previous prototype): a self-relative cell
// encodes its own address, so MOVING it silently retargets it. That made every relocation illegal and
// forced repeated message / string / map arrays to be pre-counted in an extra tag-walk per message --
// which measured -11% on Dataset and -21.7% on many-small-messages. A region-absolute cell means the
// same thing at every address inside the region, so memcpy is safe, geometric realloc-and-copy is
// safe, and the counting pre-scan disappears entirely.
//
// It keeps self-relative's one real advantage: no base REGISTER is needed at a dereference, so
// accessors keep their exact signatures and the public API is unchanged. The mask must therefore be a
// compile-time constant -- it cannot be "this arena's size" -- which is what fixes the region to one
// size class rather than sizing it per input.
//
// kRegionSize = 4 GiB caps one arena at 4 GiB and costs an aligned 4 GiB of ADDRESS SPACE (not
// memory) per arena. In exchange the offset is a naturally aligned uint32: no 5-byte packed cell, no
// 8-byte overlapping load, no tail slack. 2^36 would lift the cap to 64 GiB at 5 bytes per cell if
// 4 GiB proves too tight; see the note in Arena on why that is a real question for a 2 GiB input.
//
// Offset 0 is the null sentinel: Arena reserves the first kMaxAlign bytes of the region so no live
// object is ever AT the base. Testing the raw loaded word against 0 is the cheapest form the check
// can take (a `test`/`jz` pair that macro-fuses), and it happens before the base is even computed.
namespace offset_detail {

inline constexpr std::uint64_t kRegionSize = std::uint64_t{1} << 32;
inline constexpr std::uint64_t kRegionMask = kRegionSize - 1;
using Offset = std::uint32_t;

// The region base, recovered from any address inside the region.
inline const char* base_of(const void* cell) noexcept {
    return reinterpret_cast<const char*>(reinterpret_cast<std::uintptr_t>(cell) &
                                         ~static_cast<std::uintptr_t>(kRegionMask));
}
inline Offset load(const Offset* cell) noexcept { return *cell; }
inline void store(Offset* cell, Offset value) noexcept { *cell = value; }

// The offset to store for `target`, measured from the base of the region `cell` lives in. Direction
// is irrelevant now (that was a self-relative concern): a region-absolute offset is the same value
// wherever the cell ends up.
inline Offset offset_of(const void* cell, const void* target) noexcept {
    return static_cast<Offset>(static_cast<const char*>(target) - base_of(cell));
}
// The target of a non-zero cell.
inline const char* resolve(const void* cell, Offset off) noexcept { return base_of(cell) + off; }

}  // namespace offset_detail

// ── Arena ────────────────────────────────────────────────────────────────────────────────────────
// EXPERIMENTAL (arena-offset prototype). A SINGLE contiguous region laid out as
//   [ adopted input bytes ][ bump-allocated nodes ][ >= 8 bytes tail slack ]
// so that every reference in the tree can be a 40-bit offset relative to the cell holding it rather
// than an 8-byte pointer (see the offset_detail block below).
//
// The region is reserved ONCE, sized from the input, and never grows or moves: a realloc would
// invalidate the raw node pointers the decoder holds on its call stack. allocate() therefore returns
// nullptr on exhaustion, surfaced as ArenaDecodeError::OutOfMemory, and the caller may retry with a
// larger reserve(). reset() rewinds to just past the input (keeping it, since every string offset
// targets it). Single-threaded; storage is RAII-owned unless a caller-owned seed buffer was passed.
class Arena {
public:
    Arena() noexcept = default;
    // Seed with a caller-owned buffer (NOT freed by the Arena). Under the single-region layout this
    // IS the whole region -- input first, nodes after -- so it must be large enough for both.
    Arena(void* buffer, std::size_t size) noexcept {
        if (buffer != nullptr && size > kMaxAlign + kTailSlack) {
            adopt_buffer(buffer, size);
        }
    }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;
    ~Arena() = default;

    // EXPERIMENTAL (arena-offset prototype). Place `input` at the FRONT of the region and return the
    // in-region view the decoder must parse. A string/bytes cell holds a BACKWARD offset to its
    // bytes, so the input must precede every node -- bytes left in the caller's own buffer could not
    // be reached from a node by a self-relative offset at all.
    //
    // The region is sized ONCE here and never grows: a realloc would move nodes that the decoder is
    // still holding raw pointers to (each decode frame writes into a `Msg&`). On exhaustion
    // allocate() returns nullptr, the decode reports OutOfMemory, and the caller may retry after a
    // larger reserve() -- safe precisely because no decode is in flight at that point.
    ByteView adopt_input(ByteView input) noexcept {
        if (input.data() == nullptr) {
            return {};
        }
        // Already inside the region (a caller that placed the bytes itself): nothing to copy.
        if (m_base != nullptr && input.data() >= m_base && input.data() < m_limit) {
            m_input_end = m_cur;
            return input;
        }
        if (m_base == nullptr && !reserve(estimate_region(input.size()))) {
            return {};
        }
        // Nothing is live above the previously adopted input (fresh, or just reset()), so drop that
        // copy instead of stacking a second one after it -- otherwise every warm re-decode of an
        // input held OUTSIDE the region would consume another input's worth and exhaust the region
        // after a handful of iterations.
        if (m_cur == m_input_end) {
            m_cur = m_base + kMaxAlign;  // keep the base burned (see reserve())
            m_used = 0;
            m_input_end = nullptr;
        }
        void* const dst = allocate(input.size(), kMaxAlign);
        if (dst == nullptr) {
            return {};
        }
        std::memcpy(dst, input.data(), input.size());
        m_input_end = m_cur;
        return ByteView{static_cast<const char*>(dst), input.size()};
    }

    // Reserve the single region up front (plus alignment and the offset-read tail slack). Returns
    // false on host OOM or the capacity cap. Only meaningful before anything has been allocated.
    // EXPERIMENTAL (masking prototype). The region must start on a kRegionSize boundary, because that
    // is what lets a dereference recover the base by masking the cell's own address. So `bytes` is
    // ignored beyond a capacity check: we take one kRegionSize-aligned kRegionSize block and let the
    // OS back it lazily -- untouched pages cost address space, not memory. That deliberately sidesteps
    // reserve-and-commit-on-demand for now; it gives a LOWER BOUND on performance (no growth, no
    // realloc, no first-touch of memory we never use) which is all the prototype needs to measure.
    //
    // The first kMaxAlign bytes are burned so that offset 0 can never name a live object and stays
    // available as the null sentinel.
    bool reserve(std::size_t bytes) noexcept {
        if (m_base != nullptr) {
            return bytes <= static_cast<std::size_t>(m_limit - m_cur);
        }
        if (bytes > kRegionUsable) {
            return false;  // beyond one region -> OOM (a 4 GiB cap is a known limit of this scheme)
        }
        if (m_reserved >= m_cap || kRegionUsable > m_cap - m_reserved) {
            return false;  // capacity cap reached -> treat as OOM (the only injectable failure)
        }
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, hicpp-no-malloc): aligned_alloc is the only
        // portable way to demand a kRegionSize-aligned block; the deleter below is its free().
        void* const raw = std::aligned_alloc(offset_detail::kRegionSize, offset_detail::kRegionSize);
        if (raw == nullptr) {
            return false;
        }
        m_storage.reset(static_cast<char*>(raw));
        m_base = m_storage.get();  // exactly kRegionSize-aligned: masking recovers precisely this
        m_cur = m_base + kMaxAlign;  // burn the base so offset 0 is never a live object
        m_limit = m_base + kRegionUsable;
        m_reserved += kRegionUsable;
        return true;
    }

    // Aligned, uninitialized bytes. `align` must be a power of two. nullptr once the region is
    // exhausted (the region never grows -- see adopt_input).
    void* allocate(std::size_t bytes, std::size_t align) noexcept {
        assert(align != 0 && (align & (align - 1)) == 0 && "align must be a power of two");
        char* const p = align_up(m_cur, align);
        // Size comparison, never `p + bytes`: forming that pointer is UB on an empty arena (p is
        // null) or when `bytes` is a huge untrusted value (it overflows past the end of the object).
        if (m_cur != nullptr && p <= m_limit && bytes <= static_cast<std::size_t>(m_limit - p)) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the bump allocation
            m_cur = p + bytes;
            m_used += bytes;
            return p;
        }
        return nullptr;
    }

    // Uninitialized storage for `n` Ts (the generated tree is trivially destructible).
    template <class T>
    T* allocate_array(std::size_t n) noexcept {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destructed");
        if (n == 0 || n > SIZE_MAX / sizeof(T)) {  // reject zero and a count that would overflow
            return nullptr;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bump-allocated typed storage
        return reinterpret_cast<T*>(allocate(n * sizeof(T), alignof(T)));
    }

    // Construct one T in the arena (trivially destructible only). Returns nullptr on OOM.
    template <class T, class... Args>
    T* create(Args&&... args) noexcept {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destructed");
        void* const mem = allocate(sizeof(T), alignof(T));
        if (mem == nullptr) {
            return nullptr;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): arena-owned, see above
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    // Return the unused tail of the MOST-RECENT allocation (see the packed-array decode). A no-op
    // unless `ptr`'s end is exactly the current bump cursor.
    void shrink_last(void* ptr, std::size_t old_bytes, std::size_t new_bytes) noexcept {
        if (ptr == nullptr || new_bytes > old_bytes) {
            return;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): tail check/trim, last block
        char* const base = static_cast<char*>(ptr);
        if (base + old_bytes == m_cur) {  // still the last allocation -> reclaim the unused tail
            m_cur = base + new_bytes;
            m_used -= (old_bytes - new_bytes);
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }

    // Rewind for reuse, KEEPING the adopted input in place (it is the target of every string
    // offset), so a warm re-decode pays no copy -- provided the caller hands decode() the view that
    // adopt_input returned, i.e. one already inside the region. Re-decoding from the ORIGINAL
    // outside-the-region bytes re-copies them over this one (see adopt_input).
    void reset() noexcept {
        if (m_input_end != nullptr) {
            m_cur = m_input_end;
            m_used = static_cast<std::size_t>(m_input_end - m_base);
        } else {
            m_cur = m_base + kMaxAlign;  // keep the base burned: offset 0 stays the null sentinel
            m_used = 0;
        }
    }

    [[nodiscard]] std::size_t bytes_used() const noexcept { return m_used; }
    [[nodiscard]] std::size_t bytes_reserved() const noexcept { return m_reserved; }

    // Cap the total host memory the arena reserves; a reserve past it fails as if on host OOM, so
    // the decode returns ArenaDecodeError::OutOfMemory. Default: unbounded.
    void set_capacity_limit(std::size_t max_reserved_bytes) noexcept { m_cap = max_reserved_bytes; }

private:
    static constexpr std::size_t kMaxAlign = alignof(std::max_align_t);
    // The self-relative prototype read its 5-byte cell with an overlapping 8-byte load and needed
    // slack past the region end for it. A uint32 cell is read exactly, so no slack is required.
    static constexpr std::size_t kTailSlack = 0;
    // Usable bytes in one region: everything after the burned base (see reserve()).
    static constexpr std::size_t kRegionUsable =
        static_cast<std::size_t>(offset_detail::kRegionSize) - kMaxAlign;
    static constexpr std::size_t kMinRegion = 4096;
    // A decoded tree typically costs a few times the wire bytes it came from. The region cannot grow,
    // so this errs HIGH: over-reserving costs address space, under-reserving costs a whole retry.
    // 12 is the smallest multiple that decodes the whole bench suite (1-byte varints expanding into
    // 8-byte array elements are the worst case). It is NOT a free knob to raise: because the region is
    // eagerly allocated and first-touched, over-reserving measurably slows the array sweeps -- at 24
    // they lose another 30-40% of throughput on top. That sensitivity is the strongest argument that a
    // productized version must reserve ADDRESS SPACE and commit on demand rather than pick a multiple.
    static constexpr std::size_t kExpansion = 12;

    static std::size_t estimate_region(std::size_t input_bytes) noexcept {
        if (input_bytes > SIZE_MAX / kExpansion) {
            return SIZE_MAX / kExpansion;  // absurd input -> let reserve() fail cleanly
        }
        const std::size_t want = input_bytes * kExpansion + kMinRegion;
        return want < kMinRegion ? kMinRegion : want;
    }

    static char* align_up(char* p, std::size_t align) noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): bump-pointer alignment
        return p == nullptr
                   ? nullptr
                   : reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(p) + (align - 1)) &
                                             ~(align - 1));
    }

    // EXPERIMENTAL (masking prototype): a caller-owned seed buffer only works if it is already
    // kRegionSize-aligned, since masking recovers the base from that alignment alone. Anything else is
    // ignored and the Arena falls back to allocating its own region on first use.
    void adopt_buffer(void* buffer, std::size_t size) noexcept {
        char* const raw = static_cast<char*>(buffer);
        if ((reinterpret_cast<std::uintptr_t>(raw) & offset_detail::kRegionMask) != 0) {
            return;
        }
        m_storage = nullptr;  // caller owns it
        m_base = raw;
        m_cur = m_base + kMaxAlign;  // burn the base so offset 0 is never a live object
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): end of the seed buffer
        m_limit = raw + size;
        m_reserved += static_cast<std::size_t>(m_limit - m_base);
    }

    // aligned_alloc'd, so it must be free()d -- not delete[]d.
    struct FreeDeleter {
        void operator()(char* p) const noexcept {
            std::free(p);  // NOLINT(cppcoreguidelines-no-malloc, hicpp-no-malloc)
        }
    };
    std::unique_ptr<char[], FreeDeleter> m_storage;  // null => caller-owned seed buffer (not freed)
    char* m_base = nullptr;       // first usable byte (kMaxAlign-aligned)
    char* m_cur = nullptr;        // bump cursor
    char* m_limit = nullptr;      // last allocatable byte (region end minus kTailSlack)
    char* m_input_end = nullptr;  // end of the adopted input; reset() rewinds here
    std::size_t m_used = 0;
    std::size_t m_reserved = 0;
    std::size_t m_cap = SIZE_MAX;  // capacity cap on m_reserved; SIZE_MAX = unbounded (the default)
};


// ── ArenaString ──────────────────────────────────────────────────────────────────────────────────
// A pure 12-byte read-only string that BORROWS a {ptr,len} view into the input wire buffer -- strings
// and bytes are zero-copy. The decoded tree therefore borrows the input (on top of the arena): a
// string_view it yields is valid only while the input outlives the tree. This trades the
// self-contained-tree guarantee for no per-string memcpy and no string bytes in the arena; callers who
// want a self-contained result use decode_owned (which owns the input alongside the arena).
// Trivially copyable/destructible.
//
// Layout: a `char[8]` pointer (align 1) + a `uint32` length -> sizeof 12, alignof 4. Storing the
// pointer as a byte array (read/written via memcpy) instead of a `const char*` avoids the 8-byte
// alignment that would pad the cell back to 16; the 4-aligned 12-byte cell lets the layout planner
// pack a string field next to other 4-byte fields with no gap. A borrow never inlines, so there is no
// SSO. The length is 32-bit: a protobuf message never exceeds ~2 GiB (the top-level decode rejects an
// input over UINT32_MAX bytes up front, see rp_fail_input_too_large), so no string span within it can
// either. If the size changes, update arenagen's kStringSize to match (a static_assert there enforces
// it).
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// bugprone-multi-level-implicit-pointer-conversion, bugprone-sizeof-expression): the pointer is a raw
// char[8] cell, type-punned through memcpy (the sanctioned way, &ptr -> void*) to hold align 1;
// sizeof(a pointer) is exactly what we mean.
class ArenaString {
public:
    ArenaString() noexcept = default;  // empty view

    // `s` points into the input, which the Arena placed at the front of the region, so its offset is
    // non-zero (the base itself is reserved) and offset 0 keeps meaning "unset" -- which the
    // raw-payload presence check relies on. Written in place for symmetry with the other cells,
    // though a region-absolute cell would in fact survive being copied.
    static void store(ArenaString* dst, ByteView s) noexcept {
        if (s.size() > UINT32_MAX || s.data() == nullptr) {
            *dst = ArenaString{};  // unreachable under the input-size guard; stay defined anyway
            return;
        }
        dst->m_off = offset_detail::offset_of(dst, s.data());
        dst->m_len = static_cast<std::uint32_t>(s.size());
    }

    // The borrowed pointer (null only on a default/unset ArenaString). A raw message-payload field
    // uses this for presence: a present payload borrows a non-null input pointer even when empty,
    // while an unset field keeps the null default.
    [[nodiscard]] const char* data() const noexcept {
        return m_off == 0 ? nullptr : offset_detail::resolve(this, m_off);
    }
    [[nodiscard]] std::string_view view() const noexcept { return {data(), size()}; }
    [[nodiscard]] std::size_t size() const noexcept { return m_len; }
    [[nodiscard]] bool empty() const noexcept { return m_len == 0; }

private:
    // Two naturally aligned uint32s: sizeof 8, alignof 4. The self-relative prototype needed byte
    // arrays here to keep a 5-byte offset at align 1; a region-absolute offset fits a plain uint32,
    // so the cell is both smaller AND aligned, with no memcpy type-punning and no tail slack.
    offset_detail::Offset m_off = 0;  // offset from the region base to the bytes; 0 == unset
    std::uint32_t m_len = 0;  // length; a protobuf span never exceeds ~2 GiB, so 32 bits always fits
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// bugprone-multi-level-implicit-pointer-conversion, bugprone-sizeof-expression)
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaString) == 8 && alignof(ArenaString) == 4 &&
              std::is_trivially_destructible_v<ArenaString>);

// ── ArrayView / MapView ──────────────────────────────────────────────────────────────────────────
// A read-only contiguous view into the arena, returned by repeated-field accessors. This is the
// runtime's own {ptr,len} view (the std::span stand-in the parser calls Range -- the two are kept
// separate only because this header ships self-contained and cannot include the parser's range.hpp;
// unifying them would couple this header to range.hpp and is not obviously worth it).
//
// A genuinely 12-byte, 4-aligned cell: an 8-byte data pointer plus a uint32 element count. The
// pointer lives in a char[8] (memcpy'd in/out) rather than a `const T*` member on purpose -- a real
// pointer member would force 8-byte alignment and pad the struct back to 16. The unaligned 8-byte
// load the memcpy compiles to is fine on x86-64/ARM64, and the memcpy keeps it strict-aliasing- and
// alignment-UB-free. The view points at self-contained ARENA storage, so shrinking it changes no
// ownership contract; the only cost is the count narrowing to uint32 -- the top-level decode rejects
// any input over UINT32_MAX bytes up front (see rp_fail_input_too_large), which bounds every element
// count to uint32 (each entry costs >=1 input byte). data() is called once where it matters
// (begin/end) so the compiler hoists the memcpy out of loops.
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// cppcoreguidelines-pro-bounds-pointer-arithmetic): the class IS a hand-laid-out 12-byte cell whose
// pointer is type-punned through memcpy (the sanctioned way), and the span abstraction itself.
// The in-node storage for a sub-message reference (the layout planner's PointerSubMsg): a bare
// 32-bit region-absolute offset, 4 bytes instead of an 8-byte pointer. 0 means unset (the region base
// is reserved, so no node ever sits there).
template <class T>
class ArenaPtr {
public:
    ArenaPtr() noexcept = default;

    static void store(ArenaPtr* dst, const T* target) noexcept {
        dst->m_off = target == nullptr ? 0 : offset_detail::offset_of(dst, target);
    }

    [[nodiscard]] const T* get() const noexcept {
        return m_off == 0 ? nullptr
                          : reinterpret_cast<const T*>(offset_detail::resolve(this, m_off));
    }

    // Presence WITHOUT forming the pointer. `get() != nullptr` makes the compiler prove that
    // `this + off` can never be null before it can fold the test down to `off != 0`; this asks the
    // question directly, which is what the decoder's duplicate-singular guard wants on the hot path.
    [[nodiscard]] bool is_set() const noexcept { return m_off != 0; }

private:
    offset_detail::Offset m_off = 0;  // offset from the region base to the node; 0 == unset
};

// STORAGE vs VIEW. A self-relative cell must never be copied -- a copy sits at a different address,
// so its offset would silently point somewhere else. ArenaArray<T> is therefore the in-node STORAGE
// (9 bytes, self-relative, never copied), and ArrayView<T> is the ephemeral pointer-based view an
// accessor RETURNS by value. Only the storage needs to be small; the view lives in registers.
template <class T>
class ArrayView {
public:
    ArrayView() noexcept = default;
    ArrayView(const T* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    const T& operator[](std::size_t i) const noexcept { return m_data[i]; }
    [[nodiscard]] const T* begin() const noexcept { return m_data; }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the span abstraction itself
    [[nodiscard]] const T* end() const noexcept { return m_data + m_size; }

private:
    const T* m_data = nullptr;
    std::size_t m_size = 0;
};

// The in-node array storage: a 32-bit region-absolute offset to the elements plus a 32-bit count --
// two naturally aligned uint32s, sizeof 8, alignof 4.
//
// Unlike the self-relative prototype this cell MAY be copied: it means the same thing at any address
// inside the region. That is what lets a repeated field grow by geometric realloc-and-copy again --
// moving elements no longer corrupts the cells inside them, so no counting pre-scan is needed.
template <class T>
class ArenaArray {
public:
    ArenaArray() noexcept = default;

    static void store(ArenaArray* dst, const T* data, std::size_t size) noexcept {
        if (data == nullptr || size == 0) {
            *dst = ArenaArray{};
            return;
        }
        dst->m_off = offset_detail::offset_of(dst, data);
        dst->m_size = static_cast<std::uint32_t>(size);
    }

    [[nodiscard]] const T* data() const noexcept {
        return m_off == 0 ? nullptr
                          : reinterpret_cast<const T*>(offset_detail::resolve(this, m_off));
    }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    // The ephemeral view an accessor hands out. Reading through it never touches this cell again.
    [[nodiscard]] ArrayView<T> view() const noexcept { return {data(), size()}; }

private:
    offset_detail::Offset m_off = 0;  // offset from the region base to the elements; 0 == unset
    std::uint32_t m_size = 0;
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaArray<int>) == 8 && alignof(ArenaArray<int>) == 4 &&
              std::is_trivially_destructible_v<ArenaArray<int>>);

// ── StringArrayView ────────────────────────────────────────────────────────────────────────────────
// A read-only view over an arena array of ArenaString that yields std::string_view per element. A
// repeated string/bytes accessor returns this, so consumers see std::string_view and never the internal
// ArenaString storage type; each element borrows a {ptr,len} view into the input (no copy).
class StringArrayView {
public:
    StringArrayView() noexcept = default;
    explicit StringArrayView(ArrayView<ArenaString> strings) noexcept : m_strings(strings) {}

    [[nodiscard]] std::size_t size() const noexcept { return m_strings.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_strings.empty(); }
    std::string_view operator[](std::size_t i) const noexcept { return m_strings[i].view(); }

    // Dereferences to a std::string_view by value, so it is an input iterator (a value proxy, not a
    // reference). That covers range-for and single-pass use; random access is via operator[].
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = std::string_view;

        iterator() noexcept = default;
        explicit iterator(const ArenaString* p) noexcept : m_p(p) {}
        std::string_view operator*() const noexcept { return m_p->view(); }
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): an iterator's advance
        iterator& operator++() noexcept {
            ++m_p;
            return *this;
        }
        iterator operator++(int) noexcept {
            iterator prev = *this;
            ++m_p;
            return prev;
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        bool operator==(const iterator& o) const noexcept { return m_p == o.m_p; }
        bool operator!=(const iterator& o) const noexcept { return m_p != o.m_p; }

    private:
        const ArenaString* m_p = nullptr;
    };

    [[nodiscard]] iterator begin() const noexcept { return iterator(m_strings.begin()); }
    [[nodiscard]] iterator end() const noexcept { return iterator(m_strings.end()); }

private:
    ArrayView<ArenaString> m_strings;
};

// A read-only map view over insertion-order entries (a generated `Entry` exposing `.key()`).
// find() is a last-wins linear scan (protobuf maps take the last value for a duplicate key).
template <class Entry>
class MapView {
public:
    MapView() noexcept = default;
    explicit MapView(ArrayView<Entry> entries) noexcept : m_entries(entries) {}

    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] const Entry* begin() const noexcept { return m_entries.begin(); }
    [[nodiscard]] const Entry* end() const noexcept { return m_entries.end(); }

    template <class Key>
    [[nodiscard]] const Entry* find(const Key& key) const noexcept {
        const Entry* hit = nullptr;
        for (const Entry& e : m_entries) {
            if (e.key() == key) {
                hit = &e;  // keep scanning: last duplicate wins
            }
        }
        return hit;
    }

private:
    ArrayView<Entry> m_entries;
};
// MapView is an ephemeral VIEW (it wraps an ArrayView), so its size does not matter -- a map field's
// in-node STORAGE is an ArenaArray<Entry>, which is the 8-byte cell the layout planner sizes against.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaArray<ArenaString>) == 8 && alignof(ArenaArray<ArenaString>) == 4 &&
              std::is_trivially_destructible_v<ArenaArray<ArenaString>>);
static_assert(std::is_trivially_destructible_v<MapView<ArenaString>>);

// ── Decode failure ────────────────────────────────────────────────────────────────────────────────
struct ArenaDecodeError {
    enum class Code : std::uint8_t {
        None,
        Wire,                     // a WireError from the underlying reader (see `wire` / `offset`)
        OutOfMemory,              // the Arena could not satisfy an allocation
        RecursionTooDeep,         // message nesting exceeded kMaxDecodeDepth
        MissingRequired,          // a proto2 `required` field was absent (see `field_number`)
        RepeatedSingularMessage,  // a singular message field occurred more than once (see field_number)
        StringTooLong,  // reserved (unreachable): strings borrow the input (never copied) and an input
        // over 4 GiB is rejected as InputTooLarge, so no string store fails; kept for
        // Code-value stability
        InputTooLarge,  // input exceeds the 4 GiB (UINT32_MAX) size at which element counts / string
                        // lengths stay representable
    };

    Code code = Code::None;
    WireError wire = WireError::None;  // valid when code == Wire
    std::size_t offset = 0;            // byte offset of a wire failure
    std::uint32_t field_number = 0;    // the missing field, when code == MissingRequired

    [[nodiscard]] constexpr bool ok() const noexcept { return code == Code::None; }
};

// ── decode-support helpers (used by generated decoders) ──────────────────────────────────────────────
// Each records a failure into `err` (if non-null) and is meant to be followed by `return false`/
// `nullptr` at the call site. A null `err` is the "I don't care why it failed" fast path.
// Record a wire failure from an explicit (code, offset) -- the value-threaded decode loops read from a
// raw cursor and compute the offset from the cursor they passed in.
inline void rp_fail_wire_at(ArenaDecodeError* err, WireError code, std::size_t offset) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::Wire;
        err->wire = code;
        err->offset = offset;
    }
}
inline void rp_fail_oom(ArenaDecodeError* err) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::OutOfMemory;
    }
}
// The input exceeds UINT32_MAX bytes. Every repeated/map entry costs >=1 input byte, so a
// <=UINT32_MAX input bounds every element count to the uint32 ArrayView/MapView size (and every
// string length); the top-level decode rejects a larger input up front rather than risk a narrowing
// truncation. Not reachable by any practical test buffer (a >4 GiB input); a defensive guard.
inline void rp_fail_input_too_large(ArenaDecodeError* err) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::InputTooLarge;
    }
}
inline void rp_fail_recursion(ArenaDecodeError* err) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::RecursionTooDeep;
    }
}
inline void rp_fail_missing_required(ArenaDecodeError* err, std::uint32_t field_number) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::MissingRequired;
        err->field_number = field_number;
    }
}
inline void rp_fail_repeated_singular(ArenaDecodeError* err, std::uint32_t field_number) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::RepeatedSingularMessage;
        err->field_number = field_number;
    }
}

// ── decode_owned: a self-contained decode ────────────────────────────────────────────────────────
// Decode `input` into a tree that OWNS its inputs: the returned handle keeps both the moved-in input
// bytes and a default Arena alive, so every borrowed string_view stays valid for as long as any copy
// of the handle lives -- no external lifetime to manage. Uses shared_ptr's aliasing constructor: the
// control block owns {input, arena}, the shared_ptr points at the decoded root. Returns an empty
// shared_ptr on decode failure (err, if non-null, carries the reason). For a caller-managed Arena, or
// to decode a string_view you own without moving it, use the low-level T::decode(ByteView, Arena&)
// directly and keep the input alive yourself.
template <class T>
[[nodiscard]] std::shared_ptr<const T> decode_owned(std::string input,
                                                    ArenaDecodeError* err = nullptr) {
    struct OwnedArena {
        std::string
            input;  // moved-in; .data() is stable (heap-pinned in the control block, never mutated)
        Arena arena;  // default-constructed in place; address-stable, holds the tree structure
    };
    auto bundle = std::make_shared<OwnedArena>();
    bundle->input = std::move(input);
    const T* root =
        T::decode(ByteView{bundle->input.data(), bundle->input.size()}, bundle->arena, err);
    if (root == nullptr) {
        return {};  // decode failed: drop the bundle, return empty
    }
    return std::shared_ptr<const T>(std::move(bundle),
                                    root);  // aliasing ctor: share bundle, expose root
}

// ── decoder reach-through (keeps the decoder off the generated public surface) ───────────────────────
// Generated decoders live in a private `rp_decode_into` static; this one-line forwarder is the only
// caller, and every message befriends the template. Defined ONCE here, not per generated file, so two
// same-package headers in one TU don't redefine it, and every cross-file/cross-message call is the same
// `::rapidproto::arena_detail::decode_into(sub, ...)` regardless of the target's namespace (deduced).
namespace arena_detail {
template <class T>
[[nodiscard]] bool decode_into(T& out, ByteView body, Arena& arena, int depth,
                               ArenaDecodeError* err) noexcept {
    return T::rp_decode_into(out, body, arena, depth, err);
}

// True when this platform's byte order matches protobuf's fixed32/64 wire encoding (little-endian),
// so a packed fixed-width array's wire bytes ARE its in-memory image and the decoder can bulk-copy
// the span in one memcpy instead of reading each element. On a big-endian (or unknown-endian)
// target this is false and the decoder falls back to the byte-swapping per-element read. C++17 has
// no std::endian; __BYTE_ORDER__ is defined by every supported compiler, Windows is always
// little-endian, and anything else conservatively takes the safe per-element path.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
// NOLINTNEXTLINE(misc-redundant-expression): the two macros are equal on a LE build -- that IS the test
inline constexpr bool kFixedIsNativeLE = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#elif defined(_WIN32)
inline constexpr bool kFixedIsNativeLE = true;
#else
inline constexpr bool kFixedIsNativeLE = false;
#endif

// The SWAR-kernel packed-varint decode for a LARGE span, pulled OUT of line -- shared across every
// `repeated <varint-type>` field of this element type (and across messages/TUs, since the instantiation
// is keyed only on <Elem, Conv>). RP_NOINLINE is the point: the generated decode() is flattened, so an
// inline copy of this ~2k-instruction kernel would be duplicated at every packed field -- code that grows
// linearly with the field count (a large schema's decode() balloons) and, on gcc, degrades register
// allocation of the adjacent tail. One out-of-line copy, one call per large-span field, keeps decode()
// small. Only the kernel is out-of-lined: the generated caller keeps the array grow, the SMALL-span tail
// (the common repeated-field case), and the trim inline, so tiny arrays pay no call. The array is already
// grown to fit; this decodes into `dst` (= acc + n) and returns the element count, or SIZE_MAX on a
// malformed varint (`err` set). `dst` is passed by value (no caller address escapes -> no spill). Guarded
// to spans >= 256 by the caller, which is also decode_packed_varints's own kernel-vs-byte-loop threshold,
// so the kernels always engage here (a sub-256 span would only run the byte-loop tail, done inline).
template <class Elem, class Conv>
RP_NOINLINE std::size_t decode_packed_varints_large(const std::uint8_t* vp, const std::uint8_t* ve,
                                                    Elem* dst, ArenaDecodeError* err) noexcept {
    WireError we = WireError::None;
    std::size_t fo = 0;
    const std::size_t dc =
        wire::decode_packed_varints(vp, ve, vp, &we, &fo, wire::array_sink<Elem, Conv>{dst});
    if (dc == static_cast<std::size_t>(-1)) {
        rp_fail_wire_at(err, we, fo);
    }
    return dc;
}

// The validating byte-loop for a SMALL packed-varint span (< 256 bytes, below decode_packed_varints's
// kernel threshold -- a large-span kernel would never engage). RP_FLATTEN so it INLINES into the
// generated decode(): a small array is the common repeated-field shape, and inlining keeps its decode a
// handful of instructions with no call and no kernel-dispatch setup. This is byte-for-byte
// decode_packed_varints's own tail (same array_sink read/convert/store, same span-relative fail offset),
// just without the kernel scaffold a sub-256 span can't use. Decodes into `dst`; returns the element
// count or SIZE_MAX on a malformed varint (`err` set).
template <class Elem, class Conv>
RP_FLATTEN std::size_t decode_packed_varints_small(const std::uint8_t* vp, const std::uint8_t* ve,
                                                   Elem* dst, ArenaDecodeError* err) noexcept {
    const wire::array_sink<Elem, Conv> sink{dst};
    const std::uint8_t* const begin = vp;
    std::size_t n = 0;
    while (vp < ve) {
        std::uint64_t raw = 0;
        WireError we = WireError::None;
        const std::uint8_t* const np = wire::read_varint(vp, ve, &raw, &we);
        if (np == nullptr) {
            rp_fail_wire_at(err, we, static_cast<std::size_t>(vp - begin));
            return static_cast<std::size_t>(-1);
        }
        vp = np;
        sink.put(n, raw);
        ++n;
    }
    return n;
}
}  // namespace arena_detail

}  // namespace rapidproto
