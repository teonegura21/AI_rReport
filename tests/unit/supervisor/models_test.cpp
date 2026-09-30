// Model files: verification with its cache, and resumable downloads, against
// an in-process file server that behaves like the ones downloads meet in the
// wild (ranges honoured or ignored, cut connections, tokens, redirects).
#include <rr/supervisor/models.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <netinet/in.h>

#include <rr/core/hash.hpp>
#include <rr/core/strings.hpp>
#include <rr/core/unique_fd.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::supervisor {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kPiece = std::size_t{64} << 10U;  // the file server writes 64 KiB at a time

std::string random_bytes(std::size_t size, unsigned seed) {
    std::mt19937 engine{seed};
    std::uniform_int_distribution<int> byte{0, 255};
    std::string bytes(size, '\0');
    for (char& c : bytes) {
        c = static_cast<char>(byte(engine));
    }
    return bytes;
}

// A deliberately small HTTP/1.1 server that serves one file the way a CDN
// does: byte ranges on a streamed body, an honest Content-Length, and cuts
// made by closing the connection early. One request per connection.
//
// cpp-httplib cannot play this part: for a streamed body it silently turns a
// handler's 206 into a 200 when the request carries Range (range_error()
// clears the ranges), and it slices buffered 206 bodies itself. A client that
// trusts status codes, as it must, would then restart every resume.
//
// Routes: /files/model.gguf (the file) and /redirect/model.gguf (a 302 to the
// file on "localhost", another host name for the same server).
class FileServer {
public:
    struct Seen {
        std::string host;
        std::string range;
        std::string authorization;
        std::string accept_encoding;
    };

    explicit FileServer(std::string content) : content_(std::move(content)) {
        listener_ = UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
        auto* generic = reinterpret_cast<sockaddr*>(&address);
        socklen_t length = sizeof(address);
        if (!listener_.valid() || ::bind(listener_.get(), generic, length) != 0 ||
            ::listen(listener_.get(), 16) != 0 || ::getsockname(listener_.get(), generic, &length) != 0) {
            throw std::runtime_error("the file server cannot listen");
        }
        port_ = ntohs(address.sin_port);
        acceptor_ = std::jthread{[this](const std::stop_token& stop) { accept_loop(stop); }};
    }

    [[nodiscard]] std::string url(std::string_view path, std::string_view host = "127.0.0.1") const {
        return std::format("http://{}:{}{}", host, port_, path);
    }

    // Behaviours, set before the transfers they should affect.
    void ignore_ranges() { ignore_range_ = true; }
    void misplace_ranges() { misplace_range_ = true; }  // answer 206, but from byte 0
    void cut(int transfers, std::size_t after_bytes) {  // the next `transfers` stop early
        cut_after_ = after_bytes;
        cuts_left_ = transfers;
    }
    void pace(std::chrono::milliseconds per_piece) {
        pause_ms_per_piece_ = static_cast<int>(per_piece.count());
    }
    void require_token(std::string token) {
        const std::lock_guard lock(mutex_);
        required_token_ = std::move(token);
    }

    [[nodiscard]] std::vector<Seen> seen() {
        const std::lock_guard lock(mutex_);
        return seen_;
    }

private:
    void accept_loop(const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            pollfd ready{.fd = listener_.get(), .events = POLLIN, .revents = 0};
            if (::poll(&ready, 1, 50) <= 0) {
                continue;
            }
            const UniqueFd connection{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
            if (connection.valid()) {
                serve(connection.get(), stop);
            }
        }
    }

    // Sends everything, or returns false once the client is gone.
    static bool send_all(int fd, std::string_view bytes) {
        while (!bytes.empty()) {
            const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (sent <= 0) {
                if (sent < 0 && errno == EINTR) {
                    continue;
                }
                return false;
            }
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    static std::optional<std::string> read_head(int fd) {
        std::string head;
        std::array<char, 4096> buffer{};
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 65536) {
            pollfd ready{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&ready, 1, 2000) <= 0) {
                return std::nullopt;
            }
            const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count <= 0) {
                return std::nullopt;
            }
            head.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return head;
    }

