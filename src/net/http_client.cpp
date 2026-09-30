#include <rr/net/http_client.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <format>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#include <rr/core/scope_exit.hpp>
#include <rr/core/strings.hpp>
#include <rr/core/unique_fd.hpp>

namespace rr::net {

namespace {

using Clock = std::chrono::steady_clock;

// Longest a transfer loop sleeps in curl_multi_poll() between checks.
constexpr std::chrono::milliseconds kMaxPollWait{1000};

// libcurl's global state must be initialized once, before any other libcurl
// call and before other threads use it. A function-local static gives exactly
// that, thread-safely; it is never torn down (process lifetime).
bool curl_ready() noexcept {
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return initialized;
}

struct EasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};
struct MultiDeleter {
    void operator()(CURLM* handle) const noexcept { curl_multi_cleanup(handle); }
};
struct ShareDeleter {
    void operator()(CURLSH* handle) const noexcept { curl_share_cleanup(handle); }
};
struct SlistDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};

// One reusable transfer slot: an easy handle driven through its own multi
// handle. The multi handle owns the connection cache, so a slot's
// connections survive between requests.
struct Slot {
    std::unique_ptr<CURLM, MultiDeleter> multi;
    std::unique_ptr<CURL, EasyDeleter> easy;
};

// Mutexes for the share object (DNS cache, TLS sessions), one per data kind.
struct ShareLocks {
    std::array<std::mutex, CURL_LOCK_DATA_LAST> mutexes;
};

void lock_share(CURL* /*handle*/, curl_lock_data data, curl_lock_access /*access*/, void* user) {
    static_cast<ShareLocks*>(user)->mutexes.at(static_cast<std::size_t>(data)).lock();
}

void unlock_share(CURL* /*handle*/, curl_lock_data data, void* user) {
    static_cast<ShareLocks*>(user)->mutexes.at(static_cast<std::size_t>(data)).unlock();
}

// Per-request state shared with libcurl's callbacks. Callbacks run inside
// curl_multi_perform() on the requesting thread, so no locking is needed.
struct Exchange {
    const HttpClient::StreamHandlers* handlers = nullptr;  // null: buffer the whole body
    std::size_t max_body = 0;
    bool follow_redirects = false;  // libcurl follows 3xx + Location itself
    HttpClient::Response response{};
    bool head_delivered = false;
    std::optional<Error> failure{};  // why a callback aborted the transfer
    Clock::time_point last_activity = Clock::now();
    UniqueFd upload{};
    std::filesystem::path upload_path{};
};

// Runs a user callback, which must not let an exception unwind through
// libcurl's C frames.
template <class F>
Result<void> guarded(F&& callback) noexcept {
    try {
        return std::forward<F>(callback)();
    } catch (const std::exception& error) {
        return fail(Errc::internal, std::format("stream handler threw: {}", error.what()));
    } catch (...) {
        return fail(Errc::internal, "stream handler threw");
    }
}

int parse_status_line(std::string_view line) {
    // "HTTP/1.1 200 OK", "HTTP/2 404"
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos || line.size() < space + 4) {
        return 0;
    }
    int status = 0;
    for (const char c : line.substr(space + 1, 3)) {
        if (c < '0' || c > '9') {
            return 0;
        }
        status = status * 10 + (c - '0');
    }
    return status;
}

Result<void> deliver_head(Exchange& exchange) {
    if (exchange.head_delivered || exchange.handlers == nullptr) {
        return ok();
    }
    exchange.head_delivered = true;
    if (!exchange.handlers->on_head) {
        return ok();
    }
    return guarded(
        [&] { return exchange.handlers->on_head(exchange.response.status, exchange.response.headers); });
}

