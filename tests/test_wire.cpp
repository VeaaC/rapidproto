// Primitive unit tests for the type-agnostic wire reader: hand-authored byte buffers
// exercising every primitive and every WireError, plus structural decode of the
// checked-in protoc fixtures. No external tools needed.

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

// Unwrap an optional the reader returned, failing the test if empty. The explicit guard
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

std::optional<std::uint64_t> decode_varint(const Bytes& bytes) {
    WireReader reader(view(bytes));
    return reader.read_varint();
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
    CHECK(decode_varint({0x00}) == 0U);
    CHECK(decode_varint({0x01}) == 1U);
    CHECK(decode_varint({0xac, 0x02}) == 300U);  // canonical spec example
    // 10-byte maximum: 9 * 0x7f data bits + bit 63 => UINT64_MAX.
    CHECK(decode_varint({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}) ==
          std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("wire: varint errors set the code and offset", "[wire]") {
    SECTION("truncated (continuation bit but buffer ends)") {
        const Bytes data = {0x80};
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_varint().has_value());
        CHECK(reader.error_code() == WireError::TruncatedVarint);
        CHECK(reader.fail_offset() == 0);
    }
    SECTION("overflow: 11th continuation byte") {
        const Bytes data = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_varint().has_value());
        CHECK(reader.error_code() == WireError::VarintOverflow);
    }
    SECTION("overflow: 10th byte sets a bit beyond 63") {
        const Bytes data = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_varint().has_value());
        CHECK(reader.error_code() == WireError::VarintOverflow);
    }
}

// read_varint_inline is a force-inlined byte-copy of read_varint used only in packed-array loops; it
// MUST decode and fail bit-identically. Pin that invariant directly -- the deterministic decode tests
// otherwise reach the inline copy only via packed decoders fed VALID varints, leaving its overflow /
// truncation branches unguarded (so a future edit to one body and not the other would slip through).
TEST_CASE("wire: read_varint_inline is bit-identical to read_varint", "[wire]") {
    const std::vector<Bytes> inputs = {
        {},  // empty -> truncated
        {0x00},
        {0x01},
        {0x7f},
        {0xac, 0x02},                                                  // 300
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01},  // UINT64_MAX
        {0x80},                                                        // truncated
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},  // overflow (11th cont. byte)
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0x02},  // overflow (10th byte > bit 63)
    };
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        CAPTURE(i);
        WireReader ra(view(inputs[i]));
        WireReader rb(view(inputs[i]));
        const std::optional<std::uint64_t> va = ra.read_varint();
        const std::optional<std::uint64_t> vb = rb.read_varint_inline();
        CHECK(va == vb);                              // same value + same has_value
        CHECK(ra.error_code() == rb.error_code());    // same error
        CHECK(ra.fail_offset() == rb.fail_offset());  // same fail offset
        CHECK(ra.position() == rb.position());        // same cursor advance
    }
}

TEST_CASE("wire: a failed read parks the cursor", "[wire]") {
    const Bytes data = {0x80};  // truncated varint
    WireReader reader(view(data));
    CHECK_FALSE(reader.read_varint().has_value());
    CHECK(reader.failed());
    CHECK(reader.at_end());  // parked, so consumer loops terminate
    // A subsequent read still fails and the FIRST error is kept (not overwritten).
    CHECK_FALSE(reader.read_tag().has_value());
    CHECK(reader.error_code() == WireError::TruncatedVarint);
}

TEST_CASE("wire: empty buffer", "[wire]") {
    const Bytes empty;
    WireReader reader(view(empty));
    CHECK(reader.at_end());
    CHECK_FALSE(reader.read_tag().has_value());
    CHECK(must(read_message(view(empty))).empty());  // a zero-field message is valid
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
        const Bytes one = {0x00, 0x00, 0x80, 0x3f};  // 0x3f800000 = 1.0f
        WireReader reader(view(one));
        const std::uint32_t bits = must(reader.read_fixed32());
        CHECK(bits == 0x3f800000U);
        CHECK(bits_equal_float(bit_cast_float(bits), 1.0F));
    }
    SECTION("NaN and +inf") {
        CHECK(std::isnan(bit_cast_float(0x7fc00000U)));
        CHECK(std::isinf(bit_cast_float(0x7f800000U)));
    }
    SECTION("truncated") {
        const Bytes three = {0x00, 0x00, 0x00};
        WireReader reader(view(three));
        CHECK_FALSE(reader.read_fixed32().has_value());
        CHECK(reader.error_code() == WireError::TruncatedI32);
    }
}

