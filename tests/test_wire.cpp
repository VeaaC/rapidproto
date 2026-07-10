// Primitive unit tests for the type-agnostic wire reader -- the rapidproto::wire free functions
// (read_varint / read_tag / read_tag_or_end / read_fixed32 / read_fixed64 / read_length_delimited /
// skip_value / read_group). Hand-authored byte buffers exercise every primitive and every WireError,
// plus a structural walk of the checked-in protoc fixtures. No external tools needed.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <variant>  // IWYU pragma: keep (std::get over the payload variant)
#include <vector>

#include "rapidproto/runtime.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

using Bytes = std::vector<std::uint8_t>;

ByteView view(const Bytes& bytes) {
    return byte_view(bytes.data(), bytes.size());
}
ByteView view(Bytes&&) = delete;  // forbid views into a temporary (would dangle)

// Unwrap an optional a helper returned, failing the test if empty. The explicit guard
// (not just REQUIRE) is what clang-tidy's optional-access model recognizes.
template <typename T>
T must(const std::optional<T>& opt) {
    REQUIRE(opt.has_value());
    if (!opt.has_value()) {
        return T{};
    }
    return *opt;
}

// Exact bit-pattern comparison (avoids -Wfloat-equal; correct for exactly-representable values).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): commutative comparison
bool bits_equal_float(float a, float b) noexcept {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): commutative comparison
bool bits_equal_double(double a, double b) noexcept {
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}

// --- thin, buffer-oriented drivers over the wire:: free readers -------------------------------
// Each reads from the FRONT of the buffer, returning success + the decoded value + the WireError.
// A leaf reader fails at the entry cursor, so its fail offset is definitionally 0 here.

struct VarintRead {
    bool ok;
    std::uint64_t value;
    WireError err;
};
VarintRead decode_varint(const Bytes& bytes) {
    const std::uint8_t* const p = bytes.data();
    WireError err = WireError::None;
    std::uint64_t value = 0;
    const std::uint8_t* const np = wire::read_varint(p, p + bytes.size(), &value, &err);
    return {np != nullptr, value, err};
}

struct TagRead {
    bool ok;
    Tag tag;
    WireError err;
};
TagRead decode_tag(const Bytes& bytes) {
    const std::uint8_t* const p = bytes.data();
    WireError err = WireError::None;
    Tag tag{};
    const std::uint8_t* const np = wire::read_tag(p, p + bytes.size(), &tag, &err);
    return {np != nullptr, tag, err};
}

struct Fixed32Read {
    bool ok;
    std::uint32_t value;
    WireError err;
};
Fixed32Read decode_fixed32(const Bytes& bytes) {
    const std::uint8_t* const p = bytes.data();
    WireError err = WireError::None;
    std::uint32_t value = 0;
    const std::uint8_t* const np = wire::read_fixed32(p, p + bytes.size(), &value, &err);
    return {np != nullptr, value, err};
}

struct Fixed64Read {
    bool ok;
    std::uint64_t value;
    WireError err;
};
Fixed64Read decode_fixed64(const Bytes& bytes) {
    const std::uint8_t* const p = bytes.data();
    WireError err = WireError::None;
    std::uint64_t value = 0;
    const std::uint8_t* const np = wire::read_fixed64(p, p + bytes.size(), &value, &err);
    return {np != nullptr, value, err};
}

struct LenRead {
    bool ok;
    ByteView value;  // borrows `bytes`; the caller must keep it alive
    WireError err;
};
LenRead decode_len(const Bytes& bytes) {
    const std::uint8_t* const p = bytes.data();
    WireError err = WireError::None;
    ByteView value;
    const std::uint8_t* const np = wire::read_length_delimited(p, p + bytes.size(), &value, &err);
    return {np != nullptr, value, err};
}

// Fused end-or-tag read for a decode loop's driver: distinguishes clean end / tag / malformed tag.
struct TagOrEnd {
    wire::TagState state;
    Tag tag;
    WireError err;
};
TagOrEnd decode_tag_or_end(const Bytes& bytes) {
    const std::uint8_t* const p = bytes.data();
    WireError err = WireError::None;
    Tag tag{};
    wire::TagState state = wire::TagState::End;
    wire::read_tag_or_end(p, p + bytes.size(), &tag, &err, &state);
    return {state, tag, err};
}