// Whether the header block that just ended is the final response's: not an
// interim 1xx ("100 Continue", "103 Early Hints") and not a redirect that
// libcurl is about to follow. When in doubt it answers "not final", which only
// delays the head until the first body byte or the end of the transfer (the
// fallbacks in on_body() and perform()); it never loses it.
bool is_final_head(const Exchange& exchange) {
    const int status = exchange.response.status;
    if (status >= 100 && status < 200) {
        return false;
    }
    const bool followed_redirect = exchange.follow_redirects && status >= 300 && status < 400 &&
                                   status != 304 && exchange.response.headers.contains("Location");
    return !followed_redirect;
}

std::size_t on_header(char* buffer, std::size_t size, std::size_t count, void* user) {
    auto& exchange = *static_cast<Exchange*>(user);
    const std::size_t total = size * count;
    exchange.last_activity = Clock::now();
    std::string_view line{buffer, total};
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    if (line.starts_with("HTTP/")) {
        // A new response begins (after a redirect or a "100 Continue"):
        // headers of the previous one do not apply.
        exchange.response.status = parse_status_line(line);
        exchange.response.headers = Headers{};
        return total;
    }
    if (line.empty()) {
        // The blank line that ends a header block. Hand the head over now:
        // the body may not start for a long time (a model reading its prompt).
        // Later blank lines (after trailers) find the head already delivered.
        if (is_final_head(exchange)) {
            if (auto head = deliver_head(exchange); !head.has_value()) {
                exchange.failure = std::move(head).error();
                return 0;  // anything but `total` aborts the transfer
            }
        }
        return total;
    }
    if (const std::size_t colon = line.find(':'); colon != std::string_view::npos) {
        exchange.response.headers.add(std::string{trim(line.substr(0, colon))},
                                      std::string{trim(line.substr(colon + 1))});
    }
    return total;
}

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto& exchange = *static_cast<Exchange*>(user);
    const std::size_t total = size * count;
    exchange.last_activity = Clock::now();
    const std::string_view chunk{data, total};

    if (auto head = deliver_head(exchange); !head.has_value()) {
        exchange.failure = std::move(head).error();
        return 0;  // anything but `total` aborts the transfer
    }
    if (exchange.handlers != nullptr && exchange.handlers->on_chunk && exchange.response.success()) {
        if (auto delivered = guarded([&] { return exchange.handlers->on_chunk(chunk); });
            !delivered.has_value()) {
            exchange.failure = std::move(delivered).error();
            return 0;
        }
        return total;
    }
    // Buffered: send(), or an error body from stream().
    if (exchange.response.body.size() + total > exchange.max_body) {
        exchange.failure = Error{Errc::io, std::format("response body exceeds {} bytes", exchange.max_body)};
        return 0;
    }
    exchange.response.body.append(chunk);
    return total;
}

std::size_t on_upload(char* buffer, std::size_t size, std::size_t count, void* user) {
    auto& exchange = *static_cast<Exchange*>(user);
    while (true) {
        const ssize_t read_count = ::read(exchange.upload.get(), buffer, size * count);
        if (read_count >= 0) {
            exchange.last_activity = Clock::now();  // a slow upload is not an idle one
            return static_cast<std::size_t>(read_count);
        }
        if (errno != EINTR) {
            exchange.failure =
                Error{Errc::io, std::format("cannot read '{}': {}", exchange.upload_path.string(),
                                            std::system_category().message(errno))};
            return CURL_READFUNC_ABORT;
        }
    }
}

// curl_easy_setopt() is a C variadic function: each option must be passed
// with exactly the type libcurl reads (long, curl_off_t, a pointer). This
// wrapper rejects plain `int` at compile time and keeps the first error.
class OptionSetter {
public:
    explicit OptionSetter(CURL* easy) noexcept : easy_(easy) {}

    template <class T>
    OptionSetter& operator()(CURLoption option, T value) noexcept {
        static_assert(!std::is_same_v<T, int> && !std::is_same_v<T, bool>,
                      "libcurl reads numeric options as long or curl_off_t");
        if (code_ == CURLE_OK) {
            code_ = curl_easy_setopt(easy_, option, value);
        }
        return *this;
    }

