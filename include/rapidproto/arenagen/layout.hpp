// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The arena layout planner -- the "brain" of the arena generator. A pure analysis pass (no codegen)
// that maps every message to its in-memory representation:
//   - a FIELD KIND per field (inline scalar/enum, SSO string, inlined-fixed vs pointer sub-message,
//     repeated/map views, oneof),
//   - a padding-minimized MEMBER ORDER (sort by alignment desc, size desc, field number),
//   - a bit-packed PRESENCE/VALUE mask (bools and presence flags share mask words),
//   - a recursive, cycle-aware FIXED-SIZE analysis that drives sub-message inlining.
// The result feeds struct + parse emission (generator.cpp) and is golden-tested on its own via a layout
// dump, so every decision is reviewable as text before any C++ is generated.

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/ast.hpp"

namespace rapidproto {
struct ResolvedFileSet;  // rapidproto/resolver.hpp
struct SymbolTable;      // rapidproto/resolve.hpp (carries the FQN -> node type index)
}  // namespace rapidproto

namespace rapidproto::arenagen {

// How a field is stored in its parent message's struct.
enum class FieldKind {
    InlineScalar,       // numeric scalar stored inline; a `bool` instead occupies a value bit
    InlineEnum,         // enum stored inline as its int32 underlying value
    SsoString,          // string/bytes stored as an ArenaString (inline small, arena-copied large)
    InlineFixedSubMsg,  // fixed-size sub-message inlined by value
    PointerSubMsg,      // sub-message referenced by an arena pointer (null = absent)
    Repeated,           // repeated field -> ArrayView<elem>
    Map,                // map field -> MapView<entry>
    Raw,  // field-modes `raw`: the message field's arena-copied payload(s), decoded later
};

const char* kind_name(FieldKind kind);

// Planning options: benchmark-tunable knobs (defaults are the benchmark-chosen values) plus
// the per-field materialization selection.
struct LayoutOptions {
    // A fixed-size sub-message is inlined by value iff its size <= this, else stored behind an arena
    // pointer. 16 is the benchmark-chosen optimum: with single-pass-growable repeated arrays, inlining
    // a sub-message of size S into a parent costs ~2S of array memory (struct + its realloc copy)
    // while a pointer costs ~16+S, so inlining wins exactly up to S = 16. Confirmed by recompiling at
    // 16/24/32 (see the knob-tuning note in tests/bench_arena.cpp).
    std::size_t inline_submsg_cutoff = 16;
    bool unknown_present = false;  // reserve a per-message "unknown fields present" bit
    // The resolved per-field materialization selection (see modes.hpp); null/inactive = every
    // field materializes. Caller-owned; must outlive planning.
    const FieldModes* modes = nullptr;
};

// A map field's synthesized entry {key, value}: its own little two-member layout (no bits; both
// key and value are always materialized).
struct EntryPlan {
    FieldKind key_kind = FieldKind::InlineScalar;
    std::string key_repr;
    std::size_t key_offset = 0;  // byte offsets within the entry struct (for compact emission)
    FieldKind value_kind = FieldKind::InlineScalar;
    std::string value_repr;
    std::string value_fqn;  // message/enum value type, else ""
    std::size_t value_offset = 0;
    std::size_t size = 0;
    std::size_t align = 0;
};

// One field's storage decision. For a bit-only field (a `bool`) size and align are 0 and the data
// lives in `value_bit`; everything else occupies `size` bytes at `offset`.
struct MemberPlan {
    const FieldNode* field = nullptr;         // set for regular/repeated fields
    const MapFieldNode* map_field = nullptr;  // set instead for a Map (or a raw map)
    FieldKind kind = FieldKind::InlineScalar;
    bool is_bool = false;    // an InlineScalar `bool`: a value bit, not a byte
    std::size_t size = 0;    // storage bytes (0 for bit-only)
    std::size_t align = 0;   // storage alignment (0 for bit-only)
    std::size_t offset = 0;  // byte offset within the struct (set when size > 0)
    int presence_bit = -1;   // mask bit index, or -1 (Implicit/Required, pointer, repeated/map/raw)
    int value_bit = -1;      // mask bit index for an inline `bool` value, or -1
    std::string repr;        // storage label for the dump (e.g. "int32", "ArenaString", ".p.Sub")
    std::string target_fqn;  // referenced message/enum FQN, else ""
    std::optional<EntryPlan> entry;  // Map only
};

// One member of a oneof's union. Inside a union everything is byte storage (no bit-packing), so a
// `bool` member is a 1-byte bool.
struct OneofMemberPlan {
    const FieldNode* field = nullptr;
    FieldKind kind = FieldKind::InlineScalar;
    std::size_t size = 0;
    std::size_t align = 0;
    std::string repr;
    std::string target_fqn;
};

// A oneof: a uint8 discriminant (0 = none set, else the 1-based member index) plus a union sized to
// the largest member. The discriminant is the oneof's presence -- no mask bit is used.
struct OneofPlan {
    const OneofNode* oneof = nullptr;
    std::size_t disc_offset = 0;
    std::size_t union_offset = 0;
    std::size_t union_size = 0;
    std::size_t union_align = 0;
    std::vector<OneofMemberPlan> members;
};

// The complete in-memory plan for one message.
struct MessageLayout {
    const MessageNode* message = nullptr;
    std::string fqn;
    std::size_t size = 0;     // total struct size (a multiple of `align`)
    std::size_t align = 1;    // struct alignment
    bool fixed_size = false;  // inline-eligible: no string/repeated/map/pointer/oneof/self-ref

    std::vector<MemberPlan>
        members;                    // byte members in memory (offset) order, then bit-only members
    std::vector<OneofPlan> oneofs;  // in memory order (disc/union offsets assigned)

    // Fields the profile DROPPED: no member, no accessor, no decode arm -- listed here so the
    // layout dump (the reviewable plan) shows the omission explicitly instead of silently.
    std::vector<const FieldNode*> dropped;
    std::vector<const MapFieldNode*> dropped_maps;

    int mask_bits = 0;  // total presence + value (+ unknown) bits
    std::size_t mask_offset = 0;
    std::size_t mask_size = 0;  // 0 if no bits; else 1/2/4/8 (or a multiple of 8 for >64 bits)
    std::size_t mask_align = 0;
    int unknown_bit = -1;  // mask bit index of the unknown-present flag, or -1
};

// Every message's layout (top-level and nested, in declaration order), plus FQN lookup.
struct LayoutSet {
    std::vector<MessageLayout> layouts;
    // FQN -> index into `layouts`, so the emitter's per-message find() is O(1) instead of a scan
    // (it is called several times per emitted message). Filled as plan_layouts walks.
    std::unordered_map<std::string, std::size_t> by_fqn;
    [[nodiscard]] const MessageLayout* find(const std::string& fqn) const;
};

// Plan the layout of every message in an analyzed file set. `symbols` is the table analyze() returned;
// its FQN -> node maps drive the sub-message inspection/recursion. Precondition: analyze() has run.
LayoutSet plan_layouts(const ResolvedFileSet& set, const SymbolTable& symbols,
                       const LayoutOptions& options = {});

}  // namespace rapidproto::arenagen
