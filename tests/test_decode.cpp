// The decode runtime contract, exercised directly: this hand-writes a decoder the way the generator
// emits one (field-identity tags + a compile-time-filtered dispatch switch over
// rapidproto::combine/handles/invoke_field), so the runtime is tested independently of the
// generator. Covers every part of the contract: (tag, value) dispatch, subset skipping,
// unknown-field skipping, void vs DecodeStatus callbacks, abort propagation, and wire-error
// propagation.

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rapidproto/runtime.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

using Bytes = std::vector<std::uint8_t>;

ByteView view(const Bytes& bytes) {
    return byte_view(bytes.data(), bytes.size());
}
ByteView view(Bytes&&) = delete;  // forbid views into a temporary (would dangle)

// A hand-written stand-in for generated code: message Demo { int32 id=1; string name=2;
//   repeated int32 nums=3; }  decoded over a borrowed byte view.
struct Demo {
    explicit Demo(ByteView bytes) noexcept : m_bytes(bytes) {}

    struct id {
        using Value = std::int32_t;
        static constexpr std::uint32_t kNumber = 1;
        static constexpr std::string_view kName = "id";
    };
    struct name {
        using Value = std::string_view;
        static constexpr std::uint32_t kNumber = 2;
        static constexpr std::string_view kName = "name";
    };
    struct nums {  // repeated -> fires per element
        using Value = std::int32_t;
        static constexpr std::uint32_t kNumber = 3;
        static constexpr std::string_view kName = "nums";
    };

    template <class... Cbs>
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): mirrors generated dispatch
    [[nodiscard]] DecodeStatus decode(Cbs&&... cbs) const {
        auto dispatch = combine(std::forward<Cbs>(cbs)...);
        WireReader reader{m_bytes};
        while (!reader.at_end()) {
            const auto tag = reader.read_tag();
            if (!tag) {
                return DecodeStatus::from_reader(reader);
            }
            switch (tag->field_number) {
                case id::kNumber:
                    if constexpr ((false || ... || handles_one<Cbs, id, id::Value>)) {
                        const auto raw = reader.read_varint();
                        if (!raw) {
                            return DecodeStatus::from_reader(reader);
                        }
                        if (const auto s = invoke_field(dispatch, id{}, varint_to_int32(*raw));
                            !s.ok()) {
                            return s;
                        }
                    } else if (!reader.skip(tag->wire_type, tag->field_number)) {
                        return DecodeStatus::from_reader(reader);
                    }
                    break;
                case name::kNumber:
                    if constexpr ((false || ... || handles_one<Cbs, name, name::Value>)) {
                        const auto payload = reader.read_length_delimited();
                        if (!payload) {
                            return DecodeStatus::from_reader(reader);
                        }
                        if (const auto s = invoke_field(dispatch, name{}, *payload); !s.ok()) {
                            return s;
                        }
                    } else if (!reader.skip(tag->wire_type, tag->field_number)) {
                        return DecodeStatus::from_reader(reader);
                    }
                    break;
                case nums::kNumber:
                    if constexpr ((false || ... || handles_one<Cbs, nums, nums::Value>)) {
                        const auto raw = reader.read_varint();
                        if (!raw) {
                            return DecodeStatus::from_reader(reader);
                        }
                        if (const auto s = invoke_field(dispatch, nums{}, varint_to_int32(*raw));
                            !s.ok()) {
                            return s;
                        }
                    } else if (!reader.skip(tag->wire_type, tag->field_number)) {
                        return DecodeStatus::from_reader(reader);
                    }
                    break;
                default:
                    if (!reader.skip(tag->wire_type, tag->field_number)) {
                        return DecodeStatus::from_reader(reader);
                    }
                    break;
            }
        }
        return DecodeStatus::success();
    }

private:
    ByteView m_bytes;
};

// Demo { id=42, name="hi", nums=[7, 8] } plus an unknown field 5 (varint 99).
const Bytes kDemo = {
    0x08, 0x2a,            // field 1, varint 42
    0x12, 0x02, 'h', 'i',  // field 2, len "hi"
    0x18, 0x07,            // field 3, varint 7
    0x18, 0x08,            // field 3, varint 8
    0x28, 0x63,            // field 5, varint 99 (unknown)
};

}  // namespace