    [[nodiscard]] CURLcode code() const noexcept { return code_; }

private:
    CURL* easy_;
    CURLcode code_ = CURLE_OK;
};

long to_long_ms(std::chrono::milliseconds duration) noexcept {
    return static_cast<long>(std::max<std::int64_t>(duration.count(), 1));
}

Result<std::unique_ptr<curl_slist, SlistDeleter>> build_header_list(const HttpClient::Request& request) {
    std::unique_ptr<curl_slist, SlistDeleter> list;
    const auto append = [&](const std::string& line) -> Result<void> {
        curl_slist* extended = curl_slist_append(list.get(), line.c_str());
        if (extended == nullptr) {
            return fail(Errc::internal, "out of memory building request headers");
        }
        (void)list.release();  // curl_slist_append returned the (possibly same) head
        list.reset(extended);
        return ok();
    };
    for (const auto& [name, value] : request.headers) {
        // "Name:" would *remove* a header in libcurl's syntax; "Name;" sends it empty.
        RR_TRY(append(value.empty() ? std::format("{};", name) : std::format("{}: {}", name, value)));
    }
    // No "Expect: 100-continue" round trip before uploads.
    if (!request.headers.contains("Expect")) {
        RR_TRY(append("Expect:"));
    }
    const bool sends_body = request.method != Method::get && request.method != Method::head;
    if (sends_body && !request.headers.contains("Content-Type")) {
        RR_TRY(append("Content-Type: application/octet-stream"));  // not curl's form-urlencoded default
    }
    return list;
}

