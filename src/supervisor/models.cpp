#include <rr/supervisor/models.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <format>
#include <span>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <rr/core/file_io.hpp>
#include <rr/core/hash.hpp>
#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>
#include <rr/log/log.hpp>

namespace rr::supervisor {

namespace {

constexpr std::size_t kChunk = std::size_t{4} << 20U;  // 4 MiB reads when hashing
constexpr mode_t kFileMode = 0644;
constexpr std::string_view kSidecarSuffix = ".verified";
constexpr std::size_t kPartialHashPrefix = 16;  // hex characters of the sha256 in a partial's name

// "17.20 GB", "3.1 MB", "512 bytes": sizes in messages people read.
std::string human_bytes(std::uint64_t bytes) {
    const auto value = static_cast<double>(bytes);
    if (bytes >= 1'000'000'000) {
        return std::format("{:.2f} GB", value / 1e9);
    }
    if (bytes >= 1'000'000) {
        return std::format("{:.1f} MB", value / 1e6);
    }
    if (bytes >= 1'000) {
        return std::format("{:.1f} kB", value / 1e3);
    }
    return std::format("{} bytes", bytes);
}

// What identifies a file's content without reading it. ctime changes on every
// write, rename or chmod and, unlike mtime, cannot be set from user space.
struct Identity {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::int64_t ctime_ns = 0;
    std::uint64_t inode = 0;

    friend bool operator==(const Identity&, const Identity&) = default;
};

std::int64_t nanoseconds(const struct timespec& time) noexcept {
    return (static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000) + time.tv_nsec;
}

// The identity of `path`; std::nullopt when there is no such file.
Result<std::optional<Identity>> identify(const std::filesystem::path& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            return std::optional<Identity>{};
        }
        return os_error("cannot stat", path, errno);
    }
    if (!S_ISREG(info.st_mode)) {
        return fail(Errc::invalid_argument, std::format("'{}' is not a regular file", path.string()));
    }
    return Identity{.size = static_cast<std::uint64_t>(info.st_size),
                    .mtime_ns = nanoseconds(info.st_mtim),
                    .ctime_ns = nanoseconds(info.st_ctim),
                    .inode = static_cast<std::uint64_t>(info.st_ino)};
}

// --- The verification cache -----------------------------------------------------

std::filesystem::path sidecar_path(const std::filesystem::path& file) {
    std::filesystem::path sidecar = file;
    sidecar += kSidecarSuffix;
    return sidecar;
}

struct Record {
    Identity identity;
    std::string sha256;
};

