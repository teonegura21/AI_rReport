// rr/blob/blob_store.hpp — content-addressed, crash-safe file storage.
//
// Why this exists
//   Media and large payloads (audio, keyframes, caption files; uploads on
//   the runtime) are stored once, named by the BLAKE3 hash of their content.
//   Content addressing gives three properties for free:
//     * deduplication: the same audio stored twice occupies space once;
//     * integrity: a blob's name *is* its checksum, and verify() re-checks it;
//     * idempotent transfers: the hub asks the runtime "do you have <hash>?"
//       and uploads only what is missing (ADR-0001, §4.1 of the plan).
//
// Crash safety
//   A blob appears under its final name only when it is complete and durable:
//     1. bytes go to <root>/.tmp/<ulid> (the same filesystem as the final
//        name), hashed while they are written;
//     2. the file is fsync'ed and its hash checked against the expected one,
//        if the caller gave one;
//     3. rename() moves it to <root>/<first 2 hex>/<hash> (atomic in POSIX);
//     4. the directory is fsync'ed, so the rename itself survives a power cut.
//   Readers therefore never observe a partial blob. A crash leaves at most a
//   stray temp file, which a later open() sweeps away.
//
// Layout
//   <root>/ab/ab34…   blobs (64 hex characters), fanned out over up to 256
//                     directories so that none grows huge
//   <root>/.tmp/      writes in progress
//   Blob files are created 0600 and store directories 0700: blobs are the
//   user's media.
//
// Thread-safety: a BlobStore is an immutable value (its root path) and may be
//   used from any number of threads; concurrent writes of the same content
//   are harmless, because identical content has an identical name. A single
//   BlobWriter belongs to one thread.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/hash.hpp>
#include <rr/core/result.hpp>

namespace rr::blob {

// A stored blob: its content hash and size in bytes.
struct BlobRef {
    Blake3Digest hash{};
    std::uint64_t size = 0;

    friend bool operator==(const BlobRef&, const BlobRef&) = default;
};

inline constexpr std::uint64_t kNoSizeLimit = std::numeric_limits<std::uint64_t>::max();
// Default cap for read_bytes()/read_text(), which load a blob into memory.
inline constexpr std::uint64_t kDefaultReadLimit = std::uint64_t{256} << 20U;  // 256 MiB

// An in-progress blob. Stream bytes in with write(), then commit(). A writer
// destroyed without a successful commit() deletes its temporary file.
// After any failure the writer is spent: further calls fail.
class BlobWriter {
public:
    BlobWriter(BlobWriter&&) noexcept;
    BlobWriter& operator=(BlobWriter&&) noexcept;
    BlobWriter(const BlobWriter&) = delete;
    BlobWriter& operator=(const BlobWriter&) = delete;
    ~BlobWriter();

    // Appends bytes. Errc::invalid_argument past the writer's size limit;
    // Errc::io on write errors (e.g. a full disk).
    [[nodiscard]] Result<void> write(std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> write(std::string_view text);

    // Makes the blob durable and visible under its hash. With `expected`, a
    // different content hash fails with Errc::integrity and nothing is
    // published — how uploads are verified on arrival.
    [[nodiscard]] Result<BlobRef> commit(const std::optional<Blake3Digest>& expected = std::nullopt);

    // Bytes written so far.
    [[nodiscard]] std::uint64_t size() const noexcept;

private:
    friend class BlobStore;
    struct State;
    explicit BlobWriter(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

class BlobStore {
public:
    // Temp files untouched for this long are leftovers of crashed writes.
    static constexpr std::chrono::hours kStaleTempAge{1};

    // Opens the store rooted at `root`, creating directories as needed, and
    // sweeps stale temp files.
    [[nodiscard]] static Result<BlobStore> open(const std::filesystem::path& root);

    // --- Writing ---------------------------------------------------------
    [[nodiscard]] Result<BlobWriter> begin_write(std::uint64_t max_size = kNoSizeLimit) const;
    [[nodiscard]] Result<BlobRef> put_bytes(std::span<const std::byte> bytes) const;
    [[nodiscard]] Result<BlobRef> put_text(std::string_view text) const;
    // Copies a file into the store, hashing as it goes. Errc::cancelled when
    // `stop` is requested mid-copy (nothing is published).
    [[nodiscard]] Result<BlobRef> put_file(const std::filesystem::path& source,
                                           const std::stop_token& stop = {}) const;

    // --- Reading -----------------------------------------------------------
    [[nodiscard]] bool contains(const Blake3Digest& hash) const;
    // Where the blob lives (whether or not it exists), for tools such as
    // ffmpeg that take a path.
    [[nodiscard]] std::filesystem::path path_of(const Blake3Digest& hash) const;
    // Errc::not_found when absent.
    [[nodiscard]] Result<BlobRef> stat(const Blake3Digest& hash) const;
    // Load a whole blob into memory; Errc::invalid_argument above `max_size`.
    [[nodiscard]] Result<std::vector<std::byte>> read_bytes(const Blake3Digest& hash,
                                                            std::uint64_t max_size = kDefaultReadLimit) const;
    [[nodiscard]] Result<std::string> read_text(const Blake3Digest& hash,
                                                std::uint64_t max_size = kDefaultReadLimit) const;
    // Re-hashes the blob: Errc::integrity when its content no longer matches
    // its name (bit rot, or tampering).
    [[nodiscard]] Result<void> verify(const Blake3Digest& hash, const std::stop_token& stop = {}) const;

    // --- Housekeeping -------------------------------------------------------
    // Deletes a blob. Returns false when it was not there (not an error:
    // the goal "this blob is absent" holds either way).
    [[nodiscard]] Result<bool> remove(const Blake3Digest& hash) const;
    // Marks a blob as recently used (its modification time), which protects
    // it from remove_unused_since().
    [[nodiscard]] Result<void> touch(const Blake3Digest& hash) const;
    // Deletes blobs neither written nor touched since `cutoff`, and returns
    // how many were deleted. The runtime uses this to evict idle uploads.
    [[nodiscard]] Result<std::size_t> remove_unused_since(std::filesystem::file_time_type cutoff) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    explicit BlobStore(std::filesystem::path root) noexcept : root_(std::move(root)) {}

    std::filesystem::path root_;  // absolute
};

}  // namespace rr::blob
