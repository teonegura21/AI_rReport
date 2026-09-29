#include <rr/blob/blob_store.hpp>

#include <algorithm>
#include <cerrno>
#include <format>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rr/core/scope_exit.hpp>
#include <rr/core/ulid.hpp>
#include <rr/core/unique_fd.hpp>

namespace rr::blob {

namespace {

constexpr std::string_view kTempDirectory = ".tmp";
constexpr std::size_t kCopyChunk = std::size_t{1} << 20U;  // 1 MiB: large enough to amortize syscalls
constexpr mode_t kFileMode = 0600;
constexpr mode_t kDirectoryMode = 0700;

// Maps an errno value to an error class and a readable message.
std::unexpected<Error> os_error(std::string_view what, const std::filesystem::path& path, int error_number,
                                std::source_location where = std::source_location::current()) {
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

std::unexpected<Error> fs_error(std::string_view what, const std::filesystem::path& path,
                                const std::error_code& ec,
                                std::source_location where = std::source_location::current()) {
    // On POSIX, std::filesystem reports errno values in the generic category.
    return os_error(what, path, ec.value(), where);
}

Result<UniqueFd> open_file(const std::filesystem::path& path, int flags, mode_t mode = 0) {
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

// Reads up to buffer.size() bytes; 0 means end of file.
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

// Makes a completed rename durable: the new directory entry is only on disk
// once the directory itself has been synced.
Result<void> sync_directory(const std::filesystem::path& directory) {
    RR_TRY_ASSIGN(UniqueFd fd, open_file(directory, O_RDONLY | O_DIRECTORY));
    RR_TRY(sync_file(fd.get(), directory));
    if (const int error_number = fd.close(); error_number != 0) {
        return os_error("cannot close", directory, error_number);
    }
    return ok();
}

// Creates one directory with private permissions; an existing one is fine.
Result<void> make_directory(const std::filesystem::path& directory) {
    if (::mkdir(directory.c_str(), kDirectoryMode) != 0 && errno != EEXIST) {
        return os_error("cannot create directory", directory, errno);
    }
    return ok();
}

std::filesystem::path blob_path(const std::filesystem::path& root, const Blake3Digest& hash) {
    const std::string hex = hash.hex();
    return root / hex.substr(0, 2) / hex;
}

// Removes temp files whose last write is older than kStaleTempAge. Files
// still being written are safe: every write() refreshes their mtime.
void sweep_stale_temp_files(const std::filesystem::path& temp_directory) {
    const auto cutoff = std::filesystem::file_time_type::clock::now() - BlobStore::kStaleTempAge;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{temp_directory, ec}, end; !ec && it != end;
         it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && it->last_write_time(entry_ec) < cutoff && !entry_ec) {
            std::filesystem::remove(it->path(), entry_ec);  // best effort: another process may race us
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// BlobWriter
// ---------------------------------------------------------------------------
// Invariant: once a writer is spent (it failed, or commit() returned), it
// owns no file. Rejected or redundant bytes are deleted at once, not when the
// writer object happens to be destroyed.
struct BlobWriter::State {
    std::filesystem::path root;
    std::filesystem::path temp_path;  // empty once published or discarded
    UniqueFd fd;
    Blake3Hasher hasher;
    std::uint64_t size = 0;
    std::uint64_t max_size = kNoSizeLimit;
    bool spent = false;  // failed or committed: no further calls

    // Ends the writer without publishing: closes and deletes the temp file.
    void discard() noexcept {
        spent = true;
        fd = UniqueFd{};
        if (!temp_path.empty()) {
            std::error_code ignored;  // best effort; open() sweeps anything left over
            std::filesystem::remove(temp_path, ignored);
            temp_path.clear();
        }
    }
};

BlobWriter::BlobWriter(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
BlobWriter::BlobWriter(BlobWriter&&) noexcept = default;
BlobWriter& BlobWriter::operator=(BlobWriter&&) noexcept = default;

BlobWriter::~BlobWriter() {
    if (state_) {
        state_->discard();  // a no-op after a successful commit()
    }
}

Result<void> BlobWriter::write(std::span<const std::byte> bytes) {
    State& state = *state_;
    if (state.spent) {
        return fail(Errc::internal, "write() on a blob writer that failed or was committed");
    }
    if (bytes.size() > state.max_size - state.size) {
        state.discard();
        return fail(Errc::invalid_argument,
                    std::format("blob exceeds the size limit of {} bytes", state.max_size));
    }
    if (auto written = write_all(state.fd.get(), bytes, state.temp_path); !written) {
        state.discard();
        return written;
    }
    state.hasher.update(bytes);
    state.size += bytes.size();
    return ok();
}

Result<void> BlobWriter::write(std::string_view text) {
    return write(std::as_bytes(std::span{text.data(), text.size()}));
}

Result<BlobRef> BlobWriter::commit(const std::optional<Blake3Digest>& expected) {
    State& state = *state_;
    if (state.spent) {
        return fail(Errc::internal, "commit() on a blob writer that failed or was committed");
    }
    // Whatever happens below, this writer is finished; any return path that
    // does not publish leaves the temp file to discard().
    const ScopeExit finish{[&state] { state.discard(); }};

    RR_TRY(sync_file(state.fd.get(), state.temp_path));
    if (const int error_number = state.fd.close(); error_number != 0) {
        return os_error("cannot close", state.temp_path, error_number);
    }
    const BlobRef blob{.hash = state.hasher.finalize(), .size = state.size};
    if (expected.has_value() && *expected != blob.hash) {
        return fail(Errc::integrity, std::format("content hash mismatch: expected {}, received {} ({} bytes)",
                                                 *expected, blob.hash, blob.size));
    }

    const std::filesystem::path final_path = blob_path(state.root, blob.hash);
    RR_TRY(make_directory(final_path.parent_path()));

    // Identical content already stored: nothing to publish, and the redundant
    // temp file is discarded. (A same-named file of another size is damaged;
    // the rename below replaces it.)
    std::error_code ec;
    if (const auto existing_size = std::filesystem::file_size(final_path, ec);
        !ec && existing_size == blob.size) {
        return blob;
    }
    std::filesystem::rename(state.temp_path, final_path, ec);
    if (ec) {
        return fs_error("cannot publish blob", final_path, ec);
    }
    state.temp_path.clear();  // published: the file now belongs to the store
    RR_TRY(sync_directory(final_path.parent_path()));
    return blob;
}

std::uint64_t BlobWriter::size() const noexcept {
    return state_->size;
}

// ---------------------------------------------------------------------------
// BlobStore
// ---------------------------------------------------------------------------
Result<BlobStore> BlobStore::open(const std::filesystem::path& root) {
    std::error_code ec;
    const std::filesystem::path absolute_root = std::filesystem::absolute(root, ec);
    if (ec) {
        return fs_error("cannot resolve", root, ec);
    }
    // The root may be a user-chosen directory: create it if needed, but leave
    // the permissions of an existing one alone.
    std::filesystem::create_directories(absolute_root, ec);
    if (ec) {
        return fs_error("cannot create blob store", absolute_root, ec);
    }
    const std::filesystem::path temp_directory = absolute_root / kTempDirectory;
    RR_TRY(make_directory(temp_directory));
    sweep_stale_temp_files(temp_directory);
    return BlobStore{absolute_root};
}

Result<BlobWriter> BlobStore::begin_write(std::uint64_t max_size) const {
    auto state = std::make_unique<BlobWriter::State>();
    state->root = root_;
    state->max_size = max_size;
    state->temp_path = root_ / kTempDirectory / new_ulid();
    // O_EXCL: a temp name is never shared, not even by accident.
    RR_TRY_ASSIGN(state->fd, open_file(state->temp_path, O_WRONLY | O_CREAT | O_EXCL, kFileMode));
    return BlobWriter{std::move(state)};
}

Result<BlobRef> BlobStore::put_bytes(std::span<const std::byte> bytes) const {
    RR_TRY_ASSIGN(BlobWriter writer, begin_write());
    RR_TRY(writer.write(bytes));
    return writer.commit();
}

Result<BlobRef> BlobStore::put_text(std::string_view text) const {
    return put_bytes(std::as_bytes(std::span{text.data(), text.size()}));
}

Result<BlobRef> BlobStore::put_file(const std::filesystem::path& source, const std::stop_token& stop) const {
    RR_TRY_ASSIGN(const UniqueFd input, open_file(source, O_RDONLY));
    RR_TRY_ASSIGN(BlobWriter writer, begin_write());
    std::vector<std::byte> buffer(kCopyChunk);
    while (true) {
        if (stop.stop_requested()) {
            return fail(Errc::cancelled, std::format("copy of '{}' cancelled", source.string()));
        }
        RR_TRY_ASSIGN(const std::size_t count, read_some(input.get(), buffer, source));
        if (count == 0) {
            break;
        }
        RR_TRY(writer.write(std::span{buffer}.first(count)));
    }
    return writer.commit();
}

bool BlobStore::contains(const Blake3Digest& hash) const {
    std::error_code ec;
    return std::filesystem::is_regular_file(path_of(hash), ec);
}

std::filesystem::path BlobStore::path_of(const Blake3Digest& hash) const {
    return blob_path(root_, hash);
}

Result<BlobRef> BlobStore::stat(const Blake3Digest& hash) const {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_of(hash), ec);
    if (ec) {
        return fs_error("no blob", path_of(hash), ec);
    }
    return BlobRef{.hash = hash, .size = size};
}

Result<std::vector<std::byte>> BlobStore::read_bytes(const Blake3Digest& hash, std::uint64_t max_size) const {
    const std::filesystem::path path = path_of(hash);
    RR_TRY_ASSIGN(const UniqueFd input, open_file(path, O_RDONLY));
    struct stat info{};
    if (::fstat(input.get(), &info) != 0) {
        return os_error("cannot stat", path, errno);
    }
    const auto size = static_cast<std::uint64_t>(info.st_size);
    if (size > max_size) {
        return fail(Errc::invalid_argument,
                    std::format("blob {} is {} bytes, above the {}-byte read limit", hash, size, max_size));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        RR_TRY_ASSIGN(const std::size_t count,
                      read_some(input.get(), std::span{bytes}.subspan(filled), path));
        if (count == 0) {
            return fail(Errc::integrity, std::format("blob {} is shorter than its recorded size", hash));
        }
        filled += count;
    }
    return bytes;
}

Result<std::string> BlobStore::read_text(const Blake3Digest& hash, std::uint64_t max_size) const {
    RR_TRY_ASSIGN(const std::vector<std::byte> bytes, read_bytes(hash, max_size));
    std::string text(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(), [](std::byte byte) { return static_cast<char>(byte); });
    return text;
}

Result<void> BlobStore::verify(const Blake3Digest& hash, const std::stop_token& stop) const {
    RR_TRY_ASSIGN(const Blake3Digest actual, blake3_file(path_of(hash), stop));
    if (actual != hash) {
        return fail(Errc::integrity,
                    std::format("blob {} is corrupt: its content hashes to {}", hash, actual));
    }
    return ok();
}

Result<bool> BlobStore::remove(const Blake3Digest& hash) const {
    std::error_code ec;
    const bool removed = std::filesystem::remove(path_of(hash), ec);
    if (ec) {
        return fs_error("cannot remove blob", path_of(hash), ec);
    }
    return removed;
}

Result<void> BlobStore::touch(const Blake3Digest& hash) const {
    std::error_code ec;
    std::filesystem::last_write_time(path_of(hash), std::filesystem::file_time_type::clock::now(), ec);
    if (ec) {
        return fs_error("cannot touch blob", path_of(hash), ec);
    }
    return ok();
}

Result<std::size_t> BlobStore::remove_unused_since(std::filesystem::file_time_type cutoff) const {
    std::size_t removed = 0;
    std::size_t failures = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator fan{root_, ec}, end; !ec && fan != end; fan.increment(ec)) {
        const std::string fan_name = fan->path().filename().string();
        if (fan_name.size() != 2 || !fan->is_directory()) {
            continue;  // skips .tmp and anything that is not ours
        }
        std::error_code inner_ec;
        for (std::filesystem::directory_iterator it{fan->path(), inner_ec}; !inner_ec && it != end;
             it.increment(inner_ec)) {
            if (!Blake3Digest::from_hex(it->path().filename().string()).has_value()) {
                continue;  // not a blob name
            }
            std::error_code entry_ec;
            const auto written = it->last_write_time(entry_ec);
            if (entry_ec || written >= cutoff) {
                continue;
            }
            if (std::filesystem::remove(it->path(), entry_ec)) {
                ++removed;
            } else if (entry_ec) {
                ++failures;
            }
        }
    }
    if (ec) {
        return fs_error("cannot scan blob store", root_, ec);
    }
    if (failures > 0) {
        return fail(Errc::io, std::format("removed {} unused blob(s), but {} could not be removed from '{}'",
                                          removed, failures, root_.string()));
    }
    return removed;
}

}  // namespace rr::blob
