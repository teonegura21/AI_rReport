#include <rr/core/file_io.hpp>

#include <cerrno>
#include <format>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

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

}  // namespace rr
