// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// rapidproto arena runtime: the std-library-only support that generated arena decoders depend on. An
// arena decoder materializes a protobuf message into a fully-allocated, READ-ONLY object tree whose
// every node lives in a bump Arena. The tree BORROWS the input: strings and bytes are {ptr,len} views
// into the input wire buffer (no copy), so the tree is valid only while BOTH the Arena and the input
// outlive it. Callers who want a self-contained result use decode_owned, which bundles the input and a
// default Arena into one owning handle. This header amalgamates:
//   - Arena       : a growable bump allocator (chunked); the only allocator the tree uses.
//   - ArenaString : a read-only string that borrows a {ptr,len} view into the input (no copy, no SSO).
//   - ArrayView / MapView : read-only views the generated accessors return for repeated and maps.
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
// A growable bump allocator. Hands out aligned, uninitialized memory from a sequence of chunks; on a
// chunk overflow it appends a new (geometrically larger) chunk. reset() rewinds for reuse without
// freeing; chunks are RAII-owned (freed when the Arena is destroyed). Single-threaded. allocate()
// returns nullptr only on host OOM (the only failure), surfaced as ArenaDecodeError::OutOfMemory.
class Arena {
public:
    Arena() noexcept = default;
    // Seed with a caller-owned initial buffer (NOT freed by the Arena): zero-malloc until exceeded.
    // The whole buffer is usable; the chunk descriptor lives inline in the Arena, not the buffer.
    Arena(void* buffer, std::size_t size) noexcept {
        if (buffer != nullptr && size > kMaxAlign) {
            adopt_buffer(buffer, size);
        }
    }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;
    ~Arena() = default;  // chunks are RAII (unique_ptr storage); nothing to free by hand

    // Aligned, uninitialized bytes. `align` must be a power of two. nullptr only on OOM.
    void* allocate(std::size_t bytes, std::size_t align) noexcept {
        assert(align != 0 && (align & (align - 1)) == 0 && "align must be a power of two");
        char* const p = align_up(m_cur, align);
        // Size comparison, never `p + bytes`: forming that pointer is UB on an empty arena (p is null)
        // or when `bytes` is a huge untrusted value (it overflows past the end of the object).
        if (m_cur != nullptr && p <= m_limit && bytes <= static_cast<std::size_t>(m_limit - p)) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the bump allocation
            m_cur = p + bytes;  // fits in the current chunk; p + bytes is now in-bounds
            m_used += bytes;
            return p;
        }
        return allocate_slow(bytes, align);
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

    // Construct one T in the arena (trivially destructible only). Returns nullptr on OOM. The
    // arena owns every object it places; the returned pointer is a borrow into the chunk, never
    // individually freed.
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

    // Return the unused tail of the MOST-RECENT allocation, shrinking it from `old_bytes` to
    // `new_bytes`. A no-op unless `ptr`'s end is exactly the current bump cursor -- i.e. it is only
    // effective when nothing has been allocated since. The packed-array decode uses this: it
    // pre-sizes a scalar array to an upper bound, fills it (allocating nothing in between), then
    // trims the over-estimate back to the exact element count.
    //
    // Decoder-internal, like allocate_array/create: this is the Arena surface the GENERATED code
    // targets, not a general-purpose API. It trusts the caller to pass the exact byte size of the
    // most-recent allocation; a wrong `old_bytes` that happened to equal the cursor offset would
    // rewind into live data (generated callers always pass the true size -- see the emit site).
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

    // Rewind to the first chunk for reuse; keeps all chunks allocated (no malloc on the next decode).
    void reset() noexcept {
        m_used = 0;
        m_cursor = 0;
        if (m_live) {
            m_cur = m_head.data;
            m_limit = m_head.limit;
        } else {
            m_cur = nullptr;
            m_limit = nullptr;
        }
    }

    [[nodiscard]] std::size_t bytes_used() const noexcept {
        return m_used;
    }  // useful payload handed out
    [[nodiscard]] std::size_t bytes_reserved() const noexcept {
        return m_reserved;
    }  // total host memory held

    // Cap the total host memory the arena reserves; a grow past it fails as if on host OOM, so the
    // decode returns ArenaDecodeError::OutOfMemory. Default: unbounded. Bounds arena memory for untrusted
    // input, and makes the OOM path exercisable in tests. Set before parsing; should be >= any seed.
    // Note: a packed *varint* scalar array is pre-sized to its byte length (an upper bound on the
    // element count) and trimmed after (shrink_last), so its TRANSIENT peak can briefly reach a few
    // times the field's payload for multi-byte-value fields; size the cap for that peak, not the tree.
    void set_capacity_limit(std::size_t max_reserved_bytes) noexcept { m_cap = max_reserved_bytes; }

private:
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-avoid-c-arrays,
    // modernize-avoid-c-arrays): a bump allocator IS bounds-checked pointer arithmetic over raw
    // byte chunks, and unique_ptr<char[]> is the idiomatic owning form of an untyped heap buffer
    // (std::array cannot size at runtime; vector<char> value-initializes, touching every page).
    // One memory block. `storage` owns it (freed by the Arena's implicit destructor); for a
    // caller-seeded buffer `storage` is null so the buffer is left alone. `data`/`limit` bound the
    // usable region (data is kMaxAlign-aligned). Movable, never copied.
    struct Chunk {
        std::unique_ptr<char[]> storage;  // null => caller-owned seed buffer (not freed)
        char* data = nullptr;             // first usable byte (kMaxAlign-aligned)
        char* limit = nullptr;            // one past the last usable byte
    };

