// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The one home for the golden-file protocol every golden suite shares: read the pinned bytes,
// compare with a line-level diagnostic, and honor RAPIDPROTO_REGEN_GOLDEN. Before this header the
// suites carried nine copies of read_file, five of first_difference, and six open-codings of the
// regen-or-compare dance -- and the copies had drifted (two suites compared multi-hundred-line
// headers with no diagnostic at all).

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "catch_amalgamated.hpp"

namespace rapidproto::test {

inline std::string read_file(const std::string& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// The first differing line of two blobs, for a byte-compare's INFO: a failed golden CHECK on a
// large header is unreadable without it.
inline std::string first_difference(const std::string& expected, const std::string& actual) {
    std::istringstream exp(expected);
    std::istringstream act(actual);
    std::string exp_line;
    std::string act_line;
    int number = 1;
    while (true) {
        const bool exp_ok = static_cast<bool>(std::getline(exp, exp_line));
        const bool act_ok = static_cast<bool>(std::getline(act, act_line));
        if (!exp_ok && !act_ok) {
            return "(no line difference; trailing-newline mismatch?)";
        }
        if (exp_ok != act_ok || exp_line != act_line) {
            return "first diff at line " + std::to_string(number) +
                   ":\n  expected: " + (exp_ok ? exp_line : "<eof>") +
                   "\n  actual:   " + (act_ok ? act_line : "<eof>");
        }
        ++number;
    }
}

// Opt-in regeneration: deliberately re-read per call, not cached -- a check run must never turn
// into a rewrite because the variable leaked in from an earlier regen in the same environment
// (check.sh's deep tier red-tested exactly that inheritance hazard).
inline bool regen_goldens() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test binary, opt-in regeneration only
    return std::getenv("RAPIDPROTO_REGEN_GOLDEN") != nullptr;
}

// The regen-or-compare protocol: overwrite the golden under RAPIDPROTO_REGEN_GOLDEN (warning so a
// regen run is visibly not a verification), otherwise byte-compare with the line diagnostic.
// `label` names the case in both paths.
inline void check_golden(const std::string& golden_path, const std::string& label,
                         const std::string& actual) {
    if (regen_goldens()) {
        std::ofstream(golden_path, std::ios::binary) << actual;
        WARN("regenerated golden: " << label);
        return;
    }
    const std::string expected = read_file(golden_path);
    INFO("golden: " << label);
    INFO(first_difference(expected, actual));
    CHECK(actual == expected);
}

}  // namespace rapidproto::test
