// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#include "rapidproto/arenagen/layout.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rapidproto/arena_runtime.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/ast.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

namespace rapidproto::arenagen {
namespace {

// In-memory sizes of the runtime storage types the layout assumes. Asserted against the actual
// runtime so the planner can never drift from arena_runtime.hpp.
constexpr std::size_t kPtrSize = 8;
constexpr std::size_t kPtrAlign = 8;
constexpr std::size_t kStringSize = 12;  // ArenaString (borrowed view: char[8] ptr + uint32 len)
constexpr std::size_t kStringAlign = 4;
constexpr std::size_t kViewSize = 12;  // ArrayView<T> / MapView<E> = {char[8] ptr, uint32 size}
constexpr std::size_t kViewAlign = 4;
constexpr std::size_t kEnumSize = 4;  // enums stored as their int32 underlying value
constexpr std::size_t kEnumAlign = 4;
constexpr std::size_t kDiscSize = 1;  // oneof discriminant: a uint8 member index (0 = none)
constexpr std::size_t kDiscAlign = 1;
constexpr int kBitsPerByte = 8;  // mask-word sizing: bits per byte and per uint64 word
constexpr int kBitsPerWord = 64;
constexpr std::size_t kBytesPerWord = 8;
// Caps the planner's native-stack recursion over message-REFERENCE chains (M2 { M1 f; } M3 { M2 f; }
// ...), which -- unlike syntactic nesting (kMaxParseDepth) -- is unbounded in a protoc-valid schema.
// Past the cap a sub-message conservatively degrades to pointer storage instead of recursing, the
// same fallback as a cycle back-edge: still-correct layout, marginally less optimal. Real schemas
// stay far below this; only a reference chain declared wrapper-first (forward references) recurses
// at all, since a chain declared bottom-up is served memoized at depth <= 2.
constexpr std::size_t kMaxChainDepth = 200;
static_assert(sizeof(ArenaString) == kStringSize && alignof(ArenaString) == kStringAlign);
static_assert(sizeof(ArrayView<int>) == kViewSize && alignof(ArrayView<int>) == kViewAlign);
static_assert(sizeof(MapView<ArenaString>) == kViewSize &&
              alignof(MapView<ArenaString>) == kViewAlign);

std::size_t align_up(std::size_t n, std::size_t align) {
    return (n + align - 1) & ~(align - 1);  // align is always a power of two here
}

// proto scalar keyword -> {in-memory size, align, is bool}. string/bytes are NOT here (they
// become an ArenaString, a borrowed view into the input); everything else is a fixed-size
// numeric/bool. The layout dump labels a scalar member with its proto type keyword -- the map
// KEY -- so there is no separate label column to drift from it.
struct ScalarInfo {
    std::size_t size;
    std::size_t align;
    bool is_bool;
};
const ScalarInfo* scalar_info(std::string_view type) {
    static const std::map<std::string_view, ScalarInfo> kTable = {
        {"int32", {4, 4, false}},   {"sint32", {4, 4, false}},   {"sfixed32", {4, 4, false}},
        {"uint32", {4, 4, false}},  {"fixed32", {4, 4, false}},  {"int64", {8, 8, false}},
        {"sint64", {8, 8, false}},  {"sfixed64", {8, 8, false}}, {"uint64", {8, 8, false}},
        {"fixed64", {8, 8, false}}, {"float", {4, 4, false}},    {"double", {8, 8, false}},
        {"bool", {1, 1, true}},
    };
    const auto it = kTable.find(type);
    return it != kTable.end() ? &it->second : nullptr;
}

bool is_string_field(const FieldNode& field) {
    return !field.is_message_type && !field.is_enum_type &&
           (field.type_name == "string" || field.type_name == "bytes");
}

// The element storage label for a repeated field (the array holds these by value).
std::string elem_repr(const FieldNode& field) {
    if (field.is_message_type || field.is_enum_type) {
        return field.resolved_type_fqn;
    }
    if (is_string_field(field)) {
        return "ArenaString";
    }
    return std::string(field.type_name);  // scalar keywords label themselves
}

// The padding-minimizing slot order: alignment desc, size desc, then a stable tiebreak (field number
// where one exists, else a per-slot sequence). This leaves NO internal padding -- only trailing
// padding to the struct's own alignment -- because (1) every member's size is a multiple of its own
// alignment (true for scalars and for inlined sub-message structs alike), and (2) alignments are
// powers of two placed in descending order, so each class's alignment divides the previous one and
// the cursor, once aligned for a class, stays aligned through it and into the next.
struct Slot {
    std::size_t size;
    std::size_t align;
    long long tiebreak;  // field number, or a sentinel
    int seq;             // final disambiguator for a total order
    std::size_t* out;    // where to write the assigned offset
};

// The overall size + alignment of a laid-out struct (order_slots' result).
struct StructExtent {
    std::size_t size;
    std::size_t align;
};

// Sort `slots` into the padding-minimizing order, write each slot's offset through its `out` pointer,
// and return the struct's overall {size, align}.
StructExtent order_slots(std::vector<Slot>& slots) {
    std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) {
        if (a.align != b.align) {
            return a.align > b.align;
        }
        if (a.size != b.size) {
            return a.size > b.size;
        }
        if (a.tiebreak != b.tiebreak) {
            return a.tiebreak < b.tiebreak;
        }
        return a.seq < b.seq;
    });
    std::size_t cursor = 0;
    std::size_t max_align = 1;
    for (const Slot& slot : slots) {
        const std::size_t off = align_up(cursor, slot.align);
        *slot.out = off;
        cursor = off + slot.size;
        max_align = std::max(max_align, slot.align);
    }
    return {cursor == 0 ? 1 : align_up(cursor, max_align),
            max_align};  // empty struct is 1 byte in C++
}