// The sidecar's record. Anything unreadable or malformed means "no cache".
std::optional<Record> read_sidecar(const std::filesystem::path& file) {
    const std::filesystem::path path = sidecar_path(file);
    const auto fd = open_file(path, O_RDONLY);
    if (!fd.has_value()) {
        return std::nullopt;
    }
    std::string text;
    std::array<std::byte, 1024> buffer{};
    while (text.size() < 4096) {  // a record is ~200 bytes; anything larger is not ours
        const auto count = read_some(fd->get(), buffer, path);
        if (!count.has_value()) {
            return std::nullopt;
        }
        if (*count == 0) {
            break;
        }
        text.append(reinterpret_cast<const char*>(
                        buffer.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                    *count);
    }
    const auto document = json::parse(text);
    if (!document.has_value() || !document->is_object()) {
        return std::nullopt;
    }
    try {
        Record record{.identity = {.size = document->at("size").get<std::uint64_t>(),
                                   .mtime_ns = document->at("mtime_ns").get<std::int64_t>(),
                                   .ctime_ns = document->at("ctime_ns").get<std::int64_t>(),
                                   .inode = document->at("inode").get<std::uint64_t>()},
                      .sha256 = document->at("sha256").get<std::string>()};
        return record;
    } catch (const json::Json::exception&) {
        return std::nullopt;  // a field missing or of the wrong type
    }
}

// Records that the file with `identity` hashes to `sha256`. Written under a
// temporary name and renamed, so a crash never leaves half a record.
Result<void> write_sidecar(const std::filesystem::path& file, const Identity& identity,
                           std::string_view sha256) {
    const std::filesystem::path path = sidecar_path(file);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    const std::string text = json::dump(json::Json{{"sha256", sha256},
                                                   {"size", identity.size},
                                                   {"mtime_ns", identity.mtime_ns},
                                                   {"ctime_ns", identity.ctime_ns},
                                                   {"inode", identity.inode}});
    {
        RR_TRY_ASSIGN(UniqueFd fd, open_file(temporary, O_WRONLY | O_CREAT | O_TRUNC, kFileMode));
        RR_TRY(write_all(fd.get(), text, temporary));
        RR_TRY(sync_file(fd.get(), temporary));
        if (const int error_number = fd.close(); error_number != 0) {
            return os_error("cannot close", temporary, error_number);
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        return os_error("cannot rename", temporary, ec.value());
    }
    return sync_directory(path.parent_path());
}

// --- Hashing ----------------------------------------------------------------------

// Feeds the whole file into `hasher`; returns how many bytes that was.
Result<std::uint64_t> hash_into(Sha256Hasher& hasher, const std::filesystem::path& path,
                                const ModelFile& file, std::optional<std::uint64_t> total,
                                const ProgressSink& on_progress, const std::stop_token& stop) {
    RR_TRY_ASSIGN(const UniqueFd fd, open_file(path, O_RDONLY));
    std::vector<std::byte> buffer(kChunk);
    std::uint64_t done = 0;
    while (true) {
        if (stop.stop_requested()) {
            return fail(Errc::cancelled, std::format("hashing '{}' was cancelled", path.string()));
        }
        RR_TRY_ASSIGN(const std::size_t count, read_some(fd.get(), buffer, path));
        if (count == 0) {
            return done;
        }
        hasher.update(std::span{buffer}.first(count));
        done += count;
        if (on_progress) {
            on_progress(Progress{.file = &file, .done = done, .total = total});
        }
    }
}

// --- Downloading --------------------------------------------------------------------

std::filesystem::path partial_path(const std::filesystem::path& final_path, const ModelFile& file) {
    std::filesystem::path partial = final_path;
    partial += std::format(".{}.partial", std::string_view{file.sha256}.substr(0, kPartialHashPrefix));
    return partial;
}

std::optional<std::uint64_t> parse_number(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

struct ContentRange {
    std::uint64_t start = 0;
    std::optional<std::uint64_t> total{};  // "*" when the server does not say
};

// "bytes START-END/TOTAL" (RFC 9110 §14.4).
std::optional<ContentRange> parse_content_range(std::string_view value) {
    constexpr std::string_view kUnit = "bytes ";
    if (!value.starts_with(kUnit)) {
        return std::nullopt;
    }
    value.remove_prefix(kUnit.size());
    const std::size_t dash = value.find('-');
    const std::size_t slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash > slash) {
        return std::nullopt;
    }
    const auto start = parse_number(value.substr(0, dash));
    if (!start.has_value() || !parse_number(value.substr(dash + 1, slash - dash - 1)).has_value()) {
        return std::nullopt;
    }
    const std::string_view total = value.substr(slash + 1);
    return ContentRange{.start = *start, .total = total == "*" ? std::nullopt : parse_number(total)};
}

Result<std::uint64_t> statvfs_free_space(const std::filesystem::path& directory) {
    struct statvfs info{};
    if (::statvfs(directory.c_str(), &info) != 0) {
        return os_error("cannot inspect the filesystem of", directory, errno);
    }
    return static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
}

Result<void> ensure_space(const std::filesystem::path& directory, std::uint64_t needed,
                          const SyncOptions& options) {
    RR_TRY_ASSIGN(const std::uint64_t free,
                  options.free_space ? options.free_space(directory) : statvfs_free_space(directory));
    if (free < needed) {
        return fail(Errc::io, std::format("the download needs {} more, but only {} are free on '{}'",
                                          human_bytes(needed), human_bytes(free), directory.string()));
    }
    return ok();
}

// Moves the write position of `fd` to `offset`, dropping anything after it.
Result<void> truncate_to(int fd, std::uint64_t offset, const std::filesystem::path& path) {
    if (::ftruncate(fd, static_cast<off_t>(offset)) != 0) {
        return os_error("cannot truncate", path, errno);
    }
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return os_error("cannot seek in", path, errno);
    }
    return ok();
}

// The token to send with a request for `url`: the Hub's own hosts only.
std::optional<std::string_view> token_for(const std::string& url, const SyncOptions& options) {
    if (!options.hub_token.has_value() || options.hub_token->empty()) {
        return std::nullopt;
    }
    const auto host = url_host(url);
    if (!host.has_value() || std::ranges::find(options.token_hosts, *host) == options.token_hosts.end()) {
        return std::nullopt;
    }
    return options.hub_token->expose();
}

// One transfer, continuing the partial file from where it ends.
class Transfer {
public:
    Transfer(const ModelFile& file, std::string url, std::filesystem::path partial,
             const SyncOptions& options)
        : file_(&file)
        , url_(std::move(url))
        , partial_(std::move(partial))
        , options_(&options) {}

    // The sha256 of the whole partial file, once the server has nothing more
    // to send (all of it arrived, or it answered 416).
    Result<std::string> run(const net::HttpClient& client) {
        RR_TRY(prepare());
        RR_TRY_ASSIGN(const net::HttpClient::Response response,
                      client.stream(request(),
                                    {.on_head =
                                         [this](int status, const net::Headers& headers) {
                                             return on_head(status, headers);
                                         },
                                     .on_chunk = [this](std::string_view chunk) { return on_chunk(chunk); }},
                                    options_->stop));
        return finish(response);
    }

private:
    // Re-hashes what is already there, and opens the file to continue at its end.
    Result<void> prepare() {
        RR_TRY_ASSIGN(const std::optional<Identity> existing, identify(partial_));
        if (existing.has_value() && existing->size > 0) {
            log::info("resuming '{}' at {}: re-hashing what is there", partial_.filename().string(),
                      human_bytes(existing->size));
            RR_TRY_ASSIGN(offset_, hash_into(hasher_, partial_, *file_, std::nullopt, {}, options_->stop));
        }
        written_ = offset_;
        RR_TRY_ASSIGN(out_, open_file(partial_, O_WRONLY | O_CREAT, kFileMode));
        return truncate_to(out_.get(), offset_, partial_);  // what was hashed is exactly what is kept
    }

    [[nodiscard]] net::HttpClient::Request request() const {
        net::HttpClient::Request request{.url = url_,
                                         .idle_timeout = options_->idle_timeout,
                                         .follow_redirects = true,
                                         .accept_compression = false};
        if (offset_ > 0) {
            request.headers.add("Range", std::format("bytes={}-", offset_));
        }
        if (const auto token = token_for(url_, *options_); token.has_value()) {
            request.headers.add("Authorization", std::format("Bearer {}", *token));
        }
        return request;
    }

    Result<void> on_head(int status, const net::Headers& headers) {
        if (status == 206) {
            RR_TRY(resume(headers));
        } else if (status == 200) {
            RR_TRY(start_over(headers));
        } else {
            nothing_left_ = status == 416;  // the partial already ends where the file does
            return ok();                    // other statuses: the body is buffered, read in finish()
        }
        if (total_.has_value() && *total_ > written_) {
            return ensure_space(partial_.parent_path(), *total_ - written_, *options_);
        }
        return ok();
    }

    // 206: the server continues at our offset, and nowhere else.
    Result<void> resume(const net::Headers& headers) {
        const std::string_view value = headers.get("Content-Range").value_or("");
        const auto range = parse_content_range(value);
        if (!range.has_value() || range->start != offset_) {
            return fail(Errc::network,
                        std::format("asked to resume at byte {}, the server answered '{}'", offset_,
                                    value.empty() ? std::string_view{"no Content-Range"} : value));
        }
        total_ = range->total;
        return ok();
    }

    // 200: the server sends the whole file, whatever was asked.
    Result<void> start_over(const net::Headers& headers) {
        if (offset_ > 0) {
            RR_TRY(truncate_to(out_.get(), 0, partial_));
            hasher_ = Sha256Hasher{};
            offset_ = 0;
            written_ = 0;
        }
        if (const auto length = headers.get("Content-Length"); length.has_value()) {
            total_ = parse_number(*length);
        }
        return ok();
    }

    Result<void> on_chunk(std::string_view chunk) {
        RR_TRY(write_all(out_.get(), chunk, partial_));
        hasher_.update(chunk);
        written_ += chunk.size();
        if (options_->on_progress) {
            options_->on_progress(Progress{.file = file_, .done = written_, .total = total_});
        }
        return ok();
    }

    Result<std::string> finish(const net::HttpClient::Response& response) {
        if (nothing_left_) {
            return hasher_.finalize().hex();  // the hasher holds exactly the partial's bytes
        }
        if (!response.success()) {
            Error error = net::error_from_response(response.status, response.headers, response.body);
            if (response.status == 401 || response.status == 403) {
                error = std::move(error).with_context(
                    "the repository may be gated or private: set a Hugging Face token (HF_TOKEN) that has "
                    "accepted the model's licence");
            }
            return fail(std::move(error));
        }
        RR_TRY(sync_file(out_.get(), partial_));
        if (const int error_number = out_.close(); error_number != 0) {
            return os_error("cannot close", partial_, error_number);
        }
        return hasher_.finalize().hex();
    }

    const ModelFile* file_;
    std::string url_;
    std::filesystem::path partial_;
    const SyncOptions* options_;
    Sha256Hasher hasher_;
    UniqueFd out_;
    std::uint64_t offset_ = 0;   // where this transfer continues the file
    std::uint64_t written_ = 0;  // the partial file's size now
    std::optional<std::uint64_t> total_{};
    bool nothing_left_ = false;
};

std::uint64_t size_or_zero(const std::filesystem::path& path) {
    const auto identity = identify(path);
    return identity.has_value() && identity->has_value() ? (*identity)->size : 0;
}

// Transfers until the partial holds the whole file; returns its sha256.
// Transient failures are retried; progress resets the retry budget.
Result<std::string> download(const net::HttpClient& client, const ModelFile& file, const std::string& url,
                             const std::filesystem::path& partial, const SyncOptions& options) {
    int failures_without_progress = 0;
    std::uint64_t furthest = size_or_zero(partial);
    while (true) {
        auto attempt = Transfer{file, url, partial, options}.run(client);
        if (attempt.has_value()) {
            return attempt;
        }
        Error error = std::move(attempt).error().with_context(std::format("downloading '{}'", url));
        if (!error.transient()) {
            return fail(std::move(error));
        }
        const std::uint64_t reached = size_or_zero(partial);
        failures_without_progress = reached > furthest ? 1 : failures_without_progress + 1;
        furthest = std::max(furthest, reached);
        if (failures_without_progress >= options.attempts_without_progress) {
            return fail(std::move(error).with_context(
                std::format("gave up after {} attempts without progress", failures_without_progress)));
        }
        const auto delay =
            net::backoff_delay(options.backoff, failures_without_progress, error, net::random_unit());
        log::warn("{}; retrying in {} ms", error.describe(), delay.count());
        if (!net::pause_for(delay, options.stop)) {
            return fail(Errc::cancelled,
                        std::format("cancelled while waiting to retry: {}", error.describe()));
        }
    }
}

Result<void> publish(const std::filesystem::path& partial, const std::filesystem::path& path,
                     std::string_view sha256) {
    std::error_code ec;
    std::filesystem::rename(partial, path, ec);
    if (ec) {
        return os_error("cannot publish", path, ec.value());
    }
    RR_TRY(sync_directory(path.parent_path()));
    RR_TRY_ASSIGN(const std::optional<Identity> identity, identify(path));
    if (!identity.has_value()) {
        return fail(Errc::io, std::format("'{}' vanished right after it was published", path.string()));
    }
    if (auto recorded = write_sidecar(path, *identity, sha256); !recorded.has_value()) {
        log::warn(
            "'{}' is verified but its record could not be written ({}); it will be hashed again next time",
            path.string(), recorded.error().describe());
    }
    return ok();
}

}  // namespace