    void serve(int fd, const std::stop_token& stop) {
        const auto head = read_head(fd);
        if (!head.has_value()) {
            return;
        }
        const std::size_t line_end = head->find("\r\n");
        const std::vector<std::string_view> request_line =
            split(std::string_view{*head}.substr(0, line_end), ' ');
        const std::string_view path = request_line.size() > 1 ? request_line[1] : "";
        const auto header = [&](std::string_view name) -> std::string {
            for (const std::string_view line : split(std::string_view{*head}.substr(line_end + 2), '\n')) {
                const std::size_t colon = line.find(':');
                if (colon != std::string_view::npos && iequals_ascii(trim(line.substr(0, colon)), name)) {
                    return std::string{trim(line.substr(colon + 1))};
                }
            }
            return {};
        };

        if (path == "/redirect/model.gguf") {
            (void)send_all(fd, std::format("HTTP/1.1 302 Found\r\nLocation: {}\r\nContent-Length: 0\r\n"
                                           "Connection: close\r\n\r\n",
                                           url("/files/model.gguf", "localhost")));
            return;
        }
        const std::string range = header("Range");
        {
            const std::lock_guard lock(mutex_);
            seen_.push_back(Seen{.host = header("Host"),
                                 .range = range,
                                 .authorization = header("Authorization"),
                                 .accept_encoding = header("Accept-Encoding")});
        }
        if (const auto token = required_token();
            token.has_value() && header("Authorization") != "Bearer " + *token) {
            const std::string_view body = R"({"error":{"message":"Access to this model is restricted"}})";
            (void)send_all(fd, std::format("HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
                                           "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                                           body.size(), body));
            return;
        }
        std::size_t start = 0;
        std::string status = "200 OK";
        std::string content_range;
        if (!range.empty() && !ignore_range_) {
            start = std::stoul(range.substr(std::string_view{"bytes="}.size()));  // "bytes=N-"
            if (start >= content_.size()) {
                (void)send_all(
                    fd, std::format("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */{}\r\n"
                                    "Content-Length: 0\r\nConnection: close\r\n\r\n",
                                    content_.size()));
                return;
            }
            status = "206 Partial Content";
            if (misplace_range_) {
                start = 0;  // a broken proxy or cache
            }
            content_range =
                std::format("Content-Range: bytes {}-{}/{}\r\n", start, content_.size() - 1, content_.size());
        }
        if (!send_all(
                fd,
                std::format("HTTP/1.1 {}\r\nContent-Type: application/octet-stream\r\nContent-Length: {}\r\n"
                            "{}Connection: close\r\n\r\n",
                            status, content_.size() - start, content_range))) {
            return;
        }
        const std::optional<std::size_t> stop_after =
            cuts_left_.fetch_sub(1) > 0 ? std::optional<std::size_t>{cut_after_.load()} : std::nullopt;
        std::size_t sent = 0;
        for (std::size_t at = start; at < content_.size() && !stop.stop_requested(); at += kPiece) {
            const std::string_view piece = std::string_view{content_}.substr(at, kPiece);
            if (stop_after.has_value() && sent + piece.size() >= *stop_after) {
                (void)send_all(fd, piece.substr(0, *stop_after - sent));
                return;  // closing now leaves the client short of Content-Length: a cut
            }
            if (!send_all(fd, piece)) {
                return;  // the client went away
            }
            sent += piece.size();
            std::this_thread::sleep_for(std::chrono::milliseconds{pause_ms_per_piece_.load()});
        }
    }

    [[nodiscard]] std::optional<std::string> required_token() {
        const std::lock_guard lock(mutex_);
        return required_token_;
    }

    std::string content_;
    std::atomic<bool> ignore_range_{false};
    std::atomic<bool> misplace_range_{false};
    std::atomic<int> cuts_left_{0};
    std::atomic<std::size_t> cut_after_{0};
    std::atomic<int> pause_ms_per_piece_{0};
    std::mutex mutex_;
    std::optional<std::string> required_token_;  // guarded by mutex_
    std::vector<Seen> seen_;                     // guarded by mutex_
    UniqueFd listener_;
    int port_ = 0;
    std::jthread acceptor_;  // declared last: stopped and joined before the members above go away
};

class ModelFiles : public ::testing::Test {
protected:
    [[nodiscard]] const std::string& content() const { return content_; }
    [[nodiscard]] const std::string& sha() const { return sha_; }
    [[nodiscard]] const testkit::TempDir& dir() const { return dir_; }
    [[nodiscard]] FileServer& server() { return server_; }
    [[nodiscard]] const net::HttpClient& client() const { return client_; }
    [[nodiscard]] SyncOptions& options() { return options_; }

    [[nodiscard]] NeededFile needed(const std::optional<std::string>& url = std::nullopt) const {
        return NeededFile{.model = "m",
                          .file = ModelFile{.file = "sub/model.gguf",
                                            .sha256 = sha_,
                                            .url = url.has_value() ? url : server_.url("/files/model.gguf")}};
    }
    [[nodiscard]] std::filesystem::path path() const { return dir_ / "sub" / "model.gguf"; }
    [[nodiscard]] std::filesystem::path partial() const {
        return dir_ / "sub" / std::format("model.gguf.{}.partial", sha_.substr(0, 16));
    }
    void place(const std::filesystem::path& where, std::string_view bytes) const {
        (void)dir_.write_file(where, bytes);
    }

private:
    std::string content_ = random_bytes(std::size_t{3} << 20U, 7);  // 3 MiB
    std::string sha_ = sha256(content_).hex();
    testkit::TempDir dir_{"rr_models_test"};
    FileServer server_{content_};
    net::HttpClient client_;
    SyncOptions options_{.attempts_without_progress = 3,
                         .backoff = {.initial_delay = 1ms, .max_delay = 5ms},
                         .idle_timeout = 5s};
};

// ---------------------------------------------------------------------------
// Verification and its cache
// ---------------------------------------------------------------------------
TEST_F(ModelFiles, VerifiesAndCachesTheResult) {
    place(path(), content());
    const FileCheck first = unwrap(verify_model_file(needed(), dir().path()));
    EXPECT_EQ(first.state, FileState::verified);
    EXPECT_FALSE(first.from_cache) << "hashed the first time";
    EXPECT_EQ(first.size, content().size());
    EXPECT_TRUE(std::filesystem::exists(dir() / "sub" / "model.gguf.verified"));

    const FileCheck second = unwrap(verify_model_file(needed(), dir().path()));
    EXPECT_EQ(second.state, FileState::verified);
    EXPECT_TRUE(second.from_cache) << "trusted from the record the second time";
    EXPECT_FALSE(unwrap(verify_model_file(needed(), dir().path(), {.full = true})).from_cache)
        << "unless asked to hash";
}

TEST_F(ModelFiles, AnyChangeToTheFileInvalidatesTheRecord) {
    place(path(), content());
    ASSERT_OK(verify_model_file(needed(), dir().path()));

    // Different bytes, same size, and the mtime put back (what `cp -p` does):
    // only the ctime tells.
    struct stat before{};
    ASSERT_EQ(::stat(path().c_str(), &before), 0);
    std::string altered = content();
    altered[1000] = static_cast<char>(altered[1000] ^ 0x5a);
    std::this_thread::sleep_for(20ms);  // a ctime that differs even on coarse clocks
    dir().write_file(path(), altered);
    const std::array<struct timespec, 2> times{before.st_atim, before.st_mtim};
    ASSERT_EQ(::utimensat(AT_FDCWD, path().c_str(), times.data(), 0), 0);

    const FileCheck check = unwrap(verify_model_file(needed(), dir().path()));
    EXPECT_FALSE(check.from_cache);
    EXPECT_EQ(check.state, FileState::mismatch);
    EXPECT_EQ(check.actual_sha256, sha256(altered).hex());
    EXPECT_TRUE(unwrap(verify_model_file(needed(), dir().path())).from_cache) << "a mismatch is recorded too";
}

TEST_F(ModelFiles, AFileThatChangesWhileHashedIsNotTrusted) {
    place(path(), content());
    bool changed = false;
    const auto result = verify_model_file(needed(), dir().path(), {.on_progress = [&](const Progress&) {
                                              if (!changed) {
                                                  changed = true;
                                                  place(path(), content() + "!");
                                              }
                                          }});
    ASSERT_ERRC(result, Errc::conflict);
    EXPECT_FALSE(std::filesystem::exists(dir() / "sub" / "model.gguf.verified")) << "nothing recorded";
}

TEST_F(ModelFiles, ReportsMissingAndWrongFiles) {
    EXPECT_EQ(unwrap(verify_model_file(needed(), dir().path())).state, FileState::missing);
    place(path(), "not the model");
    const FileCheck wrong = unwrap(verify_model_file(needed(), dir().path()));
    EXPECT_EQ(wrong.state, FileState::mismatch);
    EXPECT_EQ(wrong.actual_sha256, sha256("not the model").hex());
}

TEST_F(ModelFiles, ProfilesNeedEachFileOnce) {
    Catalog catalog;
    catalog.models = {Model{.id = "a",
                            .role = "commander",
                            .weights = {.file = "a.gguf"},
                            .projector = ModelFile{.file = "p.gguf"}},
                      Model{.id = "b",
                            .role = "infantry",
                            .weights = {.file = "b.gguf"},
                            .projector = ModelFile{.file = "p.gguf"}}};
    const Profile profile{.name = "x", .engines = {{.model = "a"}, {.model = "b"}}};
    std::vector<std::string> files;
    for (const NeededFile& file : profile_files(catalog, profile)) {
        files.push_back(file.file.file.string());
    }
    EXPECT_EQ(files, (std::vector<std::string>{"a.gguf", "p.gguf", "b.gguf"}));
}

// ---------------------------------------------------------------------------
// Downloads
// ---------------------------------------------------------------------------
TEST_F(ModelFiles, DownloadsVerifiesAndRecords) {
    const FileCheck check = unwrap(sync_model_file(client(), needed(), dir().path(), options()));
    EXPECT_EQ(check.state, FileState::verified);
    EXPECT_EQ(dir().read_file(path()), content());
    EXPECT_FALSE(std::filesystem::exists(partial())) << "published by rename";
    EXPECT_TRUE(unwrap(verify_model_file(needed(), dir().path())).from_cache) << "and recorded as verified";
    EXPECT_EQ(server().seen().size(), 1U);
    EXPECT_TRUE(unwrap(sync_model_file(client(), needed(), dir().path(), options())).from_cache)
        << "nothing to do the second time";
    EXPECT_EQ(server().seen().size(), 1U);
}

TEST_F(ModelFiles, DeclinesCompressionSoRangesMeanStoredBytes) {
    ASSERT_OK(sync_model_file(client(), needed(), dir().path(), options()));
    ASSERT_FALSE(server().seen().empty());
    EXPECT_EQ(server().seen()[0].accept_encoding, "");
}

TEST_F(ModelFiles, ResumesWhereAPartialDownloadEnded) {
    const std::size_t half = content().size() / 2;
    place(partial(), std::string_view{content()}.substr(0, half));
    ASSERT_OK(sync_model_file(client(), needed(), dir().path(), options()));
    EXPECT_EQ(dir().read_file(path()), content());
    ASSERT_EQ(server().seen().size(), 1U);
    EXPECT_EQ(server().seen()[0].range, std::format("bytes={}-", half));
}

TEST_F(ModelFiles, KeepsGoingWhileEachAttemptMakesProgress) {
    // Five cuts, each after 512 KiB more: more failures than the budget of
    // three without progress allows, but every one of them brought bytes.
    server().cut(5, std::size_t{512} << 10U);
    ASSERT_OK(sync_model_file(client(), needed(), dir().path(), options()));
    EXPECT_EQ(dir().read_file(path()), content());
    std::vector<std::string> ranges;
    for (const auto& seen : server().seen()) {
        ranges.push_back(seen.range);
    }
    EXPECT_EQ(ranges, (std::vector<std::string>{"", "bytes=524288-", "bytes=1048576-", "bytes=1572864-",
                                                "bytes=2097152-", "bytes=2621440-"}));
}

TEST_F(ModelFiles, GivesUpAfterFailuresWithoutProgress) {
    server().cut(100, 0);
    const auto result = sync_model_file(client(), needed(), dir().path(), options());
    ASSERT_ERRC(result, Errc::network);
    EXPECT_THAT(result.error().describe(), HasSubstr("gave up after 3 attempts without progress"));
    EXPECT_FALSE(std::filesystem::exists(path()));
}

TEST_F(ModelFiles, AResumeThatStartsElsewhereIsNeverAppended) {
    server().misplace_ranges();  // "206", but the whole file again
    place(partial(), std::string_view{content()}.substr(0, 1000));
    const auto result = sync_model_file(client(), needed(), dir().path(), options());
    ASSERT_ERRC(result, Errc::network) << "retried, then given up: never spliced into the partial";
    EXPECT_THAT(result.error().describe(), HasSubstr("asked to resume at byte 1000"));
    EXPECT_FALSE(std::filesystem::exists(path()));
}

TEST_F(ModelFiles, StartsOverWhenTheServerIgnoresTheRange) {
    server().ignore_ranges();
    place(partial(), std::string_view{content()}.substr(0, 1000));
    ASSERT_OK(sync_model_file(client(), needed(), dir().path(), options()));
    EXPECT_EQ(dir().read_file(path()), content()) << "the partial was truncated, not appended to";
}

TEST_F(ModelFiles, ANothingLeftAnswerMeansThePartialIsWhole) {
    place(partial(), content());  // complete, but never published (a crash before the rename)
    ASSERT_OK(sync_model_file(client(), needed(), dir().path(), options()));
    EXPECT_EQ(dir().read_file(path()), content());
    ASSERT_EQ(server().seen().size(), 1U);
    EXPECT_EQ(server().seen()[0].range, std::format("bytes={}-", content().size()));
}

TEST_F(ModelFiles, NeverPublishesContentWithAnotherHash) {
    NeededFile wrong = needed();
    wrong.file.sha256 = sha256("what the catalog expects").hex();
    const auto result = sync_model_file(client(), wrong, dir().path(), options());
    ASSERT_ERRC(result, Errc::integrity);
    EXPECT_THAT(result.error().message(), HasSubstr("downloaded twice"));
    EXPECT_EQ(server().seen().size(), 2U) << "one fresh start after the first mismatch";
    EXPECT_FALSE(std::filesystem::exists(path()));
    EXPECT_FALSE(std::filesystem::exists(
        dir() / "sub" / std::format("model.gguf.{}.partial", wrong.file.sha256.substr(0, 16))));
}

TEST_F(ModelFiles, SendsTheTokenOnlyToTheHub) {
    options().hub_token = Secret{"hf_secret"};
    options().token_hosts = {"127.0.0.1"};  // the "Hub" here; "localhost" plays the CDN
    ASSERT_OK(
        sync_model_file(client(), needed(server().url("/redirect/model.gguf")), dir().path(), options()));
    const auto seen = server().seen();
    ASSERT_EQ(seen.size(), 1U) << "the redirect itself is not a /files request";
    EXPECT_THAT(seen[0].host, HasSubstr("localhost"));
    EXPECT_EQ(seen[0].authorization, "") << "the token never follows a redirect to another host";

    std::filesystem::remove_all(dir() / "sub");
    ASSERT_OK(sync_model_file(client(), needed(server().url("/files/model.gguf", "localhost")), dir().path(),
                              options()));
    EXPECT_EQ(server().seen().back().authorization, "") << "nor is it sent to any other host";

    std::filesystem::remove_all(dir() / "sub");
    ASSERT_OK(sync_model_file(client(), needed(), dir().path(), options()));  // straight from the "Hub"
    EXPECT_EQ(server().seen().back().authorization, "Bearer hf_secret");
}

TEST_F(ModelFiles, ARefusalMentionsTheToken) {
    server().require_token("hf_secret");
    const auto result = sync_model_file(client(), needed(), dir().path(), options());
    ASSERT_ERRC(result, Errc::permission_denied);
    EXPECT_THAT(result.error().describe(), HasSubstr("HF_TOKEN"));
}

TEST_F(ModelFiles, ChecksFreeSpaceBeforeWriting) {
    options().free_space = [](const std::filesystem::path&) -> Result<std::uint64_t> { return 1000; };
    const auto result = sync_model_file(client(), needed(), dir().path(), options());
    ASSERT_ERRC(result, Errc::io);
    EXPECT_THAT(result.error().message(), HasSubstr("needs 3.1 MB more, but only 1.0 kB are free"));
    EXPECT_FALSE(std::filesystem::exists(path()));
}

TEST_F(ModelFiles, StopsPromptlyAndKeepsWhatArrived) {
    server().pace(50ms);  // 3 MiB would take about 2.5 s
    std::stop_source stop;
    options().stop = stop.get_token();
    const std::jthread stopper{[&] {
        std::this_thread::sleep_for(300ms);
        stop.request_stop();
    }};
    const auto started = Clock::now();
    ASSERT_ERRC(sync_model_file(client(), needed(), dir().path(), options()), Errc::cancelled);
    EXPECT_LT(Clock::now() - started, 1500ms);
    ASSERT_TRUE(std::filesystem::exists(partial()));
    EXPECT_GT(std::filesystem::file_size(partial()), 0U) << "kept for the next attempt";
}

TEST_F(ModelFiles, AMissingFileWithoutAUrlIsAClearError) {
    NeededFile orphan = needed();
    orphan.file.url.reset();
    const auto result = sync_model_file(client(), orphan, dir().path(), options());
    ASSERT_ERRC(result, Errc::not_found);
    EXPECT_THAT(result.error().message(), HasSubstr("no URL"));
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------
TEST(ModelUrls, KnowPinnedHubRevisionsAndHosts) {
    const std::string commit(40, 'a');
    EXPECT_TRUE(
        is_pinned_hub_url(std::format("https://huggingface.co/org/repo/resolve/{}/model.gguf", commit)));
    EXPECT_FALSE(is_pinned_hub_url("https://huggingface.co/org/repo/resolve/main/model.gguf"));
    EXPECT_FALSE(
        is_pinned_hub_url(std::format("https://huggingface.co/org/repo/resolve/{}X/m.gguf", commit)));

    EXPECT_EQ(url_host("https://huggingface.co/org/repo"), "huggingface.co");
    EXPECT_EQ(url_host("HTTPS://User:Pass@HF.co:443/x?y#z"), "hf.co");
    EXPECT_EQ(url_host("http://[::1]:8080/x"), "[::1]");
    EXPECT_EQ(url_host("ftp://example.com/x"), std::nullopt);
    EXPECT_EQ(url_host("not a url"), std::nullopt);
}

}  // namespace
}  // namespace rr::supervisor