TEST_CASE("wire: fixed64 / double", "[wire]") {
    SECTION("1.0") {
        const Bytes one = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f};
        WireReader reader(view(one));
        CHECK(bits_equal_double(bit_cast_double(must(reader.read_fixed64())), 1.0));
    }
    SECTION("-2.25 exact bit pattern") {
        const Bytes neg = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xc0};  // 0xc002000000000000
        WireReader reader(view(neg));
        CHECK(bits_equal_double(bit_cast_double(must(reader.read_fixed64())), -2.25));
    }
    SECTION("truncated") {
        const Bytes seven = {0, 0, 0, 0, 0, 0, 0};
        WireReader reader(view(seven));
        CHECK_FALSE(reader.read_fixed64().has_value());
        CHECK(reader.error_code() == WireError::TruncatedI64);
    }
}

TEST_CASE("wire: tag decode and validation", "[wire]") {
    SECTION("single-byte tags") {
        const Bytes field1_varint = {0x08};
        WireReader r1(view(field1_varint));
        const Tag t1 = must(r1.read_tag());
        CHECK(t1.field_number == 1);
        CHECK(t1.wire_type == WireType::Varint);

        const Bytes field2_len = {0x12};
        WireReader r2(view(field2_len));
        const Tag t2 = must(r2.read_tag());
        CHECK(t2.field_number == 2);
        CHECK(t2.wire_type == WireType::Len);
    }
    SECTION("multi-byte tag for field >= 16") {
        const Bytes data = {0x80, 0x01};  // field 16, varint (16 << 3 = 128)
        WireReader reader(view(data));
        const Tag tag = must(reader.read_tag());
        CHECK(tag.field_number == 16);
        CHECK(tag.wire_type == WireType::Varint);
    }
    SECTION("field number 0 is invalid") {
        const Bytes data = {0x00};
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_tag().has_value());
        CHECK(reader.error_code() == WireError::InvalidFieldNumber);
    }
    SECTION("reserved wire types 6 and 7") {
        const Bytes wt6 = {0x0e};  // field 1, wire type 6
        WireReader r6(view(wt6));
        CHECK_FALSE(r6.read_tag().has_value());
        CHECK(r6.error_code() == WireError::ReservedWireType);
        const Bytes wt7 = {0x0f};  // field 1, wire type 7
        WireReader r7(view(wt7));
        CHECK_FALSE(r7.read_tag().has_value());
        CHECK(r7.error_code() == WireError::ReservedWireType);
    }
    SECTION("field number beyond 2^29-1") {
        // raw tag = 2^32 (field = 2^29 > max) encoded as a 5-byte varint.
        const Bytes data = {0x80, 0x80, 0x80, 0x80, 0x10};
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_tag().has_value());
        CHECK(reader.error_code() == WireError::FieldNumberRange);
    }
}

TEST_CASE("wire: length-delimited", "[wire]") {
    SECTION("exact-fit payload") {
        const Bytes data = {0x0a, 0x03, 'a', 'b', 'c'};  // field 1, LEN, "abc"
        WireReader reader(view(data));
        must(reader.read_tag());
        CHECK(must(reader.read_length_delimited()) == "abc");
        CHECK(reader.at_end());
    }
    SECTION("length exceeds buffer") {
        const Bytes data = {0x05, 'a', 'b'};  // length 5, only 2 bytes follow
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_length_delimited().has_value());
        CHECK(reader.error_code() == WireError::LengthExceedsBuffer);
    }
}