// Slot tiebreak (sorts within an alignment/size class): a field/map number, else 0.
constexpr long long kMaskTiebreak = 0x7fffffffLL;  // sorts the mask word last within its class
long long member_tiebreak(const MemberPlan& m) {
    if (m.field != nullptr) {
        return m.field->number;
    }
    if (m.map_field != nullptr) {
        return m.map_field->number;
    }
    return 0;
}

class Planner {
public:
    Planner(const SymbolTable& index, const LayoutOptions& options)
        : m_index(index), m_opts(options) {}

    const MessageLayout& layout_for(const std::string& fqn) {
        if (const auto it = m_memo.find(fqn); it != m_memo.end()) {
            return it->second;
        }
        const auto node = m_index.messages.find(fqn);
        // Precondition: fqn is a known message (every caller checks membership first, or passes an
        // FQN collect_symbols registered). A broken invariant must not become an end()-dereference:
        // assert in debug, and degrade to an empty (pointer-safe) layout instead of UB in release.
        assert(node != m_index.messages.end() && "layout_for: fqn must be a registered message");
        if (node == m_index.messages.end()) {
            static const MessageLayout kEmpty{};
            return kEmpty;
        }
        return compute(*node->second);
    }

private:
    // Refs to long-lived inputs; the Planner is a short-lived, non-copied compute helper.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const SymbolTable& m_index;
    const LayoutOptions& m_opts;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::map<std::string, MessageLayout> m_memo;
    std::set<std::string> m_visiting;

    // The storage decision for a message-typed field: inlined by value when fixed-size and small,
    // else behind an arena pointer.
    struct Store {
        FieldKind kind;
        std::size_t size;
        std::size_t align;
    };
    Store classify_message(const std::string& target_fqn) {
        // A target still being computed is a cycle back-edge -> pointer (and never fixed-size).
        // A reference chain at kMaxChainDepth (m_visiting holds the in-flight chain) -> pointer
        // too, bounding the recursion; the target's own layout is computed later, memoized.
        if (m_visiting.size() < kMaxChainDepth && m_visiting.find(target_fqn) == m_visiting.end() &&
            m_index.messages.find(target_fqn) != m_index.messages.end()) {
            const MessageLayout& target = layout_for(target_fqn);
            if (target.fixed_size && target.size <= m_opts.inline_submsg_cutoff) {
                return {FieldKind::InlineFixedSubMsg, target.size, target.align};
            }
        }
        return {FieldKind::PointerSubMsg, kPtrSize, kPtrAlign};
    }

