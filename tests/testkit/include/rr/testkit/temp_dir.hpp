// rr/testkit/temp_dir.hpp — a private scratch directory for one test.
//
// Why this exists
//   Tests that touch the filesystem (config files, SQLite databases, the blob
//   store, subprocess working directories) each need a directory that
//     (a) cannot collide with another test running in parallel (`ctest -j`),
//     (b) is not readable by other users, and
//     (c) disappears even when the test fails.
//   mkdtemp(3) gives (a) and (b) atomically: it creates a directory with a
//   unique name and mode 0700 in one system call, so there is no window in
//   which two processes can pick the same name. The destructor gives (c).
//
//   Test-only code, so failures throw: GoogleTest reports an escaped
//   exception as a failed test with its message, which is what we want here.
//
// Thread-safety: none needed; a TempDir belongs to one test.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace rr::testkit {

class TempDir {
public:
    // Creates "<system temp dir>/<prefix>_XXXXXX" with a unique suffix.
    explicit TempDir(std::string_view prefix = "rr_test") {
        std::string pattern = (std::filesystem::temp_directory_path() / prefix).string() + "_XXXXXX";
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp(" + pattern + ")");
        }
        path_ = pattern;
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    ~TempDir() {
        std::error_code ignored;  // destructors must not throw; a leftover temp dir is harmless
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // A path inside the directory: `dir / "library.db"`.
    [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& relative) const {
        return path_ / relative;
    }

    // Writes `content` byte-for-byte to `relative`, creating parent
    // directories as needed. Returns the full path.
    std::filesystem::path write_file(const std::filesystem::path& relative, std::string_view content) const {
        const std::filesystem::path file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            throw std::runtime_error("cannot write " + file.string());
        }
        return file;
    }

    // Reads a whole file (absolute, or relative to the directory).
    [[nodiscard]] std::string read_file(const std::filesystem::path& file) const {
        std::ifstream in(file.is_absolute() ? file : path_ / file, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot read " + file.string());
        }
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

private:
    std::filesystem::path path_;
};

}  // namespace rr::testkit