    static constexpr std::size_t kMaxAlign = alignof(std::max_align_t);
    static constexpr std::size_t kDefaultChunk = 4096;
    // Geometric chunk growth stops doubling here. Each chunk wastes at most its own unfilled tail, and
    // an uncapped schedule makes the final chunk the largest one (it doubled the whole way up) and on
    // average half empty -- so the held-vs-used gap grows with the arena. Capping the chunk size caps
    // that tail at a constant; it is the dominant held-memory lever (roughly halved the gap on the
    // benchmark). Kept under glibc's 128 KiB default mmap threshold so cold-arena chunks stay on the
    // heap (a chunk that crosses the threshold becomes an mmap/munmap syscall pair).
    static constexpr std::size_t kMaxChunk = std::size_t{96} * 1024;

    // NOLINTNEXTLINE(readability-non-const-parameter): the result aliases p's WRITABLE buffer; a
    // const char* parameter would just launder the const back off through the integer round-trip.
    static char* align_up(char* p, std::size_t align) noexcept {
        const auto addr = reinterpret_cast<std::uintptr_t>(p);  // NOLINT(*-reinterpret-cast)
        const auto aligned = (addr + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
        // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<char*>(aligned);
    }

    // Chunks are addressed by index: 0 is the inline head (seed or first grown), 1..N live in m_more.
    // Keeping the head inline means a seeded arena that never grows performs zero heap allocations.
    Chunk& chunk(std::size_t i) noexcept { return i == 0 ? m_head : m_more[i - 1]; }
    [[nodiscard]] const Chunk& chunk(std::size_t i) const noexcept {
        return i == 0 ? m_head : m_more[i - 1];
    }
    [[nodiscard]] std::size_t chunk_count() const noexcept {
        return m_live ? 1 + m_more.size() : 0;
    }

    void adopt_buffer(void* buffer, std::size_t size) noexcept {
        char* const base = static_cast<char*>(buffer);
        m_head.storage = nullptr;  // caller owns it
        m_head.data = align_up(base, kMaxAlign);
        m_head.limit = base + size;
        m_live = true;
        advance_to(0);
        m_reserved += static_cast<std::size_t>(m_head.limit - m_head.data);
    }

    void* allocate_slow(std::size_t bytes, std::size_t align) noexcept {
        // Reuse the next already-allocated chunk (after a reset), else grow a fresh one.
        if (m_cursor + 1 < chunk_count() && fits(chunk(m_cursor + 1), bytes, align)) {
            advance_to(m_cursor + 1);
        } else if (bytes > SIZE_MAX - align || !grow(bytes + align)) {  // guard the bytes+align sum
            return nullptr;
        }
        char* const p = align_up(m_cur, align);
        m_cur = p + bytes;
        m_used += bytes;
        return p;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): bytes then align, as in allocate()
    static bool fits(const Chunk& c, std::size_t bytes, std::size_t align) noexcept {
        char* const p =
            align_up(c.data, align);  // size comparison, never `p + bytes` (see allocate)
        return p <= c.limit && bytes <= static_cast<std::size_t>(c.limit - p);
    }

    void advance_to(std::size_t i) noexcept {
        m_cursor = i;
        const Chunk& c = chunk(i);
        m_cur = c.data;
        m_limit = c.limit;
    }

    bool grow(std::size_t min_usable) noexcept {
        const std::size_t cap = m_next_chunk < min_usable ? min_usable : m_next_chunk;
        // +kMaxAlign so align_up of the block base can never run past the end; PTRDIFF_MAX guards
        // against new[] throwing bad_array_new_length (which the nothrow form does not suppress).
        if (cap > static_cast<std::size_t>(PTRDIFF_MAX) - kMaxAlign) {
            return false;  // request too large to represent -> treat as OOM
        }
        const std::size_t raw = cap + kMaxAlign;
        if (m_reserved >= m_cap || raw > m_cap - m_reserved) {
            return false;  // capacity cap reached -> treat as OOM (the only injectable failure)
        }
        std::unique_ptr<char[]> storage(new (std::nothrow) char[raw]);  // nothrow -> null on OOM
        if (!storage) {
            return false;
        }
        char* const base = storage.get();
        Chunk c;
        c.data = align_up(base, kMaxAlign);
        c.limit = base + raw;
        c.storage = std::move(storage);
        m_reserved += static_cast<std::size_t>(c.limit - c.data);
        if (!m_live) {
            m_head = std::move(c);
            m_live = true;
            m_cursor = 0;
        } else {
            // Insert right after the current chunk (global index m_cursor+1 == m_more[m_cursor]), so a
            // grow after reset() preserves the existing successors after the new chunk for reuse.
            m_more.insert(m_more.begin() + static_cast<std::ptrdiff_t>(m_cursor), std::move(c));
            m_cursor += 1;
        }
        advance_to(m_cursor);
        if (m_next_chunk < kMaxChunk) {
            m_next_chunk <<= 1U;  // geometric until kMaxChunk, then constant
            m_next_chunk = std::min(m_next_chunk, kMaxChunk);
        }
        return true;
    }

    char* m_cur = nullptr;
    char* m_limit = nullptr;
    Chunk m_head;               // chunk 0, inline: seed buffer or the first grown chunk
    std::vector<Chunk> m_more;  // chunks 1..N (heap), empty until a second chunk is needed
    std::size_t m_cursor = 0;   // index of the current chunk
    bool m_live = false;        // is m_head initialized?
    std::size_t m_used = 0;
    std::size_t m_reserved = 0;
    std::size_t m_next_chunk = kDefaultChunk;
    std::size_t m_cap = SIZE_MAX;  // capacity cap on m_reserved; SIZE_MAX = unbounded (the default)
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-avoid-c-arrays,
    // modernize-avoid-c-arrays)
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

    // Store a {ptr,len} view into the input (no copy; `arena` is unused, kept for a uniform call
    // shape). Yields an empty string only for a value exceeding the 32-bit length -- which the
    // top-level >UINT32_MAX input guard already rules out, so under a real decode this never fails.
    static ArenaString make(ByteView s, Arena& arena) noexcept {
        (void)arena;
        ArenaString out;
        if (s.size() > UINT32_MAX) {
            return out;  // exceeds the 32-bit length -> empty (unreachable under the input-size guard)
        }
        const char* const p = s.data();
        std::memcpy(out.m_ptr, &p, sizeof p);
        out.m_len = static_cast<std::uint32_t>(s.size());
        return out;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        const char* p = nullptr;
        std::memcpy(&p, m_ptr, sizeof p);
        return {p, m_len};
    }
    [[nodiscard]] std::size_t size() const noexcept { return m_len; }
    [[nodiscard]] bool empty() const noexcept { return m_len == 0; }

private:
    char m_ptr[8] = {};       // the borrowed pointer, byte-array (align 1) to keep the cell align 4
    std::uint32_t m_len = 0;  // length; a protobuf span never exceeds ~2 GiB, so uint32 always fits
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// bugprone-multi-level-implicit-pointer-conversion, bugprone-sizeof-expression)
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaString) == 12 && alignof(ArenaString) == 4 &&
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
template <class T>
class ArrayView {
public:
    ArrayView() noexcept = default;
    ArrayView(const T* data, std::size_t size) noexcept : m_size(static_cast<std::uint32_t>(size)) {
        std::memcpy(m_data, &data, sizeof data);
    }