Errc classify(CURLcode code) noexcept {
    switch (code) {
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_HTTP2:
        case CURLE_HTTP2_STREAM:
        case CURLE_SSL_CONNECT_ERROR:
            return Errc::network;  // transient: worth retrying
        case CURLE_OPERATION_TIMEDOUT:
            return Errc::timeout;
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CERTPROBLEM:
            return Errc::permission_denied;  // a TLS verification failure is not transient
        case CURLE_URL_MALFORMAT:
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_TOO_MANY_REDIRECTS:
            return Errc::invalid_argument;
        case CURLE_ABORTED_BY_CALLBACK:
            return Errc::cancelled;
        default:
            return Errc::internal;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl: the handle pool and the shared caches
// ---------------------------------------------------------------------------
struct HttpClient::Impl {
    // Declaration order is destruction order in reverse: the pool's handles
    // detach from the share (taking its locks) before the share goes, and
    // the share is cleaned up before its locks.
    Options options;
    std::string ca_bundle;  // stable storage for CURLOPT_CAINFO
    ShareLocks share_locks;
    std::unique_ptr<CURLSH, ShareDeleter> share;
    std::mutex pool_mutex;
    std::vector<std::unique_ptr<Slot>> pool;

    Result<std::unique_ptr<Slot>> acquire() {
        {
            const std::lock_guard lock(pool_mutex);
            if (!pool.empty()) {
                std::unique_ptr<Slot> slot = std::move(pool.back());
                pool.pop_back();
                return slot;
            }
        }
        auto slot = std::make_unique<Slot>();
        slot->multi.reset(curl_multi_init());
        slot->easy.reset(curl_easy_init());
        if (!slot->multi || !slot->easy) {
            return fail(Errc::internal, "cannot create a libcurl handle");
        }
        return slot;
    }

    void release(std::unique_ptr<Slot> slot) {
        curl_easy_reset(slot->easy.get());  // no dangling pointers to finished requests
        const std::lock_guard lock(pool_mutex);
        if (pool.size() < options.max_pooled_handles) {
            pool.push_back(std::move(slot));
        }
    }

    Result<Response> perform(const Request& request, const StreamHandlers* handlers,
                             const std::stop_token& stop);
};

namespace {

Result<void> configure(CURL* easy, const HttpClient::Request& request, const HttpClient::Options& options,
                       const std::string& ca_bundle, CURLSH* share, Exchange& exchange, curl_slist* headers,
                       char* error_buffer) {
    OptionSetter set{easy};
    set(CURLOPT_URL, request.url.c_str())(CURLOPT_SHARE, share)(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_USERAGENT, options.user_agent.c_str())(CURLOPT_CONNECTTIMEOUT_MS,
                                                       to_long_ms(options.connect_timeout));
    if (request.accept_compression) {
        set(CURLOPT_ACCEPT_ENCODING, "");  // every encoding this libcurl can decode
    }
    // An HTTPS proxy's "200 Connection established" is not the server's answer.
    set(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    set(CURLOPT_PROTOCOLS_STR, "http,https")(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, request.follow_redirects ? 1L : 0L)(CURLOPT_MAXREDIRS, 5L);
    set(CURLOPT_ERRORBUFFER, error_buffer)(CURLOPT_HTTPHEADER, headers);
    set(CURLOPT_HEADERFUNCTION, &on_header)(CURLOPT_HEADERDATA, static_cast<void*>(&exchange));
    set(CURLOPT_WRITEFUNCTION, &on_body)(CURLOPT_WRITEDATA, static_cast<void*>(&exchange));
    if (request.timeout.has_value()) {
        set(CURLOPT_TIMEOUT_MS, to_long_ms(*request.timeout));
    }
    if (!ca_bundle.empty()) {
        set(CURLOPT_CAINFO, ca_bundle.c_str());
    }

    const std::string_view method = to_string(request.method);
    if (request.method == Method::get) {
        set(CURLOPT_HTTPGET, 1L);
    } else if (request.method == Method::head) {
        set(CURLOPT_NOBODY, 1L);
    } else if (request.body_file.has_value()) {
        exchange.upload_path = *request.body_file;
        int fd = -1;
        do {
            fd = ::open(request.body_file->c_str(), O_RDONLY | O_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            return fail(errno == ENOENT ? Errc::not_found : Errc::io,
                        std::format("cannot open '{}': {}", request.body_file->string(),
                                    std::system_category().message(errno)));
        }
        exchange.upload = UniqueFd{fd};
        struct stat info{};
        if (::fstat(fd, &info) != 0) {
            return fail(Errc::io, std::format("cannot stat '{}'", request.body_file->string()));
        }
        // UPLOAD streams from the read callback with a known length; the
        // custom request line keeps the intended method (UPLOAD alone means PUT).
        set(CURLOPT_UPLOAD, 1L)(CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(info.st_size));
        set(CURLOPT_READFUNCTION, &on_upload)(CURLOPT_READDATA, static_cast<void*>(&exchange));
        set(CURLOPT_CUSTOMREQUEST, method.data());
    } else {
        set(CURLOPT_POSTFIELDS, request.body.data())(CURLOPT_POSTFIELDSIZE_LARGE,
                                                     static_cast<curl_off_t>(request.body.size()));
        if (request.method != Method::post) {
            set(CURLOPT_CUSTOMREQUEST, method.data());
        }
    }
    if (set.code() != CURLE_OK) {
        return fail(Errc::internal,
                    std::format("cannot configure the request: {}", curl_easy_strerror(set.code())));
    }
    return ok();
}

}  // namespace

Result<HttpClient::Response> HttpClient::Impl::perform(const Request& request, const StreamHandlers* handlers,
                                                       const std::stop_token& stop) {
    if (!curl_ready()) {
        return fail(Errc::internal, "libcurl failed to initialize");
    }
    RR_TRY_ASSIGN(std::unique_ptr<Slot> slot, acquire());
    const ScopeExit give_back{[&] { release(std::move(slot)); }};
    CURL* easy = slot->easy.get();
    CURLM* multi = slot->multi.get();

    Exchange exchange{.handlers = handlers,
                      .max_body = options.max_response_bytes,
                      .follow_redirects = request.follow_redirects};
    RR_TRY_ASSIGN(const auto header_list, build_header_list(request));
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    RR_TRY(configure(easy, request, options, ca_bundle, share.get(), exchange, header_list.get(),
                     error_buffer.data()));

    if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
        return fail(Errc::internal, "cannot start the transfer");
    }
    const ScopeExit detach{[&] { curl_multi_remove_handle(multi, easy); }};
    // Wakes curl_multi_poll() the moment a stop is requested. Declared after
    // `detach`, so it is gone before the handle leaves the multi handle.
    const std::stop_callback wake_on_stop{stop, [multi] { curl_multi_wakeup(multi); }};

    const std::string description = std::format("{} {}", to_string(request.method), request.url);
    int running = 1;
    while (running != 0) {
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            return fail(Errc::internal, std::format("{}: libcurl transfer failure", description));
        }
        if (running == 0) {
            break;
        }
        if (stop.stop_requested()) {
            return fail(Errc::cancelled, std::format("{}: cancelled", description));
        }
        auto wait = kMaxPollWait;
        if (request.idle_timeout.has_value()) {
            const auto idle =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - exchange.last_activity);
            if (idle >= *request.idle_timeout) {
                return fail(Errc::timeout,
                            std::format("{}: no data for {} ms", description, request.idle_timeout->count()));
            }
            wait = std::min(wait, *request.idle_timeout - idle);
        }
        if (curl_multi_poll(multi, nullptr, 0, static_cast<int>(std::max<std::int64_t>(wait.count(), 1)),
                            nullptr) != CURLM_OK) {
            return fail(Errc::internal, std::format("{}: libcurl poll failure", description));
        }
    }

    CURLcode outcome = CURLE_OK;
    int remaining = 0;
    while (const CURLMsg* message = curl_multi_info_read(multi, &remaining)) {
        if (message->msg == CURLMSG_DONE) {
            outcome =
                message->data.result;  // NOLINT(cppcoreguidelines-pro-type-union-access) — libcurl's C API
        }
    }
    if (exchange.failure.has_value()) {
        return fail(std::move(*exchange.failure).with_context(description));  // a callback's own reason
    }
    if (outcome != CURLE_OK) {
        const std::string_view detail = error_buffer.front() != '\0'
                                            ? std::string_view{error_buffer.data()}
                                            : std::string_view{curl_easy_strerror(outcome)};
        return fail(classify(outcome), std::format("{}: {}", description, detail));
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    exchange.response.status = static_cast<int>(status);
    // A body-less response (204, HEAD) never reached the body callback.
    if (auto head = deliver_head(exchange); !head.has_value()) {
        return fail(std::move(head).error().with_context(description));
    }
    return std::move(exchange.response);
}

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------
HttpClient::HttpClient(Options options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    if (impl_->options.ca_bundle.has_value()) {
        impl_->ca_bundle = impl_->options.ca_bundle->string();
    }
    if (curl_ready()) {
        impl_->share.reset(curl_share_init());
        if (impl_->share) {
            CURLSH* share = impl_->share.get();
            curl_share_setopt(share, CURLSHOPT_LOCKFUNC, &lock_share);
            curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, &unlock_share);
            curl_share_setopt(share, CURLSHOPT_USERDATA, static_cast<void*>(&impl_->share_locks));
            curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        }
    }
}

HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;
HttpClient::~HttpClient() = default;

Result<HttpClient::Response> HttpClient::send(const Request& request, const std::stop_token& stop) const {
    return impl_->perform(request, nullptr, stop);
}

Result<HttpClient::Response> HttpClient::stream(const Request& request, const StreamHandlers& handlers,
                                                const std::stop_token& stop) const {
    return impl_->perform(request, &handlers, stop);
}

Result<HttpClient::Response> expect_success(Result<HttpClient::Response> response) {
    if (!response.has_value() || response->success()) {
        return response;
    }
    return fail(error_from_response(response->status, response->headers, response->body));
}

}  // namespace rr::net
