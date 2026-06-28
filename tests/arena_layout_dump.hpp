#pragma once

// Test-support: serialize an arena LayoutSet (the output of arenagen::plan_layouts) to deterministic,
// diff-friendly text. Mirrors ast_dump.hpp in spirit -- two-space indentation, one node per line,
// attributes in a fixed order -- and exists only to pin every layout decision (field kind, memory
// order + offsets, the bit-packed presence/value mask, fixed-size verdict, inline-vs-pointer
// sub-message choice, bool-wrapper collapse, oneof union) in a golden, before any C++ is emitted.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "rapidproto/arenagen/layout.hpp"

namespace rapidproto::arenalayoutdump {

class Dumper {
public:
    std::string dump(const arenagen::LayoutSet& set) {
        for (const arenagen::MessageLayout& layout : set.layouts) {
            dump_message(layout);
        }
        return std::move(m_out);
    }

private:
    std::string m_out;
    int m_indent = 0;

    void line(const std::string& text) {
        for (int i = 0; i < m_indent; ++i) {
            m_out += "  ";
        }
        m_out += text;
        m_out += '\n';
    }

    struct Indent {
        explicit Indent(Dumper& dumper) : m_dumper(dumper) { ++m_dumper.m_indent; }
        ~Indent() { --m_dumper.m_indent; }
        Indent(const Indent&) = delete;
        Indent& operator=(const Indent&) = delete;
        Indent(Indent&&) = delete;
        Indent& operator=(Indent&&) = delete;
        Dumper& m_dumper;
    };

    static std::string num(std::size_t value) { return std::to_string(value); }

    static std::string member_name(const arenagen::MemberPlan& m) {
        if (m.field != nullptr) {
            return m.field->name;
        }
        return m.map_field != nullptr ? m.map_field->name : "?";
    }

    // The storage mechanism that encodes presence. A `required` field additionally carries no resting
    // bit (validated transiently at parse); that is shown separately by dump_member.
    static std::string presence_str(const arenagen::MemberPlan& m) {
        if (m.presence_bit >= 0) {
            return " presence=bit" + std::to_string(m.presence_bit);
        }
        switch (m.kind) {
            case arenagen::FieldKind::PointerSubMsg:
                return " presence=ptr";
            case arenagen::FieldKind::Repeated:
            case arenagen::FieldKind::Map:
                return " presence=empty";
            default:
                return " presence=none";
        }
    }

    void dump_member(const arenagen::MemberPlan& m) {
        if (m.kind == arenagen::FieldKind::Map) {
            line("map " + member_name(m) + " num=" + std::to_string(m.map_field->number) +
                 " off=" + num(m.offset) + " size=" + num(m.size) + " align=" + num(m.align) +
                 " presence=empty");
            const Indent indent(*this);
            const arenagen::EntryPlan& e = *m.entry;
            line("entry key=" + std::string(arenagen::kind_name(e.key_kind)) + ":" + e.key_repr +
                 " value=" + arenagen::kind_name(e.value_kind) + ":" + e.value_repr +
                 " size=" + num(e.size) + " align=" + num(e.align));
            return;
        }
        std::string head = "field " + member_name(m);
        if (m.field != nullptr) {
            head += " num=" + std::to_string(m.field->number);
        }
        head += " " + std::string(arenagen::kind_name(m.kind)) + " " + m.repr;
        if (m.size > 0) {
            head += " off=" + num(m.offset) + " size=" + num(m.size) + " align=" + num(m.align);
        } else {
            head += " off=- size=- align=-";  // bit-only (bool / bool-wrapper)
        }
        head += presence_str(m);
        if (m.value_bit >= 0) {
            head += " value=bit" + std::to_string(m.value_bit);
        }
        if (m.field != nullptr && m.field->presence == FieldPresence::Required) {
            head +=
                " required";  // the generated decoder validates presence transiently; no resting bit
        }
        line(head);
    }

    void dump_oneof(const arenagen::OneofPlan& o) {
        line("oneof " + o.oneof->name + " disc-off=" + num(o.disc_offset) +
             " union-off=" + num(o.union_offset) + " union-size=" + num(o.union_size) +
             " union-align=" + num(o.union_align));
        const Indent indent(*this);
        for (const arenagen::OneofMemberPlan& member : o.members) {
            line("member " + member.field->name + " num=" + std::to_string(member.field->number) +
                 " " + arenagen::kind_name(member.kind) + " " + member.repr +
                 " size=" + num(member.size) + " align=" + num(member.align));
        }
    }

    void dump_mask(const arenagen::MessageLayout& layout) {
        if (layout.mask_bits == 0) {
            return;
        }
        line("mask off=" + num(layout.mask_offset) + " size=" + num(layout.mask_size) +
             " align=" + num(layout.mask_align) + " bits=" + std::to_string(layout.mask_bits));
        std::vector<std::pair<int, std::string>> bits;
        for (const arenagen::MemberPlan& m : layout.members) {
            if (m.presence_bit >= 0) {
                bits.emplace_back(m.presence_bit, "presence " + member_name(m));
            }
            if (m.value_bit >= 0) {
                bits.emplace_back(m.value_bit, "value " + member_name(m));
            }
        }
        if (layout.unknown_bit >= 0) {
            bits.emplace_back(layout.unknown_bit, "unknown");
        }
        std::sort(bits.begin(), bits.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        const Indent indent(*this);
        for (const auto& [index, desc] : bits) {
            line("bit " + std::to_string(index) + " " + desc);
        }
    }

    void dump_message(const arenagen::MessageLayout& layout) {
        std::string head = "message " + layout.fqn + " size=" + num(layout.size) +
                           " align=" + num(layout.align) +
                           " fixed=" + (layout.fixed_size ? "yes" : "no");
        if (layout.is_bool_wrapper) {
            head += " bool-wrapper";
        }
        line(head);
        const Indent indent(*this);
        for (const arenagen::MemberPlan& m : layout.members) {
            dump_member(m);
        }
        for (const arenagen::OneofPlan& o : layout.oneofs) {
            dump_oneof(o);
        }
        dump_mask(layout);
    }
};

inline std::string dump_layouts(const arenagen::LayoutSet& set) {
    return Dumper().dump(set);
}

}  // namespace rapidproto::arenalayoutdump