    // Classify a oneof member as byte storage (no bit-packing inside a union), filling `member` in place.
    void classify_byte_member(const FieldNode& field, OneofMemberPlan& member) {
        if (field.is_message_type) {
            const Store store = classify_message(field.resolved_type_fqn);
            member.kind = store.kind;
            member.size = store.size;
            member.align = store.align;
            member.repr = field.resolved_type_fqn;
            member.target_fqn = field.resolved_type_fqn;
        } else if (field.is_enum_type) {
            member.kind = FieldKind::InlineEnum;
            member.size = kEnumSize;
            member.align = kEnumAlign;
            member.repr = field.resolved_type_fqn;
            member.target_fqn = field.resolved_type_fqn;
        } else if (is_string_field(field)) {
            member.kind = FieldKind::BorrowString;
            member.size = kStringSize;
            member.align = kStringAlign;
            member.repr = "ArenaString";
        } else {
            const ScalarInfo* info = scalar_info(field.type_name);
            assert(info != nullptr && "resolved non-message/enum field must be a known scalar");
            member.kind = FieldKind::InlineScalar;
            member.size = info->size;
            member.align = info->align;
            member.repr = std::string(field.type_name);
        }
    }

    [[nodiscard]] FieldMode mode_of(const FieldNode& field) const {
        if (m_opts.modes == nullptr) {
            return FieldMode::Materialize;
        }
        const auto it = m_opts.modes->fields.find(&field);
        return it != m_opts.modes->fields.end() ? it->second : FieldMode::Materialize;
    }
    [[nodiscard]] FieldMode mode_of(const MapFieldNode& map) const {
        if (m_opts.modes == nullptr) {
            return FieldMode::Materialize;
        }
        const auto it = m_opts.modes->maps.find(&map);
        return it != m_opts.modes->maps.end() ? it->second : FieldMode::Materialize;
    }

    // A `raw` member's storage: the message field's borrowed PAYLOAD -- an ArenaString view into the
    // input when singular (null data encodes absence, like a materialized pointer; a present empty
    // sub-message keeps a non-null empty view), an ArrayView<ArenaString> (one payload per element)
    // when repeated. Same storage as a string/bytes field -- a raw payload is just a borrowed byte
    // span handed to the field type's own decode(). No mask bits. target_fqn names the payload's type
    // for the dump; the planner never recurses into it -- deferring that decode is the point.
    static MemberPlan raw_member(const FieldNode& field) {
        MemberPlan plan;
        plan.field = &field;
        plan.kind = FieldKind::Raw;
        // Singular raw is a bare ArenaString; repeated raw an ArrayView<ArenaString>. The ArenaString
        // and the ArrayView are both 12-byte / 4-align cells, so the size/align is the same either way.
        static_assert(kStringSize == kViewSize && kStringAlign == kViewAlign);
        plan.size = kStringSize;
        plan.align = kStringAlign;
        plan.repr = field.is_repeated ? "ArrayView<ArenaString>" : "ArenaString";
        plan.target_fqn = field.resolved_type_fqn;
        return plan;
    }

    MemberPlan classify_field(const FieldNode& field) {
        MemberPlan plan;
        plan.field = &field;
        if (field.is_repeated) {
            plan.kind = FieldKind::Repeated;
            plan.size = kViewSize;
            plan.align = kViewAlign;
            plan.repr = "ArrayView<" + elem_repr(field) + ">";
            if (field.is_message_type || field.is_enum_type) {
                plan.target_fqn = field.resolved_type_fqn;
            }
            return plan;
        }
        if (field.is_message_type) {
            const Store store = classify_message(field.resolved_type_fqn);
            plan.kind = store.kind;
            plan.size = store.size;
            plan.align = store.align;
            plan.repr = field.resolved_type_fqn;
            plan.target_fqn = field.resolved_type_fqn;
        } else if (field.is_enum_type) {
            plan.kind = FieldKind::InlineEnum;
            plan.size = kEnumSize;
            plan.align = kEnumAlign;
            plan.repr = field.resolved_type_fqn;
            plan.target_fqn = field.resolved_type_fqn;
        } else if (is_string_field(field)) {
            plan.kind = FieldKind::BorrowString;
            plan.size = kStringSize;
            plan.align = kStringAlign;
            plan.repr = "ArenaString";
        } else {
            const ScalarInfo* info = scalar_info(field.type_name);
            assert(info != nullptr && "resolved non-message/enum field must be a known scalar");
            plan.kind = FieldKind::InlineScalar;
            plan.repr = std::string(field.type_name);
            if (info->is_bool) {
                plan.is_bool = true;  // a value bit, not a byte
            } else {
                plan.size = info->size;
                plan.align = info->align;
            }
        }
        return plan;
    }