// Skip a field's value whose tag (wire_type + field_number) is already consumed: the buffer is the
// value bytes. Reports success, how many bytes were consumed, and (on failure) the WireError and the
// absolute fail offset the reader wrote.
struct SkipResult {
    bool ok;
    std::size_t consumed;
    WireError err;
    std::size_t fail_off;
};
SkipResult skip_value(const Bytes& bytes, WireType wire_type, std::uint32_t field_number) {
    const std::uint8_t* const begin = bytes.data();
    const std::uint8_t* const end = begin + bytes.size();
    WireError err = WireError::None;
    std::size_t fail_off = 0;
    const std::uint8_t* const np =
        wire::skip_value(begin, end, begin, Tag{field_number, wire_type}, 0, &err, &fail_off);
    return {np != nullptr, np != nullptr ? static_cast<std::size_t>(np - begin) : 0, err, fail_off};
}

// Read a group body whose SGROUP tag is already consumed: the buffer is the body bytes followed by
// the matching EGROUP tag. Reports the body span, the total bytes consumed (through the EGROUP), and
// (on failure) the WireError and the absolute fail offset.
struct GroupRead {
    bool ok;
    ByteView body;  // borrows `bytes`
    std::size_t consumed;
    WireError err;
    std::size_t fail_off;
};
GroupRead decode_group(const Bytes& bytes, std::uint32_t field_number) {
    const std::uint8_t* const begin = bytes.data();
    const std::uint8_t* const end = begin + bytes.size();
    WireError err = WireError::None;
    std::size_t fail_off = 0;
    ByteView body;
    const std::uint8_t* const np =
        wire::read_group(begin, end, begin, field_number, &body, &err, &fail_off);
    return {np != nullptr, body, np != nullptr ? static_cast<std::size_t>(np - begin) : 0, err,
            fail_off};
}

// One structural record, mirroring the removed WireReader::WireField. The payload variant is keyed by
// wire type: Varint -> uint64_t, I64 -> uint64_t, I32 -> uint32_t, Len -> ByteView (payload span),
// SGroup -> ByteView (group body span).
struct Field {
    std::uint32_t field_number;
    WireType wire_type;
    std::variant<std::uint64_t, std::uint32_t, ByteView> payload;
};

// Walk a whole buffer into its ordered fields via the wire:: free readers -- the accept/reject and
// value semantics of the removed read_message/read_field pull API. On the first wire error returns
// nullopt and, if out_error is non-null, writes the WireError.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a flat per-wire-type dispatch
std::optional<std::vector<Field>> walk(ByteView input, WireError* out_error = nullptr) {
    const std::uint8_t* p = wire::byte_ptr(input);
    const std::uint8_t* const begin = p;
    const std::uint8_t* const end = p + input.size();
    std::vector<Field> fields;
    std::size_t fail_off = 0;
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
                err = WireError::UnexpectedEndGroup;  // a stray EGROUP is never a value
                break;
        }
        if (out_error != nullptr) {
            *out_error = err;
        }
        return std::nullopt;
    }
}

// `levels` balanced field-1 start/end group tags: <start>*levels then <end>*levels.
Bytes nested_groups(int levels) {
    Bytes bytes;
    bytes.reserve(static_cast<std::size_t>(levels) * 2);
    for (int i = 0; i < levels; ++i) {
        bytes.push_back(0x0b);  // start group, field 1
    }
    for (int i = 0; i < levels; ++i) {
        bytes.push_back(0x0c);  // end group, field 1
    }
    return bytes;
}

}  // namespace

