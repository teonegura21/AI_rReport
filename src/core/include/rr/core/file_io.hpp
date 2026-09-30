// rr/core/file_io.hpp — careful POSIX file operations, returning Results.
//
// Why this exists
//   The blob store and model sync both write large files that must survive a
//   crash either whole or not at all. Each step has a trap: open() and read()
//   can be interrupted (EINTR), write() can write less than asked, and a
//   rename() is durable only once the *directory* has been fsync'ed too. Doing
//   each step right once, here, beats doing it almost right twice.
//
//   Errors carry the path and the system's message, and map errno to an error
//   class: ENOENT → not_found, EACCES/EPERM → permission_denied, else io.
//
// Thread-safety: plain functions over descriptors the caller owns.
#pragma once

#include <cstddef>
#include <filesystem>
#include <source_location>
#include <span>
#include <string_view>

#include <sys/types.h>

#include <rr/core/result.hpp>
#include <rr/core/unique_fd.hpp>

namespace rr {

// The error for a failed system call on `path`: "<what> '<path>': <strerror>".
[[nodiscard]] std::unexpected<Error> os_error(std::string_view what, const std::filesystem::path& path,
                                              int error_number,
                                              std::source_location where = std::source_location::current());

// open(2) with O_CLOEXEC, retried when interrupted.
[[nodiscard]] Result<UniqueFd> open_file(const std::filesystem::path& path, int flags, mode_t mode = 0);

// Writes every byte, continuing after short writes and interruptions.
[[nodiscard]] Result<void> write_all(int fd, std::span<const std::byte> bytes,
                                     const std::filesystem::path& path);
[[nodiscard]] Result<void> write_all(int fd, std::string_view bytes, const std::filesystem::path& path);

// Reads up to buffer.size() bytes; 0 means the end of the file.
[[nodiscard]] Result<std::size_t> read_some(int fd, std::span<std::byte> buffer,
                                            const std::filesystem::path& path);

// fsync(2): the file's data and metadata reach the disk.
[[nodiscard]] Result<void> sync_file(int fd, const std::filesystem::path& path);

// Makes a completed rename (or a new file) durable: the new directory entry
// only reaches the disk once the directory itself has been synced.
[[nodiscard]] Result<void> sync_directory(const std::filesystem::path& directory);

}  // namespace rr
