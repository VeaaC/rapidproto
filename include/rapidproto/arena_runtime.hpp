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
            m_cur = m_base;
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
    bool reserve(std::size_t bytes) noexcept {
        if (m_base != nullptr) {
            return bytes <= static_cast<std::size_t>(m_limit - m_cur);
        }
        if (bytes > static_cast<std::size_t>(PTRDIFF_MAX) - kMaxAlign - kTailSlack) {
            return false;  // too large to represent -> treat as OOM
        }
        const std::size_t raw = bytes + kMaxAlign + kTailSlack;
        if (m_reserved >= m_cap || raw > m_cap - m_reserved) {
            return false;  // capacity cap reached -> treat as OOM (the only injectable failure)
        }
        std::unique_ptr<char[]> storage(new (std::nothrow) char[raw]);  // nothrow -> null on OOM
        if (!storage) {
            return false;
        }
        m_storage = std::move(storage);
        m_base = align_up(m_storage.get(), kMaxAlign);
        m_cur = m_base;
        m_limit = m_storage.get() + raw - kTailSlack;
        m_reserved += static_cast<std::size_t>(m_limit - m_base);
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
            m_cur = m_base;
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
    // A 5-byte offset cell is read with ONE 8-byte load, so no allocation may come within 8 bytes of
    // the region end -- the over-read then always stays inside the same allocation.
    static constexpr std::size_t kTailSlack = 8;
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

    void adopt_buffer(void* buffer, std::size_t size) noexcept {
        char* const raw = static_cast<char*>(buffer);
        m_storage = nullptr;  // caller owns it
        m_base = align_up(raw, kMaxAlign);
        m_cur = m_base;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): end of the seed buffer
        m_limit = raw + size - kTailSlack;
        m_reserved += static_cast<std::size_t>(m_limit - m_base);
    }

    // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    std::unique_ptr<char[]> m_storage;  // null => caller-owned seed buffer (not freed)
    // NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    char* m_base = nullptr;       // first usable byte (kMaxAlign-aligned)
    char* m_cur = nullptr;        // bump cursor
    char* m_limit = nullptr;      // last allocatable byte (region end minus kTailSlack)
    char* m_input_end = nullptr;  // end of the adopted input; reset() rewinds here
    std::size_t m_used = 0;
    std::size_t m_reserved = 0;
    std::size_t m_cap = SIZE_MAX;  // capacity cap on m_reserved; SIZE_MAX = unbounded (the default)
};

// ── 40-bit self-relative offsets ─────────────────────────────────────────────────────────────────
// EXPERIMENTAL (arena-offset prototype). The input and the arena live in ONE contiguous region, so a
// reference can be a 40-bit offset relative to the CELL that holds it instead of an 8-byte pointer.
//
// Self-relative rather than base-relative: `this + off` needs no base register, so accessors keep
// their exact signatures and the public API is unchanged. It also makes 0 a free null sentinel -- an
// offset of 0 would point at the cell itself, which is never a valid target.
//
// UNSIGNED, with the direction implied by the field kind: message / array / map references always
// point FORWARD (a parent node is allocated before its children, and a grown array reallocates
// further forward), while string / bytes always point BACKWARD into the input region that precedes
// the nodes. Unsigned costs one mask instead of the two shifts sign-extension needs, and doubles
// reach to 1 TiB -- enough for a 2 GiB input expanding into a much larger arena, which a 32-bit
// offset could not promise.
namespace offset_detail {

inline constexpr int kOffsetBits = 40;
inline constexpr std::size_t kOffsetBytes = 5;
inline constexpr std::uint64_t kMaxOffset = (std::uint64_t{1} << kOffsetBits) - 1;

// Read the 5-byte little-endian cell with ONE 8-byte load and a mask. This over-reads 3 bytes past
// the cell, which is why the arena guarantees >= 8 bytes of tail slack (see Arena::kTailSlack): the
// read then stays inside the same allocation, so it is well-defined and ASan-clean. The alternative
// (a 4-byte + 1-byte load) costs an extra load, shift and or on every dereference.
inline std::uint64_t load(const char* cell) noexcept {
    std::uint64_t raw = 0;
    std::memcpy(&raw, cell, sizeof raw);
    return raw & kMaxOffset;
}
inline void store(char* cell, std::uint64_t value) noexcept {
    std::memcpy(cell, &value, kOffsetBytes);  // little-endian: the low 5 bytes
}

// The distance from `cell` forward to `target` / backward from `cell` to `target`. Both are known
// non-negative by construction (see the direction rule above); a caller that cannot guarantee that
// has a layout bug, not a representation choice.
inline std::uint64_t forward(const void* cell, const void* target) noexcept {
    return static_cast<std::uint64_t>(static_cast<const char*>(target) -
                                      static_cast<const char*>(cell));
}
inline std::uint64_t backward(const void* cell, const void* target) noexcept {
    return static_cast<std::uint64_t>(static_cast<const char*>(cell) -
                                      static_cast<const char*>(target));
}

}  // namespace offset_detail

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

    // Written IN PLACE rather than constructed-and-copied: a self-relative offset depends on the
    // cell's final address, which a returned temporary does not know. The generated decoder calls
    // this with the address of the destination member.
    //
    // `s` points into the input region, which precedes every node, so the distance is a positive
    // BACKWARD offset. A present-but-empty string still records its (non-zero) distance, so
    // offset 0 continues to mean "unset" -- which the raw-payload presence check relies on.
    static void store(ArenaString* dst, ByteView s) noexcept {
        if (s.size() > UINT32_MAX || s.data() == nullptr) {
            *dst = ArenaString{};  // unreachable under the input-size guard; stay defined anyway
            return;
        }
        offset_detail::store(dst->m_off, offset_detail::backward(dst, s.data()));
        const auto len = static_cast<std::uint32_t>(s.size());
        std::memcpy(dst->m_len, &len, sizeof len);
    }

    // The borrowed pointer (null only on a default/unset ArenaString). A raw message-payload field
    // uses this for presence: a present payload borrows a non-null input pointer even when empty,
    // while an unset field keeps the null default.
    [[nodiscard]] const char* data() const noexcept {
        const std::uint64_t off = offset_detail::load(m_off);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the offset IS the reference
        return off == 0 ? nullptr : reinterpret_cast<const char*>(this) - off;
    }
    [[nodiscard]] std::string_view view() const noexcept { return {data(), size()}; }
    [[nodiscard]] std::size_t size() const noexcept {
        std::uint32_t n = 0;
        std::memcpy(&n, m_len, sizeof n);
        return n;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
    // BOTH members are byte arrays so the cell is align 1 and a genuine 9 bytes: a `std::uint32_t`
    // member would force align 4 and pad 9 -> 12, undoing the shrink. This is the same trick the
    // pointer version used to keep its cell at align 4, one step further. Align 1 also lets the
    // layout planner drop a string field into any gap.
    char m_off[offset_detail::kOffsetBytes] = {};  // backward offset to the input; 0 == unset
    char m_len[4] = {};  // length; a protobuf span never exceeds ~2 GiB, so 32 bits always fits
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// bugprone-multi-level-implicit-pointer-conversion, bugprone-sizeof-expression)
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaString) == 9 && alignof(ArenaString) == 1 &&
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
// 40-bit FORWARD offset, 5 bytes instead of an 8-byte pointer. The target is allocated after the node
// that refers to it, so the distance is always positive; 0 means unset.
template <class T>
class ArenaPtr {
public:
    ArenaPtr() noexcept = default;