TEST_CASE("wire: varint values", "[wire]") {
    CHECK(decode_varint({0x00}).value == 0U);
    CHECK(decode_varint({0x01}).value == 1U);
    CHECK(decode_varint({0xac, 0x02}).value == 300U);  // canonical spec example
    // 10-byte maximum: 9 * 0x7f data bits + bit 63 => UINT64_MAX.
    CHECK(decode_varint({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}).value ==
          std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("wire: varint errors set the code", "[wire]") {
    SECTION("truncated (continuation bit but buffer ends)") {
        const VarintRead r = decode_varint({0x80});
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::TruncatedVarint);
    }
    SECTION("overflow: 11th continuation byte") {
        const VarintRead r =
            decode_varint({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::VarintOverflow);
    }
    SECTION("overflow: 10th byte sets a bit beyond 63") {
        const VarintRead r =
            decode_varint({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02});
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::VarintOverflow);
    }
}

TEST_CASE("wire: read_tag_or_end distinguishes end / tag / error", "[wire]") {
    SECTION("empty buffer is a clean end") {
        const Bytes empty;
        CHECK(decode_tag_or_end(empty).state == wire::TagState::End);
        CHECK(must(walk(view(empty))).empty());  // a zero-field message is valid
    }
    SECTION("a valid tag decodes") {
        const TagOrEnd r = decode_tag_or_end({0x08});  // field 1, varint
        CHECK(r.state == wire::TagState::Tag);
        CHECK(r.tag.field_number == 1);
        CHECK(r.tag.wire_type == WireType::Varint);
    }
    SECTION("a malformed leading tag is an error, not an end") {
        const TagOrEnd r = decode_tag_or_end({0x00});  // field number 0
        CHECK(r.state == wire::TagState::Error);
        CHECK(r.err == WireError::InvalidFieldNumber);
    }
}

TEST_CASE("wire: zigzag decode", "[wire]") {
    CHECK(zigzag_decode_32(0) == 0);
    CHECK(zigzag_decode_32(1) == -1);
    CHECK(zigzag_decode_32(2) == 1);
    CHECK(zigzag_decode_32(3) == -2);
    CHECK(zigzag_decode_32(0xffffffffU) == std::numeric_limits<std::int32_t>::min());
    CHECK(zigzag_decode_64(0) == 0);
    CHECK(zigzag_decode_64(1) == -1);
    CHECK(zigzag_decode_64(2) == 1);
    CHECK(zigzag_decode_64(std::numeric_limits<std::uint64_t>::max()) ==
          std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("wire: fixed32 / float", "[wire]") {
    SECTION("little-endian assembly and float bit-cast") {
        const std::uint32_t bits = decode_fixed32({0x00, 0x00, 0x80, 0x3f}).value;  // 0x3f800000
        CHECK(bits == 0x3f800000U);
        CHECK(bits_equal_float(bit_cast_float(bits), 1.0F));
    }
    SECTION("NaN and +inf") {
        CHECK(std::isnan(bit_cast_float(0x7fc00000U)));
        CHECK(std::isinf(bit_cast_float(0x7f800000U)));
    }
    SECTION("truncated") {
        const Fixed32Read r = decode_fixed32({0x00, 0x00, 0x00});
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::TruncatedI32);
    }
}

TEST_CASE("wire: fixed64 / double", "[wire]") {
    SECTION("1.0") {
        const std::uint64_t bits =
            decode_fixed64({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f}).value;
        CHECK(bits_equal_double(bit_cast_double(bits), 1.0));
    }
    SECTION("-2.25 exact bit pattern") {  // 0xc002000000000000
        const std::uint64_t bits =
            decode_fixed64({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xc0}).value;
        CHECK(bits_equal_double(bit_cast_double(bits), -2.25));
    }
    SECTION("truncated") {
        const Fixed64Read r = decode_fixed64({0, 0, 0, 0, 0, 0, 0});
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::TruncatedI64);
    }
}

TEST_CASE("wire: tag decode and validation", "[wire]") {
    SECTION("single-byte tags") {
        const TagRead t1 = decode_tag({0x08});  // field 1, varint
        CHECK(t1.ok);
        CHECK(t1.tag.field_number == 1);
        CHECK(t1.tag.wire_type == WireType::Varint);

        const TagRead t2 = decode_tag({0x12});  // field 2, len
        CHECK(t2.ok);
        CHECK(t2.tag.field_number == 2);
        CHECK(t2.tag.wire_type == WireType::Len);
    }
    SECTION("multi-byte tag for field >= 16") {
        const TagRead t = decode_tag({0x80, 0x01});  // field 16, varint (16 << 3 = 128)
        CHECK(t.ok);
        CHECK(t.tag.field_number == 16);
        CHECK(t.tag.wire_type == WireType::Varint);
    }
    SECTION("field number 0 is invalid") {
        const TagRead t = decode_tag({0x00});
        CHECK_FALSE(t.ok);
        CHECK(t.err == WireError::InvalidFieldNumber);
    }
    SECTION("reserved wire types 6 and 7") {
        const TagRead t6 = decode_tag({0x0e});  // field 1, wire type 6
        CHECK_FALSE(t6.ok);
        CHECK(t6.err == WireError::ReservedWireType);
        const TagRead t7 = decode_tag({0x0f});  // field 1, wire type 7
        CHECK_FALSE(t7.ok);
        CHECK(t7.err == WireError::ReservedWireType);
    }
    SECTION("field number beyond 2^29-1") {
        // raw tag = 2^32 (field = 2^29 > max) encoded as a 5-byte varint.
        const TagRead t = decode_tag({0x80, 0x80, 0x80, 0x80, 0x10});
        CHECK_FALSE(t.ok);
        CHECK(t.err == WireError::FieldNumberRange);
    }
}

TEST_CASE("wire: length-delimited", "[wire]") {
    SECTION("exact-fit payload") {
        const Bytes data = {0x03, 'a', 'b', 'c'};  // LEN 3, "abc"
        const LenRead r = decode_len(data);
        CHECK(r.ok);
        CHECK(r.value == "abc");
    }
    SECTION("length exceeds buffer") {
        const Bytes data = {0x05, 'a', 'b'};  // length 5, only 2 bytes follow
        const LenRead r = decode_len(data);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::LengthExceedsBuffer);
    }
    SECTION("truncated length varint") {
        const Bytes data = {0x80};  // a length varint that never terminates
        const LenRead r = decode_len(data);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::TruncatedVarint);
    }
}

