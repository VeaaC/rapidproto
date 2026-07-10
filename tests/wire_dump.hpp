#pragma once

// Test-support: serialize the structural decode of a protobuf wire buffer to a
// deterministic, diff-friendly text dump for the golden tests. Type-agnostic — it shows
// every field's wire type and raw value plus the caller-applied interpretations, and for
// LEN payloads renders BOTH the raw bytes and, when the span fully re-parses, a nested
// message (the type-agnostic ambiguity is intentional and pinned by the golden). This is
// NOT protobuf serialization; it only exists to make wire structure assertable.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "rapidproto/runtime.hpp"

namespace rapidproto::wiredump {

// One structural record, mirroring the old WireReader::WireField (which the runtime no longer ships).
// The payload variant is keyed by wire type: Varint -> uint64_t, I64 -> uint64_t, I32 -> uint32_t,
// Len -> ByteView (payload span), SGroup -> ByteView (the group body span).
struct DumpField {
    std::uint32_t field_number;
    WireType wire_type;
    std::variant<std::uint64_t, std::uint32_t, ByteView> payload;
};

// Collect a whole buffer's fields in declared order via the wire:: free readers -- the exact
// accept/reject and value semantics of the removed read_message/WireReader::read_field pull API
// (drive read_tag_or_end, then read the value per wire type). On the first wire error returns nullopt
// and, if out_error is non-null, writes the WireError (offsets are irrelevant to the dump).
inline std::optional<std::vector<DumpField>> collect_fields(ByteView input, WireError* out_error) {
    const std::uint8_t* p = wire::byte_ptr(input);
    const std::uint8_t* const begin = p;
    const std::uint8_t* const end = p + input.size();
    std::vector<DumpField> fields;
    std::size_t fail_off = 0;  // unused by the dump; the readers still want a slot
    while (true) {
        Tag tag{};
        WireError err = WireError::None;
        wire::TagState state = wire::TagState::End;
        p = wire::read_tag_or_end(p, end, &tag, &err, &state);
        if (state == wire::TagState::End) {
            return fields;
        }
        if (state == wire::TagState::Error) {
            if (out_error != nullptr) {
                *out_error = err;
            }
            return std::nullopt;
        }
        switch (tag.wire_type) {
            case WireType::Varint: {
                std::uint64_t value = 0;
                p = wire::read_varint(p, end, &value, &err);
                if (p == nullptr) {
                    break;
                }
                fields.push_back({tag.field_number, tag.wire_type, value});
                continue;
            }
            case WireType::I64: {
                std::uint64_t value = 0;
                p = wire::read_fixed64(p, end, &value, &err);
                if (p == nullptr) {
                    break;
                }
                fields.push_back({tag.field_number, tag.wire_type, value});
                continue;
            }
            case WireType::I32: {
                std::uint32_t value = 0;
                p = wire::read_fixed32(p, end, &value, &err);
                if (p == nullptr) {
                    break;
                }
                fields.push_back({tag.field_number, tag.wire_type, value});
                continue;
            }
            case WireType::Len: {
                ByteView span;
                p = wire::read_length_delimited(p, end, &span, &err);
                if (p == nullptr) {
                    break;
                }
                fields.push_back({tag.field_number, tag.wire_type, span});
                continue;
            }
            case WireType::SGroup: {
                ByteView body;
                p = wire::read_group(p, end, begin, tag.field_number, &body, &err, &fail_off);
                if (p == nullptr) {
                    break;
                }
                fields.push_back({tag.field_number, tag.wire_type, body});
                continue;
            }
            case WireType::EGroup:
                err =
                    WireError::UnexpectedEndGroup;  // matches read_field's top-level EGROUP reject
                break;
        }
        // A value read failed (or a stray EGROUP): report the first error and stop.
        if (out_error != nullptr) {
            *out_error = err;
        }
        return std::nullopt;
    }
}

class WireDumper {
public:
    std::string dump(ByteView input) {
        dump_message(input);
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
        explicit Indent(WireDumper& dumper) : m_dumper(dumper) { ++m_dumper.m_indent; }
        ~Indent() { --m_dumper.m_indent; }
        Indent(const Indent&) = delete;
        Indent& operator=(const Indent&) = delete;
        Indent(Indent&&) = delete;
        Indent& operator=(Indent&&) = delete;
        WireDumper& m_dumper;
    };

