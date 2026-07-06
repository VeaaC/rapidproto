// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// rapidproto arena runtime: the std-library-only support that generated arena decoders depend on. An
// arena decoder materializes a protobuf message into a fully-allocated, READ-ONLY object tree whose
// every node lives in a bump Arena. Nothing is allocated outside it, and the input buffer is
// freeable after the decode (all variable-length data is copied in). This header amalgamates:
//   - Arena       : a growable bump allocator (chunked); the only allocator the tree uses.
//   - ArenaString : a small-string-optimized read-only string (inline when small, arena-copied else).
//   - ArrayView / MapView : read-only views the generated accessors return for repeated and maps.
//   - ArenaDecodeError : the failure detail a decode() reports.
// It builds on the wire reader (runtime.hpp: WireReader, WireError, ByteView, the value helpers).
//
// INVARIANT: only trivially-destructible objects are placed in the Arena (ArenaString's big buffer is
// itself arena-owned), so no destructor ever runs: reset() is a pointer rewind, and dropping the
// Arena frees everything at once.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
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

    // Copy bytes into the arena; returns a view of the copy (empty view on empty input). nullptr data
    // (distinguishable only via .data()) signals OOM for a non-empty input.
    ByteView copy_bytes(ByteView src) noexcept {
        if (src.empty()) {
            return {};
        }
        void* const mem = allocate(src.size(), 1);
        if (mem == nullptr) {
            return {};
        }
        std::memcpy(mem, src.data(), src.size());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes view over the copy
        return {reinterpret_cast<const char*>(mem), src.size()};
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
// A 16-byte read-only string with small-string optimization. <= kInlineCap bytes live inline; larger
// values are copied into the arena and referenced as {ptr,len}. Trivially copyable/destructible.
// Layout (byte 15 is the discriminator): inline => [0,15) data, [15] = length (0..15); heap => [0,8)
// const char* ptr, [8,12) uint32 length, [15] = kHeapTag (0xFF, which no inline length can be).
// The 16-byte size (15 inline) is benchmark-tuned by recompiling at 16/24/32 bytes: a wider SSO inlines
// more medium strings but widens every string field, which loses on realistic mostly-short-string
// payloads (see the knob-tuning note in tests/bench_arena.cpp). If retuned, update arenagen's
// kStringSize to match (a static_assert there enforces it).
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,
// cppcoreguidelines-pro-bounds-array-to-pointer-decay, cppcoreguidelines-avoid-c-arrays,
// modernize-avoid-c-arrays, cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers,
// bugprone-multi-level-implicit-pointer-conversion): the class IS a hand-laid-out 16-byte cell --
// a raw char[16] whose bytes are addressed by documented offsets, with the heap {ptr,len} form
// type-punned through memcpy (the sanctioned way). The layout comment above is the source of
// truth the literals mirror.
class ArenaString {
public:
    static constexpr std::size_t kInlineCap = 15;

    ArenaString() noexcept { m_bytes[kTagPos] = 0; }  // empty inline

    // Build from bytes; copies into the arena iff it does not fit inline. Yields an empty string on
    // failure: the Arena was out of memory, or the value exceeds the 4 GiB a heap ArenaString can
    // address (its length is 32-bit). A >4 GiB value is rejected up front (no copy, never truncated);
    // the caller turns the empty result into a decode failure via rp_fail_string (StringTooLong vs OOM).
    static ArenaString make(ByteView s, Arena& arena) noexcept {
        ArenaString out;
        if (s.size() <= kInlineCap) {
            // Copy the <=15 inline bytes with two size-class-bounded, overlapping loads/stores rather
            // than a runtime-length memcpy. A memcpy with a variable small size lowers to a slow
            // generic small-copy (a byte loop / branch ladder) -- measured ~18% faster whole-message
            // arena decode on clang, ~3.5% on gcc, since short strings are common and clang's variable
            // small-memcpy is especially poor. Every read stays within [0, n): no over-read past the
            // wire buffer, safe on untrusted input. Bytes [n, 14] keep the zero-init above; view()
            // reads only [0, n) plus the tag, so they are never observed.
            const char* const p = s.data();
            const std::size_t n = s.size();
            if (n >= 8) {
                std::uint64_t lo = 0;
                std::uint64_t hi = 0;
                std::memcpy(&lo, p, 8);
                std::memcpy(&hi, p + n - 8, 8);
                std::memcpy(out.m_bytes, &lo, 8);
                std::memcpy(out.m_bytes + n - 8, &hi, 8);
            } else if (n >= 4) {
                std::uint32_t lo = 0;
                std::uint32_t hi = 0;
                std::memcpy(&lo, p, 4);
                std::memcpy(&hi, p + n - 4, 4);
                std::memcpy(out.m_bytes, &lo, 4);
                std::memcpy(out.m_bytes + n - 4, &hi, 4);
            } else if (n >= 1) {
                out.m_bytes[0] = p[0];
                out.m_bytes[n - 1] = p[n - 1];
                out.m_bytes[n >> 1U] = p[n >> 1U];
            }
            out.m_bytes[kTagPos] = static_cast<char>(n);
            return out;
        }
        if (s.size() > UINT32_MAX) {
            return out;  // exceeds the 32-bit heap length -> empty; the caller fails the decode
        }
        const ByteView copy = arena.copy_bytes(s);
        if (copy.data() == nullptr) {
            return out;  // OOM -> empty
        }
        const char* const ptr = copy.data();
        const auto len = static_cast<std::uint32_t>(copy.size());  // fits: s.size() checked above
        std::memcpy(out.m_bytes, &ptr, sizeof ptr);
        std::memcpy(out.m_bytes + sizeof(ptr), &len, sizeof len);
        out.m_bytes[kTagPos] = kHeapTag;
        return out;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        if (static_cast<unsigned char>(m_bytes[kTagPos]) == kHeapByte) {
            const char* ptr = nullptr;
            std::uint32_t len = 0;
            std::memcpy(&ptr, m_bytes, sizeof ptr);
            std::memcpy(&len, m_bytes + sizeof(ptr), sizeof len);
            return {ptr, len};
        }
        return {m_bytes, static_cast<std::size_t>(m_bytes[kTagPos])};
    }
    [[nodiscard]] std::size_t size() const noexcept { return view().size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
    static constexpr std::size_t kTagPos = 15;
    // The 0xFF heap-tag discriminator in two types: stored as char (kHeapTag), compared as unsigned
    // char (kHeapByte) to avoid signed-char ambiguity. No inline length (0..15) can equal it.
    static constexpr char kHeapTag = static_cast<char>(0xFF);
    static constexpr unsigned char kHeapByte = 0xFFU;

    alignas(const char*) char m_bytes[16] = {};
};
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,
// cppcoreguidelines-pro-bounds-array-to-pointer-decay, cppcoreguidelines-avoid-c-arrays,
// modernize-avoid-c-arrays, cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers,
// bugprone-multi-level-implicit-pointer-conversion)
// NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
static_assert(sizeof(ArenaString) == 16 && std::is_trivially_destructible_v<ArenaString>);

// ── ArrayView / MapView ──────────────────────────────────────────────────────────────────────────
// A read-only contiguous view into the arena, returned by repeated-field accessors. This is the
// runtime's own {ptr,len} view (the std::span stand-in the parser calls Range -- the two are kept
// separate only because this header ships self-contained and cannot include the parser's range.hpp;
// unifying them would couple this header to range.hpp and is not obviously worth it).
template <class T>
class ArrayView {
public:
    ArrayView() noexcept = default;
    ArrayView(const T* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): the span abstraction itself
    const T& operator[](std::size_t i) const noexcept { return m_data[i]; }
    [[nodiscard]] const T* begin() const noexcept { return m_data; }
    [[nodiscard]] const T* end() const noexcept { return m_data + m_size; }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

private:
    const T* m_data = nullptr;
    std::size_t m_size = 0;
};

// ── StringArrayView ────────────────────────────────────────────────────────────────────────────────
// A read-only view over an arena array of ArenaString that yields std::string_view per element. A
// repeated string/bytes accessor returns this, so consumers see std::string_view and never the internal
// ArenaString storage type; the underlying SSO storage is unchanged.
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

// ── Decode failure ────────────────────────────────────────────────────────────────────────────────
struct ArenaDecodeError {
    enum class Code : std::uint8_t {
        None,
        Wire,                     // a WireError from the underlying reader (see `wire` / `offset`)
        OutOfMemory,              // the Arena could not satisfy an allocation
        RecursionTooDeep,         // message nesting exceeded kMaxDecodeDepth
        MissingRequired,          // a proto2 `required` field was absent (see `field_number`)
        RepeatedSingularMessage,  // a singular message field occurred more than once (see field_number)
        StringTooLong,  // a string/bytes value exceeded the 4 GiB arena representation limit
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
inline void rp_fail_wire(ArenaDecodeError* err, const WireReader& reader) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::Wire;
        err->wire = reader.error_code();
        err->offset = reader.fail_offset();
    }
}
inline void rp_fail_oom(ArenaDecodeError* err) noexcept {
    if (err != nullptr) {
        err->code = ArenaDecodeError::Code::OutOfMemory;
    }
}
// A string/bytes store failed: the value exceeds the 4 GiB an ArenaString can represent
// (StringTooLong) or the Arena was out of memory (OutOfMemory). The payload size selects which.
inline void rp_fail_string(ArenaDecodeError* err, ByteView payload) noexcept {
    if (err != nullptr) {
        err->code = payload.size() > UINT32_MAX ? ArenaDecodeError::Code::StringTooLong
                                                : ArenaDecodeError::Code::OutOfMemory;
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
}  // namespace arena_detail

}  // namespace rapidproto
