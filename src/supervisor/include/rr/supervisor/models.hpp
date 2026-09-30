// rr/supervisor/models.hpp — model files: verified before use, fetched when missing.
//
// Why this exists
//   Engines load multi-gigabyte GGUF files from the pod's network volume. A
//   file that is missing, cut short by an interrupted download or silently
//   corrupted makes llama-server fail minutes into loading, or load garbage.
//   So before the engines start, every file the chosen profile needs is
//   checked against the catalog's sha256, and missing or wrong files are
//   downloaded again.
//
//   * Verification is cached. Hashing tens of gigabytes on a network volume
//     takes minutes, and pods restart often. After a file is verified, a
//     sidecar "<file>.verified" records its size, mtime, ctime, inode and
//     hash, and a later check trusts it only while all of them still match.
//     ctime changes on every write and rename and cannot be set by copy
//     tools, so a file replaced with `cp -p` or `rsync -t` (which keep the
//     mtime) is still caught. The file is stat'ed before and after hashing,
//     and the sidecar is written only if nothing changed in between.
//     VerifyOptions::full ignores the cache.
//   * Downloads resume. A download goes to "<file>.<first 16 hex of the
//     sha256>.partial": the expected hash is in the name, so a partial of an
//     older version is never continued into a newer one. Resuming asks for
//     "Range: bytes=N-" after re-hashing the N bytes already there:
//       206  the server resumed at N (its Content-Range is checked): append
//       200  it ignored the range: start again from zero
//       416  nothing is left to send: verify what is there
//     Compression is declined (no Accept-Encoding), so offsets refer to the
//     stored bytes; some CDNs ignore Range on encoded responses.
//   * Only verified content is published. Bytes are hashed as they are
//     written, so a finished download needs no second read; then fsync,
//     rename, directory fsync, sidecar. A hash mismatch deletes the partial
//     and starts over once (the source may have moved between attempts);
//     a second mismatch is an integrity error, and nothing is published.
//   * Tokens stay with their host. The Hugging Face token is sent only to
//     the Hub's own hosts (`token_hosts`). The Hub redirects downloads to a
//     CDN with a signed URL, and libcurl drops the Authorization header on
//     redirects to another host.
//   * Retrying is patient with progress. A transfer that fails after
//     bringing new bytes resets the budget of `attempts_without_progress`,
//     so a 20 GB download that stalls a few times over an hour completes,
//     while one that fails repeatedly without a byte gives up.
//   * Free space is checked before writing, against the size the server
//     announces.
//
// Thread-safety: the functions are reentrant; distinct files may be handled
//   concurrently. The same file must not be synced by two callers at once.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/secret.hpp>
#include <rr/net/http_client.hpp>
#include <rr/net/retry.hpp>
#include <rr/supervisor/catalog.hpp>

namespace rr::supervisor {

// One file a profile needs, and the model that needs it.
struct NeededFile {
    std::string model{};
    ModelFile file{};
};

// Every file the profile's engines need (weights and projectors), in profile
// order, each once.
[[nodiscard]] std::vector<NeededFile> profile_files(const Catalog& catalog, const Profile& profile);

enum class FileState : std::uint8_t {
    verified,  // present, with the catalog's sha256
    missing,
    mismatch,  // present, with other content
};
[[nodiscard]] std::string_view to_string(FileState state) noexcept;

struct FileCheck {
    NeededFile needed{};
    FileState state = FileState::missing;
    std::uint64_t size = 0;
    bool from_cache = false;                     // decided by the sidecar, without hashing
    std::optional<std::string> actual_sha256{};  // the content's hash, when it was hashed and differs
};

// Progress of a long operation on one file: hashing or downloading.
struct Progress {
    const ModelFile* file = nullptr;
    std::uint64_t done = 0;                // bytes
    std::optional<std::uint64_t> total{};  // bytes, when known
};
using ProgressSink = std::function<void(const Progress& progress)>;

struct VerifyOptions {
    bool full = false;  // hash every file, ignoring the sidecar cache
    ProgressSink on_progress{};
    std::stop_token stop{};
};

// Checks `models_dir / needed.file.file` against the catalog's sha256. A file
// that changes while it is hashed is Errc::conflict.
[[nodiscard]] Result<FileCheck> verify_model_file(const NeededFile& needed,
                                                  const std::filesystem::path& models_dir,
                                                  const VerifyOptions& options = {});
[[nodiscard]] Result<std::vector<FileCheck>> verify_model_files(const Catalog& catalog,
                                                                const Profile& profile,
                                                                const std::filesystem::path& models_dir,
                                                                const VerifyOptions& options = {});

struct SyncOptions {
    std::optional<Secret> hub_token{};  // HF_TOKEN: gated and private repositories
    std::vector<std::string> token_hosts{"huggingface.co", "hf.co"};
    int attempts_without_progress = 5;
    net::RetryPolicy backoff{.initial_delay = std::chrono::seconds{1}, .max_delay = std::chrono::seconds{60}};
    std::chrono::milliseconds idle_timeout{std::chrono::minutes{2}};  // no byte for this long: retry
    ProgressSink on_progress{};
    // Free bytes on the filesystem holding `directory`; empty: statvfs().
    std::function<Result<std::uint64_t>(const std::filesystem::path& directory)> free_space{};
    std::stop_token stop{};
};

// Makes `models_dir / needed.file.file` hold the catalog's content: verified
// (cache allowed), or downloaded from the catalog's URL when missing or
// wrong. Errc::not_found when it is missing and the catalog has no URL.
[[nodiscard]] Result<FileCheck> sync_model_file(const net::HttpClient& client, const NeededFile& needed,
                                                const std::filesystem::path& models_dir,
                                                const SyncOptions& options = {});
[[nodiscard]] Result<std::vector<FileCheck>> sync_model_files(const net::HttpClient& client,
                                                              const Catalog& catalog, const Profile& profile,
                                                              const std::filesystem::path& models_dir,
                                                              const SyncOptions& options = {});

// Whether a Hugging Face URL names a commit (".../resolve/<40 hex>/..."), so
// its content can never change under the catalog's sha256.
[[nodiscard]] bool is_pinned_hub_url(std::string_view url) noexcept;

// The lowercase host of an http(s) URL, without user info or port.
[[nodiscard]] std::optional<std::string> url_host(std::string_view url);

}  // namespace rr::supervisor
