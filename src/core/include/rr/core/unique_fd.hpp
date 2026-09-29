// rr/core/unique_fd.hpp — an owned POSIX file descriptor.
//
// Why this exists
//   Several modules work with raw descriptors (the blob store's fsync'ed
//   writes, subprocess pipes). A descriptor that leaks on an error path is a
//   resource leak and, worse, an open pipe end that keeps a reader waiting
//   for an EOF that never comes. UniqueFd closes exactly once, on every path.
//
// Thread-safety: none; like std::unique_ptr, one owner at a time.
#pragma once

#include <cerrno>
#include <utility>

#include <unistd.h>

namespace rr {

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    // Closes now and reports the outcome: 0, or the errno value. close() can
    // surface a deferred write error (notably on network filesystems), so a
    // writer that cares about durability must check it.
    [[nodiscard]] int close() noexcept {
        if (fd_ < 0) {
            return 0;
        }
        return ::close(std::exchange(fd_, -1)) == 0 ? 0 : errno;
    }

    // Closes without reporting (for error paths and destruction).
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(std::exchange(fd_, -1));
        }
    }

private:
    int fd_ = -1;
};

}  // namespace rr