std::vector<NeededFile> profile_files(const Catalog& catalog, const Profile& profile) {
    std::vector<NeededFile> needed;
    for (const EngineSpec& engine : profile.engines) {
        const Model* model = catalog.find_model(engine.model);
        if (model == nullptr) {
            continue;  // parse_catalog() refuses such profiles
        }
        for (const ModelFile* file : model->files()) {
            const bool seen = std::ranges::any_of(
                needed, [&](const NeededFile& other) { return other.file.file == file->file; });
            if (!seen) {
                needed.push_back(NeededFile{.model = model->id, .file = *file});
            }
        }
    }
    return needed;
}

std::string_view to_string(FileState state) noexcept {
    switch (state) {
        case FileState::verified:
            return "verified";
        case FileState::missing:
            return "missing";
        case FileState::mismatch:
            return "mismatch";
    }
    return "unknown";
}

Result<FileCheck> verify_model_file(const NeededFile& needed, const std::filesystem::path& models_dir,
                                    const VerifyOptions& options) {
    const std::filesystem::path path = models_dir / needed.file.file;
    FileCheck check{.needed = needed};
    RR_TRY_ASSIGN(const std::optional<Identity> before, identify(path));
    if (!before.has_value()) {
        return check;  // missing
    }
    check.size = before->size;
    if (!options.full) {
        if (const auto record = read_sidecar(path); record.has_value() && record->identity == *before) {
            check.from_cache = true;
            check.state = record->sha256 == needed.file.sha256 ? FileState::verified : FileState::mismatch;
            if (check.state == FileState::mismatch) {
                check.actual_sha256 = record->sha256;
            }
            return check;
        }
    }
    Sha256Hasher hasher;
    RR_TRY(hash_into(hasher, path, needed.file, before->size, options.on_progress, options.stop));
    std::string actual = hasher.finalize().hex();
    RR_TRY_ASSIGN(const std::optional<Identity> after, identify(path));
    if (after != before) {
        return fail(Errc::conflict, std::format("'{}' changed while it was being verified", path.string()));
    }
    if (auto recorded = write_sidecar(path, *before, actual); !recorded.has_value()) {
        log::warn("could not record the verification of '{}' ({}); it will be hashed again next time",
                  path.string(), recorded.error().describe());
    }
    check.state = actual == needed.file.sha256 ? FileState::verified : FileState::mismatch;
    if (check.state == FileState::mismatch) {
        check.actual_sha256 = std::move(actual);
    }
    return check;
}