TEST_CASE("wire: read_field over each wire type", "[wire]") {
    // field1 varint=42; field2 i64; field3 len="hi"; field4 i32; field5 group{field6 varint=7}.
    const Bytes data = {
        0x08, 0x2a,                             // field 1, varint 42
        0x11, 0,    0,    0,    0, 0, 0, 0, 0,  // field 2, i64
        0x1a, 0x02, 'h',  'i',                  // field 3, len "hi"
        0x25, 0,    0,    0,    0,              // field 4, i32
        0x2b, 0x30, 0x07, 0x2c,                 // field 5, group { field 6 varint 7 }
    };
    const std::vector<WireField> fields = must(read_message(view(data)));
    REQUIRE(fields.size() == 5);
    CHECK(fields[0].wire_type == WireType::Varint);
    CHECK(std::get<std::uint64_t>(fields[0].payload) == 42U);
    CHECK(fields[1].wire_type == WireType::I64);
    CHECK(fields[2].wire_type == WireType::Len);
    CHECK(std::get<ByteView>(fields[2].payload) == "hi");
    CHECK(fields[3].wire_type == WireType::I32);
    CHECK(fields[4].wire_type == WireType::SGroup);

    // The group body re-parses to its single inner field.
    const std::vector<WireField> inner = must(read_message(std::get<ByteView>(fields[4].payload)));
    REQUIRE(inner.size() == 1);
    CHECK(inner[0].field_number == 6);
    CHECK(std::get<std::uint64_t>(inner[0].payload) == 7U);
}

TEST_CASE("wire: duplicate field numbers are preserved in order (no merge)", "[wire]") {
    const Bytes data = {0x08, 0x01, 0x08, 0x02, 0x08, 0x03};  // field 1 = 1, 2, 3
    const std::vector<WireField> fields = must(read_message(view(data)));
    REQUIRE(fields.size() == 3);
    CHECK(std::get<std::uint64_t>(fields[0].payload) == 1U);
    CHECK(std::get<std::uint64_t>(fields[1].payload) == 2U);
    CHECK(std::get<std::uint64_t>(fields[2].payload) == 3U);
}

TEST_CASE("wire: skip advances past a field's value", "[wire]") {
    const Bytes data = {0x0a, 0x02, 'x', 'y', 0x10, 0x07};  // field1 LEN "xy", field2 varint 7
    WireReader reader(view(data));
    const Tag tag = must(reader.read_tag());
    CHECK(reader.skip(tag.wire_type, tag.field_number));
    const WireField next = must(reader.read_field());
    CHECK(next.field_number == 2);
    CHECK(std::get<std::uint64_t>(next.payload) == 7U);
}

TEST_CASE("wire: group errors", "[wire]") {
    SECTION("matched group yields the body span") {
        const Bytes data = {0x0b, 0x10, 0x2a, 0x0c};  // group field1 { field2 varint 42 } end
        WireReader reader(view(data));
        const WireField field = must(reader.read_field());
        CHECK(field.wire_type == WireType::SGroup);
        const Bytes expected_body = {0x10, 0x2a};
        CHECK(std::get<ByteView>(field.payload) == view(expected_body));
        CHECK(reader.at_end());
    }
    SECTION("end-group field number mismatch") {
        const Bytes data = {0x0b, 0x14};  // start group field1, end group field2
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_field().has_value());
        CHECK(reader.error_code() == WireError::EndGroupMismatch);
    }
    SECTION("unterminated group") {
        const Bytes data = {0x0b};  // start group, never closed
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_field().has_value());
        CHECK(reader.error_code() == WireError::UnterminatedGroup);
    }
    SECTION("stray end-group") {
        const Bytes data = {0x0c};  // end group with no open group
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_field().has_value());
        CHECK(reader.error_code() == WireError::UnexpectedEndGroup);
    }
    SECTION("nesting too deep") {
        const Bytes data(static_cast<std::size_t>(kMaxGroupDepth) + 5, 0x0b);  // many start-groups
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_field().has_value());
        CHECK(reader.error_code() == WireError::GroupTooDeep);
    }
}

