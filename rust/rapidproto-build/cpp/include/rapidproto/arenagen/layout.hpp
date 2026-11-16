// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The arena layout planner -- the "brain" of the arena generator. A pure analysis pass (no codegen)
// that maps every message to its in-memory representation:
//   - a FIELD KIND per field (inline scalar/enum, borrowed string, inlined-fixed vs pointer sub-message,
//     repeated/map views, oneof),
//   - a padding-minimized MEMBER ORDER (sort by alignment desc, size desc, field number),
//   - a bit-packed PRESENCE/VALUE mask (bools and presence flags share mask words),
//   - a recursive, cycle-aware FIXED-SIZE analysis that drives sub-message inlining.
// The result feeds struct + parse emission (generator.cpp) and is golden-tested on its own via a layout
// dump, so every decision is reviewable as text before any C++ is generated.

#include <cstddef>
#include <cstdint>
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
enum class FieldKind : std::uint8_t {
    InlineScalar,  // numeric scalar stored inline; a `bool` instead occupies a value bit
    InlineEnum,    // enum stored inline as its int32 underlying value
    BorrowString,  // string/bytes stored as an ArenaString (a borrowed {ptr,len} view, no copy)
    InlineFixedSubMsg,  // fixed-size sub-message inlined by value
    PointerSubMsg,      // sub-message referenced by an arena pointer (null = absent)
    Repeated,           // repeated field -> ArrayView<elem>
    Map,                // map field -> MapView<entry>
    Raw,  // field-modes `raw`: the message field's borrowed payload(s) (ArenaString), decoded later
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
    // The benchmark-chosen default derived by the comment above; a name would just restate it.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
    std::size_t inline_submsg_cutoff = 16;
    // The resolved decode profile (see modes.hpp); null/inactive = every field materializes and no
    // message reserves the unknown-fields bit. The profile also drives which messages get that bit
    // (`--unknown-present` resolves to "every message"). Caller-owned; must outlive planning.
    const FieldModes* modes = nullptr;

    // Threshold in DECODE ARMS (fields, including map fields, plus oneof members) at which a
    // message stops being inlinable into its PARENT: once its own arms plus the arms of the
    // closure it still inlines exceed this, it is emitted RP_NOINLINE. Not an upper bound on any
    // one function -- the message that trips it still contains the closure that tripped it.
    //
    // Why it exists: forcing RP_FLATTEN is itself an optimization over letting the compiler decide,
    // taken because GCC's inliner leaves decode throughput on the table in a large TU. The price is
    // that flatten inlines a message's WHOLE sub-message closure, so on a large schema one decoder
    // absorbs the rest -- on g++-13, googleapis google/container/v1beta1/cluster_service.proto
    // (288 messages) does not finish compiling in 280s with flatten unbounded, against 129s at
    // budget 4. This threshold is what keeps forcing flatten from costing serious build time
    // relative to not forcing it at all. Clang's inliner does not need the help; GCC does.
    //
    // 4 is the cheapest budget measured to compile: over a googleapis sample, 185s at 4 against
    // 220s at 8 and >=329s at 32 (see architecture.md). Budgets 1-4 emit identical code for
    // tests/bench/bench.proto; on a large schema they do differ (cluster_service marks 83 messages
    // at 4, 102 at 1), and budgets below 4 were not measured for compile time. Decode throughput
    // was measured and showed no regression attributable to the threshold, but this bench's
    // cross-build comparisons are too noisy to support a stronger claim -- see architecture.md.
    //
    // KNOWN GAP: a message is exempt when it inlines nothing itself -- a leaf, or one whose every
    // target is already marked -- however wide it is. Marking it would bound nothing BELOW it, but
    // it does leave a wide body free to be absorbed by each of its parents. On descriptor.proto
    // that costs the largest decoder ~46% of its .text (FieldDescriptorProto is exempt because its
    // only target is marked, and DescriptorProto absorbs it twice) at no compile-time saving. The
    // threshold does not bound what a parent absorbs from a single child.
    //
    // 0 disables the pass: flatten everything, and note that this also stops breaking CYCLES, so a
    // recursive message's flatten expansion is then bounded only by the compiler's own
    // inline-recursion limit.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
    std::size_t flatten_budget = 4;
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

    // Emit this message's rp_decode_into as RP_NOINLINE as well as RP_FLATTEN, so a PARENT's
    // flatten cannot pull this decoder's body into itself. Set by the flatten-budget pass (see
    // LayoutOptions::flatten_budget); reported in the layout dump so the decision is reviewable
    // as text rather than only visible as a code-size or timing change.
    bool noinline_decode = false;
    // What the budget pass computed for this message: its own decode-arm count plus the accumulated
    // cost of every sub-message closure it still inlines. This is the value flatten_budget was
    // compared against, kept whether or not the message was marked so the decision can be checked.
    // A marked message contributes nothing to an ancestor's cost regardless of this value -- the
    // pass skips it by noinline_decode, which is what bounds every ancestor.
    // 0 when the pass did not run (flatten_budget == 0).
    std::size_t flatten_cost = 0;
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