Result<std::vector<FileCheck>> verify_model_files(const Catalog& catalog, const Profile& profile,
                                                  const std::filesystem::path& models_dir,
                                                  const VerifyOptions& options) {
    std::vector<FileCheck> checks;
    for (const NeededFile& needed : profile_files(catalog, profile)) {
        RR_TRY_ASSIGN(FileCheck check, verify_model_file(needed, models_dir, options));
        checks.push_back(std::move(check));
    }
    return checks;
}

Result<FileCheck> sync_model_file(const net::HttpClient& client, const NeededFile& needed,
                                  const std::filesystem::path& models_dir, const SyncOptions& options) {
    RR_TRY_ASSIGN(
        FileCheck check,
        verify_model_file(needed, models_dir, {.on_progress = options.on_progress, .stop = options.stop}));
    if (check.state == FileState::verified) {
        return check;
    }
    const std::filesystem::path path = models_dir / needed.file.file;
    if (!needed.file.url.has_value()) {
        return fail(Errc::not_found,
                    std::format("'{}' (model '{}') is {}, and the catalog has no URL to fetch it from",
                                path.string(), needed.model, to_string(check.state)));
    }
    const std::string& url = *needed.file.url;
    if (const auto host = url_host(url);
        host.has_value() && std::ranges::find(options.token_hosts, *host) != options.token_hosts.end() &&
        !is_pinned_hub_url(url)) {
        log::warn(
            "'{}' is not pinned to a commit: if the repository changes, the download will not match the "
            "catalog's sha256",
            url);
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return os_error("cannot create", path.parent_path(), ec.value());
    }
    const std::filesystem::path partial = partial_path(path, needed.file);
    for (int round = 1;; ++round) {
        RR_TRY_ASSIGN(const std::string sha256, download(client, needed.file, url, partial, options));
        if (sha256 == needed.file.sha256) {
            break;
        }
        std::filesystem::remove(partial, ec);
        if (round == 2) {
            return fail(
                Errc::integrity,
                std::format("'{}' has sha256 {}, not the catalog's {} (downloaded twice); nothing was stored",
                            url, sha256, needed.file.sha256));
        }
        log::warn(
            "the download of '{}' does not match the catalog's sha256; downloading it again from the start",
            url);
    }
    RR_TRY(publish(partial, path, needed.file.sha256));
    RR_TRY_ASSIGN(const std::optional<Identity> identity, identify(path));
    check.state = FileState::verified;
    check.size = identity.has_value() ? identity->size : 0;
    check.from_cache = false;
    check.actual_sha256.reset();
    return check;
}

