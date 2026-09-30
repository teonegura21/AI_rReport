#include <rr/core/file_io.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <format>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <rr/core/random.hpp>

namespace rr {

std::unexpected<Error> os_error(std::string_view what, const std::filesystem::path& path, int error_number,
                                std::source_location where) {
    Errc code = Errc::io;
    if (error_number == ENOENT) {
        code = Errc::not_found;
    } else if (error_number == EACCES || error_number == EPERM) {
        code = Errc::permission_denied;
    }
    return fail(code,
                std::format("{} '{}': {}", what, path.string(), std::system_category().message(error_number)),
                where);
}

Result<UniqueFd> open_file(const std::filesystem::path& path, int flags, mode_t mode) {
    int fd = -1;
    do {
        fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return os_error("cannot open", path, errno);
    }
    return UniqueFd{fd};
}

Result<void> write_all(int fd, std::span<const std::byte> bytes, const std::filesystem::path& path) {
    while (!bytes.empty()) {
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return os_error("cannot write", path, errno);
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return ok();
}

Result<void> write_all(int fd, std::string_view bytes, const std::filesystem::path& path) {
    return write_all(fd, std::as_bytes(std::span{bytes.data(), bytes.size()}), path);
}

Result<std::size_t> read_some(int fd, std::span<std::byte> buffer, const std::filesystem::path& path) {
    while (true) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        if (errno != EINTR) {
            return os_error("cannot read", path, errno);
        }
    }
}

Result<void> sync_file(int fd, const std::filesystem::path& path) {
    if (::fsync(fd) != 0) {
        return os_error("cannot fsync", path, errno);
    }
    return ok();
}

Result<void> sync_directory(const std::filesystem::path& directory) {
    RR_TRY_ASSIGN(UniqueFd fd, open_file(directory, O_RDONLY | O_DIRECTORY));
    RR_TRY(sync_file(fd.get(), directory));
    if (const int error_number = fd.close(); error_number != 0) {
        return os_error("cannot close", directory, error_number);
    }
    return ok();
}

Result<std::string> read_file(const std::filesystem::path& path, std::size_t max_bytes) {
    RR_TRY_ASSIGN(const UniqueFd in, open_file(path, O_RDONLY));
    std::string content;
    std::array<std::byte, 65536> buffer{};
    while (true) {
        RR_TRY_ASSIGN(const std::size_t count, read_some(in.get(), buffer, path));
        if (count == 0) {
            return content;
        }
        if (count > max_bytes - content.size()) {
            return fail(Errc::invalid_argument,
                        std::format("'{}' is larger than {} bytes", path.string(), max_bytes));
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — bytes to chars
        content.append(reinterpret_cast<const char*>(buffer.data()), count);
    }
}

Result<void> write_file_atomically(const std::filesystem::path& path, std::string_view content, mode_t mode) {
    const std::filesystem::path directory = path.has_parent_path() ? path.parent_path() : ".";
    RR_TRY_ASSIGN(const std::string suffix, random_hex(8));
    // Hidden, so nothing that lists the directory mistakes it for a finished file.
    const std::filesystem::path temporary =
        directory / std::format(".{}.{}.tmp", path.filename().string(), suffix);
    const auto discard = [&temporary] {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    };

    RR_TRY_ASSIGN(UniqueFd out, open_file(temporary, O_WRONLY | O_CREAT | O_EXCL, mode));
    auto written = [&]() -> Result<void> {
        RR_TRY(write_all(out.get(), content, temporary));
        RR_TRY(sync_file(out.get(), temporary));
        if (const int error_number = out.close(); error_number != 0) {
            return os_error("cannot close", temporary, error_number);
        }
        return ok();
    }();
    if (!written.has_value()) {
        discard();
        return written;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        const int error_number = errno;
        discard();
        return os_error("cannot replace", path, error_number);
    }
    return sync_directory(directory);
}

}  // namespace rr
