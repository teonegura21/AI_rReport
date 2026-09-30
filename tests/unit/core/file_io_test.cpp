#include <rr/core/file_io.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

TEST(FileIo, WritesEveryByteAndReadsThemBack) {
    const testkit::TempDir dir;
    const std::filesystem::path file = dir / "data.bin";
    std::string bytes(std::size_t{3} << 20U, '\0');  // several write() calls' worth
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(index * 31U);
    }
    {
        const UniqueFd out = unwrap(open_file(file, O_WRONLY | O_CREAT | O_TRUNC, 0600));
        ASSERT_OK(write_all(out.get(), bytes, file));
        ASSERT_OK(sync_file(out.get(), file));
    }
    ASSERT_OK(sync_directory(dir.path()));

    const UniqueFd in = unwrap(open_file(file, O_RDONLY));
    std::string read_back;
    std::array<std::byte, 65536> buffer{};
    while (const std::size_t count = unwrap(read_some(in.get(), buffer, file))) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — bytes to chars
        read_back.append(reinterpret_cast<const char*>(buffer.data()), count);
    }
    EXPECT_EQ(read_back, bytes);
}

TEST(FileIo, ErrorsNameThePathAndTheirClass) {
    const testkit::TempDir dir;
    const auto missing = open_file(dir / "absent", O_RDONLY);
    ASSERT_ERRC(missing, Errc::not_found);
    EXPECT_THAT(missing.error().message(), HasSubstr("absent"));
    EXPECT_ERRC(sync_directory(dir / "absent"), Errc::not_found);

    EXPECT_EQ(os_error("cannot open", "/x", EACCES).error().code(), Errc::permission_denied);
    EXPECT_EQ(os_error("cannot open", "/x", EPERM).error().code(), Errc::permission_denied);
    EXPECT_EQ(os_error("cannot write", "/x", ENOSPC).error().code(), Errc::io);
    EXPECT_THAT(os_error("cannot write", "/x", ENOSPC).error().message(), HasSubstr("cannot write '/x'"));
}

TEST(FileIo, ReportsWritesToABadDescriptor) {
    const std::string bytes = "x";
    EXPECT_ERRC(write_all(-1, bytes, "/nowhere"), Errc::io);
    std::array<std::byte, 4> buffer{};
    EXPECT_ERRC(read_some(-1, buffer, "/nowhere"), Errc::io);
    EXPECT_ERRC(sync_file(-1, "/nowhere"), Errc::io);
}

// The names in `dir`, hidden ones included.
std::vector<std::string> names_in(const std::filesystem::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

TEST(FileIo, ReadsWholeFilesWithinALimit) {
    const testkit::TempDir dir;
    const std::string bytes(100'000, 'x');  // more than one read() buffer
    dir.write_file("data.txt", bytes);
    EXPECT_EQ(unwrap(read_file(dir / "data.txt")), bytes);
    EXPECT_EQ(unwrap(read_file(dir / "data.txt", bytes.size())), bytes) << "exactly the limit is fine";
    const auto too_big = read_file(dir / "data.txt", bytes.size() - 1);
    ASSERT_ERRC(too_big, Errc::invalid_argument);
    EXPECT_THAT(too_big.error().message(), HasSubstr("larger than 99999 bytes"));
    EXPECT_ERRC(read_file(dir / "absent"), Errc::not_found);
}

TEST(FileIo, ReplacesFilesAtomically) {
    const testkit::TempDir dir;
    const std::filesystem::path file = dir / "report.json";
    ASSERT_OK(write_file_atomically(file, "first", 0600));
    EXPECT_EQ(unwrap(read_file(file)), "first");
    struct stat status{};
    ASSERT_EQ(::stat(file.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777U, 0600U);

    ASSERT_OK(write_file_atomically(file, "second, and longer"));
    EXPECT_EQ(unwrap(read_file(file)), "second, and longer");
    EXPECT_EQ(names_in(dir.path()), std::vector<std::string>{"report.json"}) << "no temporary file is left";
}

TEST(FileIo, AFailedReplaceLeavesNothingBehind) {
    const testkit::TempDir dir;
    // rename() cannot put a file where a non-empty directory is.
    dir.write_file("taken/inside", "x");
    const auto replaced = write_file_atomically(dir / "taken", "content");
    ASSERT_FALSE(replaced.has_value());
    EXPECT_THAT(replaced.error().message(), HasSubstr("cannot replace"));
    EXPECT_EQ(names_in(dir.path()), std::vector<std::string>{"taken"}) << "the temporary file is removed";
    EXPECT_TRUE(std::filesystem::is_directory(dir / "taken")) << "and what was there is untouched";

    EXPECT_ERRC(write_file_atomically(dir / "absent" / "file", "content"), Errc::not_found);
}

}  // namespace
}  // namespace rr