    static std::string escape(ByteView bytes) {
        std::string out;
        for (const char raw : bytes) {
            const auto ch = static_cast<unsigned char>(raw);
            if (ch == '\\' || ch == '"') {
                out += '\\';
                out += static_cast<char>(ch);
            } else if (ch >= 0x20 && ch <= 0x7e) {
                out += static_cast<char>(ch);
            } else {
                std::array<char, 8> buf{};
                std::snprintf(buf.data(), buf.size(), "\\x%02x", ch);
                out += buf.data();
            }
        }
        return out;
    }

    static std::string format_double(double value) {
        if (std::isnan(value)) {
            return "nan";
        }
        if (std::isinf(value)) {
            return value < 0 ? "-inf" : "inf";
        }
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), "%g", value);
        return buf.data();
    }

    static std::string format_float(float value) {
        return format_double(static_cast<double>(value));
    }

    static std::string hex(std::uint64_t value, int width) {
        std::array<char, 24> buf{};
        std::snprintf(buf.data(), buf.size(), "%0*llx", width,
                      static_cast<unsigned long long>(value));
        return buf.data();
    }

    void dump_message(ByteView input) {
        WireError error = WireError::None;
        const std::optional<std::vector<DumpField>> fields = collect_fields(input, &error);
        if (!fields) {
            line("decode-error code=" + std::to_string(static_cast<int>(error)));
            return;
        }
        for (const DumpField& field : *fields) {
            dump_field(field);
        }
    }

    void dump_field(const DumpField& field) {
        const std::string head = "field=" + std::to_string(field.field_number) + " wire=";
        switch (field.wire_type) {
            case WireType::Varint: {
                const auto value = std::get<std::uint64_t>(field.payload);
                line(head + "VARINT u" + std::to_string(value) +
                     " i64=" + std::to_string(varint_to_int64(value)) +
                     " zz64=" + std::to_string(zigzag_decode_64(value)));
                break;
            }
            case WireType::I64: {
                const auto value = std::get<std::uint64_t>(field.payload);
                line(head + "I64 bits=0x" + hex(value, 16) +
                     " f64=" + format_double(bit_cast_double(value)));
                break;
            }
            case WireType::I32: {
                const auto value = std::get<std::uint32_t>(field.payload);
                line(head + "I32 bits=0x" + hex(value, 8) +
                     " f32=" + format_float(bit_cast_float(value)));
                break;
            }
            case WireType::Len: {
                const ByteView span = std::get<ByteView>(field.payload);
                line(head + "LEN len=" + std::to_string(span.size()));
                const Indent indent(*this);
                line("as-bytes \"" + escape(span) + "\"");
                WireError sub_error = WireError::None;
                const std::optional<std::vector<DumpField>> sub = collect_fields(span, &sub_error);
                if (sub && !sub->empty()) {
                    line("as-message");
                    const Indent inner(*this);
                    for (const DumpField& nested : *sub) {
                        dump_field(nested);
                    }
                }
                break;
            }
            case WireType::SGroup: {
                line(head + "GROUP");
                const Indent indent(*this);
                dump_message(std::get<ByteView>(field.payload));
                break;
            }
            case WireType::EGroup:
                line(head + "EGROUP");  // never produced by read_message; defensive
                break;
        }
    }
};

inline std::string dump_wire(ByteView input) {
    return WireDumper().dump(input);
}

}  // namespace rapidproto::wiredump
