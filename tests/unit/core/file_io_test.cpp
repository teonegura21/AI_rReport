#include <rr/core/file_io.hpp>

#include <array>
#include <cerrno>
#include <filesystem>
#include <span>
#include <string>

#include <fcntl.h>

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

}  // namespace
}  // namespace rr