    [[nodiscard]] const T* data() const noexcept {
        const T* p = nullptr;
        std::memcpy(&p, m_data, sizeof p);
        return p;
    }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    const T& operator[](std::size_t i) const noexcept { return data()[i]; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end() const noexcept { return data() + m_size; }

private:
    char m_data[8] = {};  // the data pointer, memcpy'd (see class comment); char[8] keeps align 4
    std::uint32_t m_size = 0;
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArrayView<int>) == 12 && alignof(ArrayView<int>) == 4 &&
              std::is_trivially_destructible_v<ArrayView<int>>);

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
// MapView wraps a single ArrayView, so it inherits the 12-byte / 4-align cell.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(MapView<ArenaString>) == 12 && alignof(MapView<ArenaString>) == 4 &&
              std::is_trivially_destructible_v<MapView<ArenaString>>);

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

// A singular raw member encodes ABSENCE as a null-data view (like a materialized message
// field's null pointer -- no mask bit spent). A PRESENT but empty payload must therefore be
// non-null: it points here.
inline constexpr char kEmptyPayload = 0;

// Arena-copy a field-modes `raw` payload, so the stored ByteView outlives the input buffer the
// decode merely borrowed. Returns false on OOM. The result is always non-null (empty payloads
// view kEmptyPayload), so a null-data view stays free to mean "absent".
[[nodiscard]] inline bool copy_payload(ByteView payload, Arena& arena, ByteView& out) noexcept {
    if (payload.empty()) {
        // A deliberately empty, deliberately NON-NULL view -- null data is the absence encoding.
        // NOLINTNEXTLINE(bugprone-string-constructor)
        out = ByteView(&kEmptyPayload, 0);
        return true;
    }
    char* const data = arena.allocate_array<char>(payload.size());
    if (data == nullptr) {
        return false;
    }
    std::memcpy(data, payload.data(), payload.size());
    out = ByteView(data, payload.size());
    return true;
}

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