    // Written IN PLACE (see ArenaString::store): the offset depends on the cell's final address.
    static void store(ArenaPtr* dst, const T* target) noexcept {
        if (target == nullptr) {
            *dst = ArenaPtr{};
            return;
        }
        offset_detail::store(dst->m_off, offset_detail::forward(dst, target));
    }

    [[nodiscard]] const T* get() const noexcept {
        const std::uint64_t off = offset_detail::load(m_off);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the offset IS the reference
        return off == 0 ? nullptr
                        : reinterpret_cast<const T*>(reinterpret_cast<const char*>(this) + off);
    }

    // Presence WITHOUT forming the pointer. `get() != nullptr` makes the compiler prove that
    // `this + off` can never be null before it can fold the test down to `off != 0`; this asks the
    // question directly, which is what the decoder's duplicate-singular guard wants on the hot path.
    [[nodiscard]] bool is_set() const noexcept { return offset_detail::load(m_off) != 0; }

private:
    char m_off[offset_detail::kOffsetBytes] = {};  // forward offset to the node; 0 == unset
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

// The in-node array storage: a 40-bit forward offset to the elements plus a 32-bit count, both held
// as byte arrays so the cell is align 1 and a genuine 9 bytes (see ArenaString for why).
template <class T>
class ArenaArray {
public:
    ArenaArray() noexcept = default;

    // Written IN PLACE (see ArenaString::store): a self-relative offset depends on the cell's final
    // address. The array is allocated from the arena AFTER the node holding this cell, so the
    // distance is a positive FORWARD offset; 0 means unset.
    static void store(ArenaArray* dst, const T* data, std::size_t size) noexcept {
        if (data == nullptr || size == 0) {
            *dst = ArenaArray{};
            return;
        }
        offset_detail::store(dst->m_off, offset_detail::forward(dst, data));
        const auto n = static_cast<std::uint32_t>(size);
        std::memcpy(dst->m_size, &n, sizeof n);
    }

    [[nodiscard]] const T* data() const noexcept {
        const std::uint64_t off = offset_detail::load(m_off);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the offset IS the reference
        return off == 0 ? nullptr
                        : reinterpret_cast<const T*>(reinterpret_cast<const char*>(this) + off);
    }
    [[nodiscard]] std::size_t size() const noexcept {
        std::uint32_t n = 0;
        std::memcpy(&n, m_size, sizeof n);
        return n;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    // The ephemeral view an accessor hands out. Reading through it never touches this cell again.
    [[nodiscard]] ArrayView<T> view() const noexcept { return {data(), size()}; }

private:
    char m_off[offset_detail::kOffsetBytes] = {};  // forward offset to the elements; 0 == unset
    char m_size[4] = {};
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaArray<int>) == 9 && alignof(ArenaArray<int>) == 1 &&
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
// in-node STORAGE is an ArenaArray<Entry>, which is the 9-byte cell the layout planner sizes against.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaArray<ArenaString>) == 9 && alignof(ArenaArray<ArenaString>) == 1 &&
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