Result<std::vector<FileCheck>> sync_model_files(const net::HttpClient& client, const Catalog& catalog,
                                                const Profile& profile,
                                                const std::filesystem::path& models_dir,
                                                const SyncOptions& options) {
    std::vector<FileCheck> checks;
    for (const NeededFile& needed : profile_files(catalog, profile)) {
        RR_TRY_ASSIGN(FileCheck check, sync_model_file(client, needed, models_dir, options));
        checks.push_back(std::move(check));
    }
    return checks;
}

bool is_pinned_hub_url(std::string_view url) noexcept {
    constexpr std::string_view kResolve = "/resolve/";
    constexpr std::size_t kCommitLength = 40;
    const std::size_t at = url.find(kResolve);
    if (at == std::string_view::npos) {
        return false;
    }
    const std::string_view revision = url.substr(at + kResolve.size());
    return revision.size() > kCommitLength && revision[kCommitLength] == '/' &&
           std::ranges::all_of(revision.substr(0, kCommitLength),
                               [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::optional<std::string> url_host(std::string_view url) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string scheme = to_lower_ascii(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") {
        return std::nullopt;
    }
    std::string_view authority = url.substr(scheme_end + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);  // user info
    }
    if (authority.starts_with('[')) {  // an IPv6 literal keeps its brackets
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        authority = authority.substr(0, close + 1);
    } else {
        authority = authority.substr(0, authority.find(':'));  // the port
    }
    if (authority.empty()) {
        return std::nullopt;
    }
    return to_lower_ascii(authority);
}

}  // namespace rr::supervisor