TEST_CASE("decode: dispatch delivers every handled field, skips the rest", "[decode]") {
    std::int32_t id = 0;
    std::string name;
    std::vector<std::int32_t> nums;
    const Demo demo{view(kDemo)};
    const DecodeStatus status =
        demo.decode([&](Demo::id, std::int32_t v) { id = v; },
                    [&](Demo::name, std::string_view v) { name = std::string(v); },
                    [&](Demo::nums, std::int32_t v) { nums.push_back(v); });

    CHECK(status.ok());
    CHECK(id == 42);
    CHECK(name == "hi");
    CHECK(nums == std::vector<std::int32_t>{7, 8});  // repeated fires per element, in wire order
}

TEST_CASE("decode: a subset callback ignores (skips) the other fields", "[decode]") {
    std::int32_t id = 0;
    const Demo demo{view(kDemo)};
    // name/nums/unknown all skipped; no callback fires for them, decoding still succeeds.
    const DecodeStatus status = demo.decode([&](Demo::id, std::int32_t v) { id = v; });
    CHECK(status.ok());
    CHECK(id == 42);
}

TEST_CASE("decode: field tags expose static metadata", "[decode]") {
    CHECK(Demo::id::kNumber == 1);
    CHECK(Demo::id::kName == "id");
    CHECK(Demo::name::kName == "name");
    CHECK(Demo::nums::kNumber == 3);
    CHECK(Demo::nums::kName == "nums");
}

TEST_CASE("decode: a void callback is accepted alongside a status one", "[decode]") {
    std::int32_t id = 0;
    std::vector<std::int32_t> nums;
    const Demo demo{view(kDemo)};
    const DecodeStatus status = demo.decode([&](Demo::id, std::int32_t v) { id = v; },  // void
                                            [&](Demo::nums, std::int32_t v) -> DecodeStatus {
                                                nums.push_back(v);
                                                return {};
                                            });  // status
    CHECK(status.ok());
    CHECK(id == 42);
    CHECK(nums == std::vector<std::int32_t>{7, 8});
}

TEST_CASE("decode: a callback abort stops decoding and propagates", "[decode]") {
    std::int32_t id = 0;
    std::vector<std::int32_t> nums;
    const Demo demo{view(kDemo)};
    // name (field 2) comes before nums (field 3); aborting there must stop before any nums fire.
    const DecodeStatus status = demo.decode(
        [&](Demo::id, std::int32_t v) { id = v; },
        [&](Demo::name, std::string_view) -> DecodeStatus { return DecodeStatus::abort(); },
        [&](Demo::nums, std::int32_t v) { nums.push_back(v); });

    CHECK_FALSE(status.ok());
    CHECK(status.aborted);
    CHECK(id == 42);      // id (field 1) was delivered before the abort
    CHECK(nums.empty());  // decoding stopped at field 2
}

TEST_CASE("decode: a wire error propagates as a DecodeStatus", "[decode]") {
    const Bytes truncated = {0x08, 0x80};  // field 1 varint with a dangling continuation byte
    const Demo demo{view(truncated)};
    const DecodeStatus status = demo.decode([](Demo::id, std::int32_t) {});
    CHECK_FALSE(status.ok());
    CHECK_FALSE(status.aborted);
    CHECK(status.wire == WireError::TruncatedVarint);
}

TEST_CASE("decode: a wire error mid-stream stops after earlier fields are delivered", "[decode]") {
    // id=42 (valid), then field 2 as LEN claiming 5 bytes but only 2 present.
    const Bytes data = {0x08, 0x2a, 0x12, 0x05, 'a', 'b'};
    std::int32_t id = 0;
    const Demo demo{view(data)};
    const DecodeStatus status = demo.decode([&](Demo::id, std::int32_t v) { id = v; });
    CHECK(id == 42);  // delivered before the error was hit
    CHECK_FALSE(status.ok());
    CHECK(status.wire == WireError::LengthExceedsBuffer);
}

TEST_CASE("decode: read with no callbacks traverses and validates, delivering nothing",
          "[decode]") {
    const Demo demo{view(kDemo)};
    CHECK(demo.decode().ok());  // every field skipped; still walks + validates the whole message
}
