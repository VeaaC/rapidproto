// Golden tests for the wire reader: decode each checked-in protoc .bin fixture, serialize
// its structural wire dump, and assert it matches a checked-in expected dump byte-for-byte.
// Regenerate (after an intentional reader/dump change) with `tests/regen_goldens.sh` (all goldens),
// or just these with `RAPIDPROTO_REGEN_GOLDEN=1 ./build/gcc/rapidproto_tests "[wire-golden]"`.
// Fixtures (.bin) are produced by tests/wire_fixtures/generate.py (needs protoc) and are
// checked in; a missing fixture skips its scenario rather than failing.

#include <catch_amalgamated.hpp>

#include "golden_file.hpp"

#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "rapidproto/runtime.hpp"
#include "wire_dump.hpp"

using namespace rapidproto;  // NOLINT(google-build-using-namespace): test convenience

namespace {

using rapidproto::test::read_file;

bool file_exists(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    return file.good();
}

}  // namespace

TEST_CASE("wire-golden: fixture dumps match expectations", "[wire-golden]") {
    const std::vector<std::string> scenarios = {"scalars", "msg", "all_wire"};

    for (const std::string& name : scenarios) {
        const std::string bin = std::string(RAPIDPROTO_WIRE_FIXTURE_DIR) + "/" + name + ".bin";
        const std::string golden = std::string(RAPIDPROTO_WIRE_GOLDEN_DIR) + "/" + name + ".txt";
        // The fixtures are CHECKED IN: a missing one is a broken checkout or a bad move, and
        // must fail loudly -- a skip here read as green when a fixture vanished.
        INFO("fixture: " << bin);
        REQUIRE(file_exists(bin));

        const std::string bytes = read_file(bin);
        test::check_golden(golden, name, wiredump::dump_wire(ByteView(bytes)));
    }
}