    EntryPlan build_entry(const MapFieldNode& map) {
        EntryPlan entry;
        // Key: an integral/string scalar (string -> ArenaString, else inline numeric; bool is
        // legal and takes the scalar path); never float/message.
        std::size_t key_size = 0;
        std::size_t key_align = 0;
        if (map.key_type == "string" || map.key_type == "bytes") {
            entry.key_kind = FieldKind::BorrowString;
            entry.key_repr = "ArenaString";
            key_size = kStringSize;
            key_align = kStringAlign;
        } else {
            const ScalarInfo* info = scalar_info(map.key_type);
            assert(info != nullptr && "a map key must be a known scalar");
            entry.key_kind = FieldKind::InlineScalar;
            entry.key_repr = std::string(map.key_type);
            key_size = info->size;
            key_align = info->align;
        }
        // Value: a synthetic FieldNode-like classification (scalar/enum/message), byte storage.
        std::size_t val_size = 0;
        std::size_t val_align = 0;
        if (map.value_is_message) {
            const Store store = classify_message(map.resolved_value_type_fqn);
            entry.value_kind = store.kind;
            entry.value_repr = map.resolved_value_type_fqn;
            entry.value_fqn = map.resolved_value_type_fqn;
            val_size = store.size;
            val_align = store.align;
        } else if (map.value_is_enum) {
            entry.value_kind = FieldKind::InlineEnum;
            entry.value_repr = map.resolved_value_type_fqn;
            entry.value_fqn = map.resolved_value_type_fqn;
            val_size = kEnumSize;
            val_align = kEnumAlign;
        } else if (map.value_type == "string" || map.value_type == "bytes") {
            entry.value_kind = FieldKind::BorrowString;
            entry.value_repr = "ArenaString";
            val_size = kStringSize;
            val_align = kStringAlign;
        } else {
            const ScalarInfo* info = scalar_info(map.value_type);
            assert(info != nullptr &&
                   "a resolved non-message/enum map value must be a known scalar");
            entry.value_kind = FieldKind::InlineScalar;
            entry.value_repr = std::string(map.value_type);
            val_size = info->size;
            val_align = info->align;
        }
        // Lay out {key, value} to get the entry's size/align + member offsets.
        std::vector<Slot> slots = {{key_size, key_align, 0, 0, &entry.key_offset},
                                   {val_size, val_align, 1, 1, &entry.value_offset}};
        const StructExtent ext = order_slots(slots);
        entry.size = ext.size;
        entry.align = ext.align;
        return entry;
    }

    OneofPlan classify_oneof(const OneofNode& oneof) {
        OneofPlan plan;
        plan.oneof = &oneof;
        for (const FieldNode& field : oneof.fields) {
            OneofMemberPlan member;
            member.field = &field;
            classify_byte_member(field, member);
            plan.union_size = std::max(plan.union_size, member.size);
            plan.union_align = std::max(plan.union_align, member.align);
            plan.members.push_back(std::move(member));
        }
        if (plan.union_align == 0) {
            plan.union_align = 1;
        }
        return plan;
    }

    static bool needs_presence(const MemberPlan& m) {
        if (m.field == nullptr || m.field->presence != FieldPresence::Explicit) {
            return false;  // Implicit/Required carry no resting presence bit; maps have no field
        }
        switch (m.kind) {
            case FieldKind::InlineScalar:
            case FieldKind::InlineEnum:
            case FieldKind::BorrowString:
            case FieldKind::InlineFixedSubMsg:
                return true;
            case FieldKind::PointerSubMsg:  // null encodes absence
            case FieldKind::Raw:            // null DATA encodes absence (a present-empty payload
                                            // borrows a non-null input pointer)
            case FieldKind::Repeated:       // empty view encodes absence
            case FieldKind::Map:
                return false;
        }
        return false;
    }
    static bool needs_value(const MemberPlan& m) {
        return m.is_bool;  // an InlineScalar bool occupies a value bit, not a byte
    }

