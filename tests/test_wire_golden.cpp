// Golden tests for the wire reader: decode each checked-in protoc .bin fixture, serialize
// its structural wire dump, and assert it matches a checked-in expected dump byte-for-byte.
// Regenerate (after an intentional reader/dump change) with `tests/regen_goldens.sh` (all goldens),
// or just these with `RAPIDPROTO_REGEN_GOLDEN=1 ./build/gcc/rapidproto_tests "[wire-golden]"`.
// Fixtures (.bin) are produced by tests/wire_fixtures/generate.py (needs protoc) and are
// checked in; a missing fixture skips its scenario rather than failing.

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include "rapidproto/runtime.hpp"
#include "wire_dump.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool file_exists(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    return file.good();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): expected vs actual, distinct roles
std::string first_difference(const std::string& expected, const std::string& actual) {
    std::istringstream exp(expected);
    std::istringstream act(actual);
    std::string exp_line;
    std::string act_line;
    int number = 1;
    while (true) {
        const bool exp_ok = static_cast<bool>(std::getline(exp, exp_line));
        const bool act_ok = static_cast<bool>(std::getline(act, act_line));
        if (!exp_ok && !act_ok) {
            return "(no line difference)";
        }
        if (exp_ok != act_ok || exp_line != act_line) {
            return "first diff at line " + std::to_string(number) +
                   ":\n  expected: " + (exp_ok ? exp_line : "<eof>") +
                   "\n  actual:   " + (act_ok ? act_line : "<eof>");
        }
        ++number;
    }
}

}  // namespace

TEST_CASE("wire-golden: fixture dumps match expectations", "[wire-golden]") {
    const std::vector<std::string> scenarios = {"scalars", "msg", "all_wire"};

    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, opt-in regeneration only
    const bool regen = std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr;

    for (const std::string& name : scenarios) {
        const std::string bin = std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/" + name + ".bin";
        const std::string golden = std::string(RAPIDPROTO_WIRE_GOLDEN_DIR) + "/" + name + ".txt";
        if (!file_exists(bin)) {
            SUCCEED("fixture " + name + ".bin not present; skipping");
            continue;
        }

        const std::string bytes = read_file(bin);
        const std::string actual = wiredump::dump_wire(ByteView(bytes));

        if (regen) {
            std::ofstream(golden, std::ios::binary) << actual;
            WARN("regenerated wire golden: " << name);
            continue;
        }

        const std::string expected = read_file(golden);
        INFO("scenario: " << name);
        INFO(first_difference(expected, actual));
        CHECK(actual == expected);
    }
}
