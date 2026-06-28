#pragma once

// Test-support: a unique scratch directory that writes fixtures under a per-(PID, tag, counter) path
// and removes the tree on destruction. The PID in the path matters because check.sh runs the gcc and
// clang test binaries in parallel -- a PID-less path would let one binary's remove_all wipe the
// other's files mid-test. Shared by the tests that drive the on-disk resolve()/pipeline entry points.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace rapidproto::test {

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static int counter = 0;
        m_dir = std::filesystem::temp_directory_path() /
                ("rapidproto_" + tag + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter++));
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
        std::filesystem::create_directories(m_dir);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): rel vs content, distinct roles
    void write(const std::string& rel, const std::string& content) const {
        const std::filesystem::path full = m_dir / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream(full) << content;
    }
    [[nodiscard]] std::string path(const std::string& rel) const { return (m_dir / rel).string(); }
    [[nodiscard]] std::string root() const { return m_dir.string(); }

private:
    std::filesystem::path m_dir;
};

}  // namespace rapidproto::test