    const MessageLayout& compute(const MessageNode& message) {
        m_visiting.insert(message.fqn);
        MessageLayout layout;
        layout.message = &message;
        layout.fqn = message.fqn;

        for (const FieldNode& field : message.fields) {
            switch (mode_of(field)) {
                case FieldMode::Drop:
                    layout.dropped.push_back(&field);
                    continue;
                case FieldMode::Raw:
                    layout.members.push_back(raw_member(field));
                    continue;
                case FieldMode::Materialize:
                    break;
            }
            layout.members.push_back(classify_field(field));
        }
        for (const MapFieldNode& map : message.map_fields) {
            switch (mode_of(map)) {
                case FieldMode::Drop:
                    layout.dropped_maps.push_back(&map);
                    continue;
                case FieldMode::Raw:
                    assert(false && "raw maps are rejected at mode resolution");
                    break;  // fall through to materialize: never reachable from resolve
                case FieldMode::Materialize:
                    break;
            }
            MemberPlan plan;
            plan.map_field = &map;
            plan.kind = FieldKind::Map;
            plan.size = kViewSize;
            plan.align = kViewAlign;
            plan.repr = "MapView";
            plan.target_fqn =
                map.value_is_message || map.value_is_enum ? map.resolved_value_type_fqn : "";
            plan.entry = build_entry(map);
            layout.members.push_back(std::move(plan));
        }
        for (const OneofNode& oneof : message.oneofs) {
            layout.oneofs.push_back(classify_oneof(oneof));
        }

        allocate_bits(layout);
        order_members(layout);
        layout.fixed_size = compute_fixed_size(layout);

        m_visiting.erase(message.fqn);
        return m_memo.emplace(message.fqn, std::move(layout)).first->second;
    }

    void allocate_bits(MessageLayout& layout) const {
        int next = 0;
        for (MemberPlan& m : layout.members) {  // declaration order: stable bit assignment
            if (needs_presence(m)) {
                m.presence_bit = next++;
            }
            if (needs_value(m)) {
                m.value_bit = next++;
            }
        }
        if (m_opts.modes != nullptr && m_opts.modes->wants_unknown(layout.message)) {
            layout.unknown_bit = next++;
        }
        layout.mask_bits = next;
        if (next == 0) {
            return;
        }
        if (next <= kBitsPerWord) {  // smallest 1/2/4/8-byte word that covers `next` bits
            std::size_t bytes = 1;
            while (bytes * kBitsPerByte < static_cast<std::size_t>(next)) {
                bytes *= 2;
            }
            layout.mask_size = layout.mask_align = bytes;
        } else {  // more than one word: an array of uint64
            const std::size_t words =
                (static_cast<std::size_t>(next) + kBitsPerWord - 1) / kBitsPerWord;
            layout.mask_size = kBytesPerWord * words;
            layout.mask_align = kBytesPerWord;
        }
    }

    static void order_members(MessageLayout& layout) {
        std::vector<Slot> slots;
        int seq = 0;
        for (MemberPlan& m : layout.members) {
            if (m.size > 0) {
                slots.push_back({m.size, m.align, member_tiebreak(m), seq++, &m.offset});
            }
        }
        for (OneofPlan& o : layout.oneofs) {
            const long long tb = o.oneof->fields.empty() ? 0 : o.oneof->fields.front().number;
            slots.push_back({kDiscSize, kDiscAlign, tb, seq++, &o.disc_offset});
            slots.push_back({o.union_size, o.union_align, tb, seq++, &o.union_offset});
        }
        if (layout.mask_size > 0) {
            slots.push_back(
                {layout.mask_size, layout.mask_align, kMaskTiebreak, seq++, &layout.mask_offset});
        }
        const StructExtent ext = order_slots(slots);
        layout.size = ext.size;
        layout.align = ext.align;

        // Dump order: byte members by offset, then bit-only members by field number.
        std::stable_sort(layout.members.begin(), layout.members.end(),
                         [](const MemberPlan& a, const MemberPlan& b) {
                             const bool ab = a.size == 0;
                             const bool bb = b.size == 0;
                             if (ab != bb) {
                                 return !ab;  // byte members first
                             }
                             if (!ab) {
                                 return a.offset < b.offset;
                             }
                             const int an = a.field != nullptr ? a.field->number : 0;
                             const int bn = b.field != nullptr ? b.field->number : 0;
                             return an < bn;
                         });
    }