TEST_CASE("wire: structural walk over each wire type", "[wire]") {
    // field1 varint=42; field2 i64; field3 len="hi"; field4 i32; field5 group{field6 varint=7}.
    const Bytes data = {
        0x08, 0x2a,                             // field 1, varint 42
        0x11, 0,    0,    0,    0, 0, 0, 0, 0,  // field 2, i64
        0x1a, 0x02, 'h',  'i',                  // field 3, len "hi"
        0x25, 0,    0,    0,    0,              // field 4, i32
        0x2b, 0x30, 0x07, 0x2c,                 // field 5, group { field 6 varint 7 }
    };
    const std::vector<Field> fields = must(walk(view(data)));
    REQUIRE(fields.size() == 5);
    CHECK(fields[0].wire_type == WireType::Varint);
    CHECK(std::get<std::uint64_t>(fields[0].payload) == 42U);
    CHECK(fields[1].wire_type == WireType::I64);
    CHECK(fields[2].wire_type == WireType::Len);
    CHECK(std::get<ByteView>(fields[2].payload) == "hi");
    CHECK(fields[3].wire_type == WireType::I32);
    CHECK(fields[4].wire_type == WireType::SGroup);

    // The group body re-parses to its single inner field.
    const std::vector<Field> inner = must(walk(std::get<ByteView>(fields[4].payload)));
    REQUIRE(inner.size() == 1);
    CHECK(inner[0].field_number == 6);
    CHECK(std::get<std::uint64_t>(inner[0].payload) == 7U);
}

TEST_CASE("wire: duplicate field numbers are preserved in order (no merge)", "[wire]") {
    const Bytes data = {0x08, 0x01, 0x08, 0x02, 0x08, 0x03};  // field 1 = 1, 2, 3
    const std::vector<Field> fields = must(walk(view(data)));
    REQUIRE(fields.size() == 3);
    CHECK(std::get<std::uint64_t>(fields[0].payload) == 1U);
    CHECK(std::get<std::uint64_t>(fields[1].payload) == 2U);
    CHECK(std::get<std::uint64_t>(fields[2].payload) == 3U);
}

TEST_CASE("wire: skip_value advances past each wire type's value", "[wire]") {
    SECTION("LEN value (length prefix + payload)") {
        const Bytes value = {0x02, 'x', 'y'};  // a LEN value: length 2 then "xy"
        const SkipResult r = skip_value(value, WireType::Len, 1);
        CHECK(r.ok);
        CHECK(r.consumed == 3);
    }
    SECTION("I32 value") {
        const Bytes value = {0x01, 0x02, 0x03, 0x04};  // a bare 4-byte I32 value
        const SkipResult r = skip_value(value, WireType::I32, 1);
        CHECK(r.ok);
        CHECK(r.consumed == 4);
    }
    SECTION("Varint value") {
        const Bytes value = {0xac, 0x02};  // 300
        const SkipResult r = skip_value(value, WireType::Varint, 1);
        CHECK(r.ok);
        CHECK(r.consumed == 2);
    }
    SECTION("I64 value") {
        const Bytes value = {0, 0, 0, 0, 0, 0, 0, 0};
        const SkipResult r = skip_value(value, WireType::I64, 1);
        CHECK(r.ok);
        CHECK(r.consumed == 8);
    }
    SECTION("SGROUP value (its whole group body + EGROUP)") {
        const Bytes value = {0x10, 0x2a, 0x0c};  // field2 varint 42, then end-group field1
        const SkipResult r = skip_value(value, WireType::SGroup, 1);
        CHECK(r.ok);
        CHECK(r.consumed == 3);
    }
}