TEST_CASE("wire: group bodies and nesting boundary", "[wire]") {
    SECTION("empty group body") {
        const Bytes data = {0x0b, 0x0c};  // group field1 {} end field1
        WireReader reader(view(data));
        const WireField field = must(reader.read_field());
        CHECK(field.wire_type == WireType::SGroup);
        CHECK(std::get<ByteView>(field.payload).empty());
        CHECK(reader.at_end());
    }
    SECTION("nested group body re-parses") {
        // group f1 { group f3 {} }: start f1, start f3, end f3, end f1.
        const Bytes data = {0x0b, 0x1b, 0x1c, 0x0c};
        WireReader reader(view(data));
        const WireField outer = must(reader.read_field());
        CHECK(outer.field_number == 1);
        const std::vector<WireField> body = must(read_message(std::get<ByteView>(outer.payload)));
        REQUIRE(body.size() == 1);
        CHECK(body[0].field_number == 3);
        CHECK(body[0].wire_type == WireType::SGroup);
        CHECK(std::get<ByteView>(body[0].payload).empty());
    }
    SECTION("exactly kMaxGroupDepth levels succeed") {
        const Bytes at_cap = nested_groups(kMaxGroupDepth);
        WireReader reader(view(at_cap));
        CHECK(reader.read_field().has_value());
        CHECK(reader.at_end());
        CHECK_FALSE(reader.failed());
    }
    SECTION("one level past the cap fails") {
        const Bytes past_cap = nested_groups(kMaxGroupDepth + 1);
        WireReader reader(view(past_cap));
        CHECK_FALSE(reader.read_field().has_value());
        CHECK(reader.error_code() == WireError::GroupTooDeep);
    }
}

// Error branches reachable only through read_field's value dispatch and skip()'s wire-type
// dispatch -- distinct from the primitive-reader errors exercised above.
TEST_CASE("wire: read_field and skip value-dispatch error branches", "[wire]") {
    SECTION("read_field over a LEN with a truncated length varint") {
        const Bytes data = {0x0a, 0x80};  // field1 LEN, then a length varint that never terminates
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_field().has_value());
        CHECK(reader.error_code() == WireError::TruncatedVarint);
    }
    SECTION("read_field over an I32 with too few value bytes") {
        const Bytes data = {0x0d, 0x01, 0x02};  // field1 I32, only 2 of the 4 value bytes
        WireReader reader(view(data));
        CHECK_FALSE(reader.read_field().has_value());
        CHECK(reader.error_code() == WireError::TruncatedI32);
    }
    SECTION("skip advances over an I32 value") {
        const Bytes data = {0x01, 0x02, 0x03, 0x04};  // a bare 4-byte I32 value
        WireReader reader(view(data));
        CHECK(reader.skip(WireType::I32, 1));
        CHECK(reader.at_end());
    }
    SECTION("skip over a stray end-group tag fails") {
        const Bytes data = {0x00};  // content irrelevant: an EGroup is never a valid value to skip
        WireReader reader(view(data));
        CHECK_FALSE(reader.skip(WireType::EGroup, 1));
        CHECK(reader.error_code() == WireError::UnexpectedEndGroup);
    }
    SECTION("skip into a group whose body opens with a malformed tag fails") {
        const Bytes data = {0x80};  // group body: a tag varint that never terminates
        WireReader reader(view(data));
        CHECK_FALSE(reader.skip(WireType::SGroup, 1));
        CHECK(reader.error_code() == WireError::TruncatedVarint);
    }
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
    const std::vector<WireField> fields = must(read_message(ByteView(*bin)));
    CHECK_FALSE(fields.empty());
}

TEST_CASE("wire: protoc AllWire fixture decodes with a group", "[wire]") {
    const std::optional<std::string> bin = read_fixture("all_wire.bin");
    if (!bin) {
        SUCCEED("fixture all_wire.bin not present; skipping");
        return;
    }
    const std::vector<WireField> fields = must(read_message(ByteView(*bin)));
    bool saw_group = false;
    for (const WireField& field : fields) {
        if (field.wire_type == WireType::SGroup) {
            saw_group = true;
            CHECK(read_message(std::get<ByteView>(field.payload)).has_value());
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
        WireReader reader(view(data));
        while (!reader.at_end()) {
            const std::optional<Tag> tag = reader.read_tag();
            if (!tag) {
                break;
            }
            const std::optional<std::uint64_t> val = reader.read_varint();
            if (!val) {
                break;
            }
            sum += *val;
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