    static bool compute_fixed_size(const MessageLayout& layout) {
        if (!layout.oneofs.empty()) {
            return false;
        }
        for (const MemberPlan& m : layout.members) {
            switch (m.kind) {
                case FieldKind::InlineScalar:
                case FieldKind::InlineEnum:
                case FieldKind::InlineFixedSubMsg:
                    break;  // all fixed
                case FieldKind::BorrowString:
                case FieldKind::PointerSubMsg:
                case FieldKind::Repeated:
                case FieldKind::Map:
                case FieldKind::Raw:
                    return false;  // indirection or self-reference
            }
        }
        return true;
    }
};

void walk_messages(const MessageNode& message, Planner& planner, LayoutSet& out) {
    out.by_fqn.emplace(message.fqn, out.layouts.size());
    out.layouts.push_back(planner.layout_for(message.fqn));
    for (const MessageNode& nested : message.nested_messages) {
        walk_messages(nested, planner, out);
    }
}

}  // namespace

const char* kind_name(FieldKind kind) {
    switch (kind) {
        case FieldKind::InlineScalar:
            return "inline-scalar";
        case FieldKind::InlineEnum:
            return "inline-enum";
        case FieldKind::BorrowString:
            return "borrow-string";
        case FieldKind::InlineFixedSubMsg:
            return "inline-submsg";
        case FieldKind::PointerSubMsg:
            return "pointer-submsg";
        case FieldKind::Repeated:
            return "repeated";
        case FieldKind::Map:
            return "map";
        case FieldKind::Raw:
            return "raw-payload";
    }
    return "?";
}

const MessageLayout* LayoutSet::find(const std::string& fqn) const {
    const auto it = by_fqn.find(fqn);
    return it != by_fqn.end() ? &layouts[it->second] : nullptr;
}