TEST_CASE("wire: skip_value error branches", "[wire]") {
    SECTION("truncated Varint value") {
        const Bytes value = {0x80};
        const SkipResult r = skip_value(value, WireType::Varint, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::TruncatedVarint);
        CHECK(r.fail_off == 0);
    }
    SECTION("truncated I32 value") {
        const Bytes value = {0x01, 0x02};  // only 2 of the 4 value bytes
        const SkipResult r = skip_value(value, WireType::I32, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::TruncatedI32);
    }
    SECTION("a stray end-group tag is never a valid value to skip") {
        const Bytes value = {0x00};  // content irrelevant
        const SkipResult r = skip_value(value, WireType::EGroup, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::UnexpectedEndGroup);
    }
    SECTION("skip into a group whose body opens with a malformed tag fails") {
        const Bytes value = {0x80};  // group body: a tag varint that never terminates
        const SkipResult r = skip_value(value, WireType::SGroup, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::TruncatedVarint);
    }
}

TEST_CASE("wire: read_group body span and errors", "[wire]") {
    SECTION("matched group yields the body span") {
        const Bytes data = {0x10, 0x2a, 0x0c};  // body { field2 varint 42 } then end-group field1
        const GroupRead r = decode_group(data, 1);
        CHECK(r.ok);
        const Bytes expected_body = {0x10, 0x2a};
        CHECK(r.body == view(expected_body));
        CHECK(r.consumed == data.size());  // cursor left just past the EGROUP
    }
    SECTION("empty group body") {
        const Bytes data = {0x0c};  // immediate end-group field1
        const GroupRead r = decode_group(data, 1);
        CHECK(r.ok);
        CHECK(r.body.empty());
    }
    SECTION("nested group body re-parses") {
        const Bytes data = {0x1b, 0x1c, 0x0c};  // group f3 {} then end-group f1
        const GroupRead r = decode_group(data, 1);
        CHECK(r.ok);
        const std::vector<Field> body = must(walk(r.body));
        REQUIRE(body.size() == 1);
        CHECK(body[0].field_number == 3);
        CHECK(body[0].wire_type == WireType::SGroup);
        CHECK(std::get<ByteView>(body[0].payload).empty());
    }
    SECTION("end-group field number mismatch") {
        const Bytes data = {0x14};  // end group field2, but we opened field1
        const GroupRead r = decode_group(data, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::EndGroupMismatch);
    }
    SECTION("unterminated group") {
        const Bytes empty;  // no EGROUP ever
        const GroupRead r = decode_group(empty, 1);
        CHECK_FALSE(r.ok);
        CHECK(r.err == WireError::UnterminatedGroup);
    }
}

TEST_CASE("wire: group nesting depth boundary", "[wire]") {
    SECTION("stray end-group at top level is unexpected") {
        const Bytes stray = {0x0c};  // end group with no open group
        WireError err = WireError::None;
        CHECK_FALSE(walk(view(stray), &err).has_value());
        CHECK(err == WireError::UnexpectedEndGroup);
    }
    SECTION("exactly kMaxGroupDepth levels succeed") {
        const Bytes at_cap = nested_groups(kMaxGroupDepth);
        const std::vector<Field> fields = must(walk(view(at_cap)));
        REQUIRE(fields.size() == 1);
        CHECK(fields[0].wire_type == WireType::SGroup);
    }
    SECTION("one level past the cap fails") {
        const Bytes past_cap = nested_groups(kMaxGroupDepth + 1);
        WireError err = WireError::None;
        CHECK_FALSE(walk(view(past_cap), &err).has_value());
        CHECK(err == WireError::GroupTooDeep);
    }
}

TEST_CASE("wire: structural-walk group error branches", "[wire]") {
    SECTION("end-group field number mismatch") {
        const Bytes data = {0x0b, 0x14};  // start group field1, end group field2
        WireError err = WireError::None;
        CHECK_FALSE(walk(view(data), &err).has_value());
        CHECK(err == WireError::EndGroupMismatch);
    }
    SECTION("unterminated group") {
        const Bytes data = {0x0b};  // start group, never closed
        WireError err = WireError::None;
        CHECK_FALSE(walk(view(data), &err).has_value());
        CHECK(err == WireError::UnterminatedGroup);
    }
    SECTION("walk over a LEN with a truncated length varint") {
        const Bytes data = {0x0a, 0x80};  // field1 LEN, then a length varint that never terminates
        WireError err = WireError::None;
        CHECK_FALSE(walk(view(data), &err).has_value());
        CHECK(err == WireError::TruncatedVarint);
    }
    SECTION("walk over an I32 with too few value bytes") {
        const Bytes data = {0x0d, 0x01, 0x02};  // field1 I32, only 2 of the 4 value bytes
        WireError err = WireError::None;
        CHECK_FALSE(walk(view(data), &err).has_value());
        CHECK(err == WireError::TruncatedI32);
    }
}

TEST_CASE("wire: fail offset points at the deep failure position", "[wire]") {
    // A group whose inner field is a truncated varint: the reader reports the absolute offset of the
    // failing inner byte (measured from the group body start passed as `begin`), not the group start.
    const Bytes body = {0x10, 0x80};  // field2 varint (tag at 0), value byte 0x80 truncated at 1
    const SkipResult r = skip_value(body, WireType::SGroup, 1);
    CHECK_FALSE(r.ok);
    CHECK(r.err == WireError::TruncatedVarint);
    CHECK(r.fail_off == 1);  // the truncated value byte, not offset 0
}

// --- protoc fixtures (decode the checked-in .bin; skip gracefully if absent) -------------

namespace {

std::optional<std::string> read_fixture(const std::string& name) {
    const std::string path = std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/" + name;
    const std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("wire: protoc Scalars fixture decodes", "[wire]") {
    const std::optional<std::string> bin = read_fixture("scalars.bin");
    if (!bin) {
        SUCCEED("fixture scalars.bin not present; skipping");
        return;
    }
    const std::vector<Field> fields = must(walk(ByteView(*bin)));
    CHECK_FALSE(fields.empty());
}

TEST_CASE("wire: protoc AllWire fixture decodes with a group", "[wire]") {
    const std::optional<std::string> bin = read_fixture("all_wire.bin");
    if (!bin) {
        SUCCEED("fixture all_wire.bin not present; skipping");
        return;
    }
    const std::vector<Field> fields = must(walk(ByteView(*bin)));
    bool saw_group = false;
    for (const Field& field : fields) {
        if (field.wire_type == WireType::SGroup) {
            saw_group = true;
            CHECK(walk(std::get<ByteView>(field.payload)).has_value());
        }
    }
    CHECK(saw_group);
}

// Hidden by the [.] tag: not run by the gate. Run manually with:
//   ./build/gcc/rapidproto_tests "[wire-bench]"
TEST_CASE("wire: decode throughput", "[.][wire-bench]") {
    // A buffer of many single-byte-tag varint fields (the dominant decode shape).
    const int field_count = 1 << 20;
    Bytes data;
    data.reserve(static_cast<std::size_t>(field_count) * 3);
    for (int i = 0; i < field_count; ++i) {
        data.push_back(0x08);  // field 1, varint
        const std::uint32_t v = static_cast<std::uint32_t>(i) & 0x3fffU;
        if (v < 0x80U) {
            data.push_back(static_cast<std::uint8_t>(v));
        } else {
            data.push_back(static_cast<std::uint8_t>((v & 0x7fU) | 0x80U));
            data.push_back(static_cast<std::uint8_t>(v >> 7U));
        }
    }

    const int rounds = 20;
    std::uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; ++r) {
        const std::uint8_t* p = data.data();
        const std::uint8_t* const end = p + data.size();
        WireError err = WireError::None;
        while (p < end) {
            Tag tag{};
            wire::TagState state = wire::TagState::End;
            p = wire::read_tag_or_end(p, end, &tag, &err, &state);
            if (state != wire::TagState::Tag) {
                break;
            }
            std::uint64_t val = 0;
            p = wire::read_varint(p, end, &val, &err);
            if (p == nullptr) {
                break;
            }
            sum += val;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    const double total_bytes = static_cast<double>(data.size()) * rounds;
    WARN("decoded " << total_bytes / 1e6 << " MB in " << ns / 1e6 << " ms => "
                    << total_bytes / (ns / 1e9) / 1e6 << " MB/s (checksum " << sum << ")");
    CHECK(sum > 0);
}