// ── flatten budget ────────────────────────────────────────────────────────────────────────────
//
// Forcing RP_FLATTEN on every rp_decode_into buys decode throughput GCC's inliner would not give on
// its own, but it inlines each message's whole transitive sub-message closure, so on a large schema
// a single decoder absorbs most of the rest and build time explodes. This pass is what keeps that
// price affordable -- without it, forcing flatten is not viable on a large schema. It bounds it by:
// walk the message graph so a message is visited after everything it would inline, accumulate the
// closure's cost, and when it exceeds the budget mark the message RP_NOINLINE -- which stops a
// parent's flatten at it, and (because its own contribution then drops to zero) bounds every
// ancestor too.
//
// Cost is measured in DECODE ARMS, a proxy for emitted code: the generated decode grows with fields,
// including map fields, plus oneof members. It is a proxy, not a measurement -- the budget is tuned
// on compile cost, so the proxy only has to be monotone in the real thing.
//
// A cycle (a self- or mutually-recursive message) is broken by marking the one message the back edge
// points at; that is enough to stop flatten going round, and the rest of the cycle stays flattened.
// Which message that is follows traversal order -- deterministic, but not meaningfully choosable.
// Left alone, flatten's expansion around a cycle is bounded only by the compiler's inline-recursion
// limit, which is not a contract we control.
namespace {

std::size_t own_arms(const MessageLayout& layout) {
    std::size_t arms = layout.members.size();
    for (const OneofPlan& oneof : layout.oneofs) {
        arms += oneof.members.size();
    }
    return arms;
}

// The sub-message layouts this message's decoder would inline, as indices into `layouts.layouts`:
// every member that decodes through another message's rp_decode_into. Map entries decode their
// value inline, so a message-valued map contributes its value type too. Indices rather than
// pointers so the walk can reach `seen` without hashing the FQN a second time.
std::vector<std::size_t> inlined_targets(const MessageLayout& layout, const LayoutSet& layouts) {
    std::vector<std::size_t> out;
    auto add = [&](const std::string& fqn) {
        if (fqn.empty()) {
            return;
        }
        const auto it = layouts.by_fqn.find(fqn);
        if (it != layouts.by_fqn.end()) {
            out.push_back(it->second);
        }
    };
    for (const MemberPlan& member : layout.members) {
        switch (member.kind) {
            case FieldKind::InlineFixedSubMsg:
            case FieldKind::PointerSubMsg:
            case FieldKind::Repeated:
                add(member.target_fqn);
                break;
            case FieldKind::Map:
                if (member.entry) {
                    add(member.entry->value_fqn);
                }
                break;
            case FieldKind::InlineScalar:
            case FieldKind::InlineEnum:
            case FieldKind::BorrowString:
            case FieldKind::Raw:
                break;  // scalars, strings, enums and raw payloads decode no sub-message; a new
                        // message-referencing kind must be ADDED here or it escapes the budget
        }
    }
    for (const OneofPlan& oneof : layout.oneofs) {
        for (const OneofMemberPlan& member : oneof.members) {
            add(member.target_fqn);
        }
    }
    return out;
}

enum class Visit : std::uint8_t { Unvisited, InProgress, Done };

// Saturate rather than wrap: a caller may set any budget, and a shared subgraph makes cost grow
// per edge, so a large budget can otherwise overflow into a smaller number and invert the verdict.
std::size_t add_cost(std::size_t cost, std::size_t more) {
    const std::size_t limit = std::numeric_limits<std::size_t>::max();
    return cost > limit - more ? limit : cost + more;
}

// Settle one message once everything it would inline has been settled: its own arms plus the
// accumulated cost of every closure it still inlines. Over budget -> mark it. A marked message is
// then skipped by every parent's accumulation below, so it contributes nothing upward (a parent
// makes a call instead of absorbing this body), which bounds every ancestor too.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): message index vs budget, distinct roles
void settle(LayoutSet& layouts, std::size_t index, std::size_t budget) {
    MessageLayout& layout = layouts.layouts[index];
    std::size_t cost = own_arms(layout);
    bool inlines_any = false;
    for (const std::size_t target : inlined_targets(layout, layouts)) {
        const MessageLayout& settled = layouts.layouts[target];
        if (!settled.noinline_decode) {
            cost = add_cost(cost, settled.flatten_cost);
            inlines_any = true;
        }
    }
    // Kept whether or not this message is marked: it is what the threshold was compared against,
    // and the layout dump reports it so a budget change is reviewable as text.
    layout.flatten_cost = cost;
    // Only a message that STILL inlines something is a candidate: marking a leaf, or one whose every
    // target is already marked, bounds nothing BELOW it and only costs a call per occurrence. The
    // gap that leaves (such a message stays flattened however wide, so a parent still absorbs its
    // whole body) is documented on LayoutOptions::flatten_budget.
    if (cost > budget && inlines_any) {
        layout.noinline_decode = true;
    }
}

// Queue this message's un-settled targets, and break any cycle found on the way: a target still
// InProgress is an ancestor on the current path, so the two are mutually recursive -- mark it and
// stop.
void expand(const MessageLayout& layout, LayoutSet& layouts, std::vector<Visit>& seen,
            std::vector<std::pair<std::size_t, bool>>& stack) {
    for (const std::size_t target : inlined_targets(layout, layouts)) {
        if (seen[target] == Visit::InProgress) {
            layouts.layouts[target].noinline_decode = true;
        } else if (seen[target] == Visit::Unvisited) {
            stack.emplace_back(target, false);
        }
    }
}

void apply_flatten_budget(LayoutSet& layouts, std::size_t budget) {
    if (budget == 0) {
        return;  // threshold disabled: flatten everything, whatever the closure size
    }
    std::vector<Visit> seen(layouts.layouts.size(), Visit::Unvisited);
    // Iterative post-order: recursion here would be unbounded on a deep schema, which is the very
    // shape this pass exists for. One stack, reused across roots -- a corpus schema has thousands.
    std::vector<std::pair<std::size_t, bool>> stack;
    for (std::size_t root = 0; root < layouts.layouts.size(); ++root) {
        if (seen[root] != Visit::Unvisited) {
            continue;  // already settled through an earlier root
        }
        stack.assign(1, {root, false});
        while (!stack.empty()) {
            const auto [index, settled] = stack.back();
            stack.pop_back();
            if (settled) {
                settle(layouts, index, budget);
                seen[index] = Visit::Done;
            } else if (seen[index] == Visit::Unvisited) {
                seen[index] = Visit::InProgress;
                stack.emplace_back(index, true);
                expand(layouts.layouts[index], layouts, seen, stack);
            }
        }
    }
}

}  // namespace

LayoutSet plan_layouts(const ResolvedFileSet& set, const SymbolTable& symbols,
                       const LayoutOptions& options) {
    Planner planner(symbols, options);
    LayoutSet out;
    for (const FileNode& file : set.files) {
        for (const MessageNode& message : file.messages) {
            walk_messages(message, planner, out);
        }
    }
    apply_flatten_budget(out, options.flatten_budget);
    return out;
}

}  // namespace rapidproto::arenagen
