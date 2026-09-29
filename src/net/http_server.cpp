#include <rr/net/http_server.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <format>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <httplib.h>

namespace rr::net {

namespace {

using Clock = std::chrono::steady_clock;

// Longest start() waits for the listening thread to come up.
constexpr std::chrono::seconds kStartTimeout{5};
// Extra workers (beyond Options::worker_threads) exit after idling this long.
constexpr std::chrono::seconds kWorkerIdleExit{30};

// When the current request began, for the access log. A request is handled
// on one worker thread from the pre-routing hook to the logger, so a
// thread-local carries the time between them.
std::optional<Clock::time_point>& request_started() noexcept {
    static thread_local std::optional<Clock::time_point> started;
    return started;
}

std::optional<Method> method_from(std::string_view name) noexcept {
    if (name == "GET") {
        return Method::get;
    }
    if (name == "HEAD") {
        return Method::head;
    }
    if (name == "POST") {
        return Method::post;
    }
    if (name == "PUT") {
        return Method::put;
    }
    if (name == "PATCH") {
        return Method::patch;
    }
    if (name == "DELETE") {
        return Method::del;
    }
    return std::nullopt;
}

// The server's worker pool. httplib runs each connection as one job, for
// its whole life, idle keep-alive time included, so the pool must grow with
// the number of open connections.
//
// httplib's own ThreadPool grows only when *no* worker is idle at the moment
// a job is queued. When several connections arrive together, each is
// counted against the same idle workers before any of them wakes up, no
// thread is added, and the surplus waits until a keep-alive connection times
// out: nine simultaneous connections against eight workers stalled one of
// them for exactly the keep-alive timeout (5 s). This queue grows whenever
// waiting jobs outnumber idle workers.
class ElasticTaskQueue final : public httplib::TaskQueue {
public:
    ElasticTaskQueue(std::size_t base_threads, std::size_t max_threads, std::chrono::seconds idle_exit)
        : base_(std::max<std::size_t>(base_threads, 1))
        , max_(std::max(max_threads, base_))
        , idle_exit_(idle_exit) {
        const std::lock_guard lock(mutex_);
        for (std::size_t index = 0; index < base_; ++index) {
            spawn_locked(/*permanent=*/true);
        }
    }
    ElasticTaskQueue(const ElasticTaskQueue&) = delete;
    ElasticTaskQueue& operator=(const ElasticTaskQueue&) = delete;
    ElasticTaskQueue(ElasticTaskQueue&&) = delete;
    ElasticTaskQueue& operator=(ElasticTaskQueue&&) = delete;
    ~ElasticTaskQueue() override { shutdown(); }

    bool enqueue(std::function<void()> job) override {
        {
            const std::lock_guard lock(mutex_);
            if (stopping_) {
                return false;
            }
            jobs_.push_back(std::move(job));
            if (jobs_.size() > idle_ && threads_.size() < max_) {
                reap_finished_locked();
                spawn_locked(/*permanent=*/false);
            }
        }
        wake_.notify_one();
        return true;
    }

    // Stops taking jobs, lets the workers finish the queued ones, joins them.
    void shutdown() override {
        std::vector<std::thread> threads;
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
            threads.swap(threads_);
            finished_.clear();
        }
        wake_.notify_all();
        for (std::thread& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    void spawn_locked(bool permanent) {
        threads_.emplace_back([this, permanent] { work(permanent); });
    }

    // Joins extra workers that exited after idling. They record their ID as
    // their last act under the lock, so joining them here cannot block long.
    void reap_finished_locked() {
        for (const std::thread::id id : finished_) {
            const auto thread = std::ranges::find(threads_, id, &std::thread::get_id);
            if (thread != threads_.end()) {
                thread->join();
                threads_.erase(thread);
            }
        }
        finished_.clear();
    }

    void work(bool permanent) {
        std::unique_lock lock(mutex_);
        while (true) {
            ++idle_;
            const auto ready = [this] { return stopping_ || !jobs_.empty(); };
            bool woken = true;
            if (permanent) {
                wake_.wait(lock, ready);
            } else {
                woken = wake_.wait_for(lock, idle_exit_, ready);
            }
            --idle_;
            if (!jobs_.empty()) {
                const std::function<void()> job = std::move(jobs_.front());
                jobs_.pop_front();
                lock.unlock();
                try {
                    job();
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // httplib catches handler exceptions itself; this guards
                    // the worker against anything that still slips through.
                }
                lock.lock();
                continue;
            }
            if (stopping_) {
                return;  // the queue is drained
            }
            if (!woken) {
                finished_.push_back(std::this_thread::get_id());  // an extra worker, idle too long
                return;
            }
        }
    }

    const std::size_t base_;
    const std::size_t max_;
    const std::chrono::seconds idle_exit_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    std::vector<std::thread::id> finished_;
    std::size_t idle_ = 0;
    bool stopping_ = false;
};

HttpServer::Request convert(const httplib::Request& in, bool with_body) {
    HttpServer::Request out;
    out.method = method_from(in.method).value_or(Method::get);
    out.path = in.path;
    for (const auto& [name, value] : in.headers) {
        out.headers.add(name, value);
    }
    if (with_body) {
        out.body = in.body;
    }
    out.path_params.insert(in.path_params.begin(), in.path_params.end());
    for (const auto& [name, value] : in.params) {
        out.query.emplace_back(name, value);
    }
    out.remote_address = in.remote_addr;
    return out;
}

// Adapts httplib's DataSink to our ChunkWriter.
class SinkWriter final : public HttpServer::ChunkWriter {
public:
    SinkWriter(httplib::DataSink& sink, const std::atomic<bool>& stopping) noexcept
        : sink_(&sink)
        , stopping_(&stopping) {}

    bool write(std::string_view bytes) override {
        if (bytes.empty()) {
            // An empty chunk would end a chunked body; just report the state.
            return sink_->is_writable();
        }
        return sink_->write(bytes.data(), bytes.size());
    }

    [[nodiscard]] bool stopping() const override { return stopping_->load(std::memory_order_relaxed); }

private:
    httplib::DataSink* sink_;
    const std::atomic<bool>* stopping_;
};

// Adapts httplib's ContentReader to our BodyReader.
class ContentBodyReader final : public HttpServer::BodyReader {
public:
    explicit ContentBodyReader(const httplib::ContentReader& reader) noexcept : reader_(&reader) {}

    Result<void> read(const std::function<Result<void>(std::string_view chunk)>& on_chunk) override {
        if (consumed_) {
            return fail(Errc::internal, "the request body was already read");
        }
        consumed_ = true;
        std::optional<Error> failure;
        const bool complete = (*reader_)([&](const char* data, std::size_t length) {
            auto accepted = on_chunk(std::string_view{data, length});
            if (!accepted.has_value()) {
                failure = std::move(accepted).error();
                return false;  // stop reading
            }
            return true;
        });
        if (failure.has_value()) {
            return fail(std::move(*failure));
        }
        if (!complete) {
            return fail(Errc::io,
                        "the request body could not be read completely (connection lost, or over the "
                        "size limit)");
        }
        return ok();
    }

private:
    const httplib::ContentReader* reader_;
    bool consumed_ = false;
};

void apply(HttpServer::Response response, httplib::Response& out, const std::atomic<bool>& stopping) {
    out.status = response.status;
    for (const auto& [name, value] : response.headers) {
        out.headers.emplace(name, value);
    }
    if (response.stream) {
        out.set_chunked_content_provider(response.content_type,
                                         [produce = std::move(response.stream), &stopping](
                                             std::size_t /*offset*/, httplib::DataSink& sink) {
                                             SinkWriter writer{sink, stopping};
                                             try {
                                                 produce(writer);
                                             } catch (...) {  // NOLINT(bugprone-empty-catch)
                                                 // The status line is already on the wire; ending the body is
                                                 // all that is left to do. The client sees a truncated
                                                 // stream.
                                             }
                                             sink.done();
                                             return true;
                                         });
        return;
    }
    out.set_content(std::move(response.body), response.content_type);
}

// Runs a handler; an exception becomes a 500 with a structured body.
template <class Produce>
void answer(httplib::Response& out, const std::atomic<bool>& stopping, Produce&& produce) {
    HttpServer::Response response;
    try {
        response = std::forward<Produce>(produce)();
    } catch (const std::exception& error) {
        response = HttpServer::Response::error(
            Error{Errc::internal, std::format("handler threw: {}", error.what())});
    } catch (...) {
        response = HttpServer::Response::error(Error{Errc::internal, "handler threw"});
    }
    apply(std::move(response), out, stopping);
}

}  // namespace

// ---------------------------------------------------------------------------
// Request helpers and response factories
// ---------------------------------------------------------------------------
std::optional<std::string_view> HttpServer::Request::path_param(std::string_view name) const {
    const auto found = path_params.find(std::string{name});
    if (found == path_params.end()) {
        return std::nullopt;
    }
    return std::string_view{found->second};
}

std::optional<std::string_view> HttpServer::Request::query_param(std::string_view name) const {
    const auto found = std::ranges::find(query, name, &std::pair<std::string, std::string>::first);
    if (found == query.end()) {
        return std::nullopt;
    }
    return std::string_view{found->second};
}

HttpServer::Response HttpServer::Response::json(int status, std::string body) {
    return Response{.status = status, .body = std::move(body), .content_type = "application/json"};
}

HttpServer::Response HttpServer::Response::text(int status, std::string body) {
    return Response{.status = status, .body = std::move(body), .content_type = "text/plain; charset=utf-8"};
}

HttpServer::Response HttpServer::Response::empty(int status) {
    return Response{.status = status, .content_type = "text/plain; charset=utf-8"};
}

HttpServer::Response HttpServer::Response::error(const Error& error) {
    Response response = json(http_status_for(error.code()), error_body(error));
    if (error.retry_after().has_value()) {
        // Whole seconds, rounded up: never tell a client to come back too early.
        const auto seconds = (error.retry_after()->count() + 999) / 1000;
        response.headers.add("Retry-After", std::to_string(seconds));
    }
    return response;
}

HttpServer::Response HttpServer::Response::event_stream(std::function<void(ChunkWriter& writer)> producer) {
    Response response{.status = 200, .content_type = "text/event-stream", .stream = std::move(producer)};
    response.headers.add("Cache-Control", "no-cache");
    response.headers.add("X-Accel-Buffering", "no");  // ask reverse proxies not to buffer the stream
    return response;
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------
struct HttpServer::Impl {
    Options options;
    httplib::Server server;
    Guard guard;
    AccessLog access_log;
    std::set<std::pair<std::string, std::string>> streaming_routes;  // (method, pattern)
    std::atomic<bool> stopping{false};
    std::atomic<bool> listen_returned{false};
    int port = 0;
    std::jthread listener;  // declared last: joined before the members it uses go away

    void register_route(Method method, const std::string& pattern, httplib::Server::Handler handler) {
        switch (method) {
            case Method::get:
            case Method::head:  // GET routes answer HEAD too
                server.Get(pattern, std::move(handler));
                break;
            case Method::post:
                server.Post(pattern, std::move(handler));
                break;
            case Method::put:
                server.Put(pattern, std::move(handler));
                break;
            case Method::patch:
                server.Patch(pattern, std::move(handler));
                break;
            case Method::del:
                server.Delete(pattern, std::move(handler));
                break;
        }
    }

    void install_hooks() {
        server.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response& response) {
            request_started() = Clock::now();
            if (!guard) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            std::optional<Response> verdict;
            try {
                verdict = guard(convert(request, /*with_body=*/false));
            } catch (const std::exception& error) {
                verdict =
                    Response::error(Error{Errc::internal, std::format("guard threw: {}", error.what())});
            } catch (...) {
                verdict = Response::error(Error{Errc::internal, "guard threw"});
            }
            if (!verdict.has_value()) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            apply(std::move(*verdict), response, stopping);
            return httplib::Server::HandlerResponse::Handled;
        });

        // Runs after a route matched, before its body is read.
        server.set_pre_request_handler([this](const httplib::Request& request, httplib::Response& response) {
            if (streaming_routes.contains({request.method, request.matched_route})) {
                return httplib::Server::HandlerResponse::Unhandled;  // reads its own body, within
                                                                     // max_stream_bytes
            }
            const auto reject = [&](int status, std::string message) {
                Response answer = Response::error(Error{Errc::invalid_argument, std::move(message)});
                answer.status = status;
                apply(std::move(answer), response, stopping);
                return httplib::Server::HandlerResponse::Handled;
            };
            if (request.has_header("Transfer-Encoding")) {
                // A buffered route must know the size before reading it.
                return reject(411, "this endpoint needs a Content-Length");
            }
            if (request.get_header_value_u64("Content-Length", 0) > options.max_body_bytes) {
                return reject(413, std::format("the request body exceeds {} bytes", options.max_body_bytes));
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        server.set_logger([this](const httplib::Request& request, const httplib::Response& response) {
            const auto started = std::exchange(request_started(), std::nullopt);
            if (!access_log) {
                return;
            }
            const AccessLogEntry entry{
                .method = request.method,
                .path = request.path,
                .status = response.status,
                .remote_address = request.remote_addr,
                .duration = started.has_value() ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      Clock::now() - *started)
                                                : std::chrono::milliseconds{0}};
            try {
                access_log(entry);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // A failing log must not take the request down with it.
            }
        });
    }
};

HttpServer::HttpServer(Options options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::route(Method method, const std::string& pattern, Handler handler) {
    impl_->register_route(
        method, pattern,
        [this, handler = std::move(handler)](const httplib::Request& request, httplib::Response& response) {
            answer(response, impl_->stopping, [&] { return handler(convert(request, true)); });
        });
}

void HttpServer::route_streaming(Method method, const std::string& pattern, StreamingHandler handler) {
    impl_->streaming_routes.emplace(std::string{to_string(method)}, pattern);
    auto adapter = [this, handler = std::move(handler)](const httplib::Request& request,
                                                        httplib::Response& response,
                                                        const httplib::ContentReader& content) {
        answer(response, impl_->stopping, [&] {
            ContentBodyReader body{content};
            return handler(convert(request, false), body);
        });
    };
    switch (method) {
        case Method::post:
            impl_->server.Post(pattern, std::move(adapter));
            break;
        case Method::put:
            impl_->server.Put(pattern, std::move(adapter));
            break;
        case Method::patch:
            impl_->server.Patch(pattern, std::move(adapter));
            break;
        case Method::del:
            impl_->server.Delete(pattern, std::move(adapter));
            break;
        case Method::get:
        case Method::head:
            break;  // requests without bodies have nothing to stream; use route()
    }
}

void HttpServer::set_guard(Guard guard) {
    impl_->guard = std::move(guard);
}

void HttpServer::set_access_log(AccessLog log) {
    impl_->access_log = std::move(log);
}

Result<void> HttpServer::start() {
    Impl& impl = *impl_;
    if (impl.listener.joinable()) {
        return fail(Errc::conflict, "the server is already running");
    }
    impl.install_hooks();
    impl.server.set_read_timeout(impl.options.read_timeout);
    impl.server.set_write_timeout(impl.options.write_timeout);
    impl.server.set_keep_alive_timeout(impl.options.keep_alive_timeout);
    // The global limit is the streaming one; buffered routes are held to
    // max_body_bytes by the pre-request hook, before anything is read.
    impl.server.set_payload_max_length(std::max(impl.options.max_body_bytes, impl.options.max_stream_bytes));
    // An exclusive listening socket. httplib's default adds SO_REUSEPORT, which
    // lets a second process bind the same port and silently take half the
    // connections. SO_REUSEADDR alone still allows a quick restart while old
    // connections linger in TIME_WAIT.
    impl.server.set_socket_options([](::socket_t socket) {
        const int enable = 1;
        (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    });
    // Streamed tokens are small writes; without TCP_NODELAY each could wait for
    // the previous one's ACK (tens of milliseconds with delayed ACKs).
    impl.server.set_tcp_nodelay(true);
    const std::size_t threads = std::max<std::size_t>(impl.options.worker_threads, 1);
    const std::size_t max_threads = std::max(threads, impl.options.max_worker_threads);
    // httplib's contract: the factory returns an owning raw pointer, which
    // the server deletes after shutdown().
    impl.server.new_task_queue = [threads, max_threads] {
        return new ElasticTaskQueue(threads, max_threads,  // NOLINT(cppcoreguidelines-owning-memory)
                                    kWorkerIdleExit);
    };

    int port = -1;
    if (impl.options.port == 0) {
        port = impl.server.bind_to_any_port(impl.options.host);
    } else if (impl.server.bind_to_port(impl.options.host, impl.options.port)) {
        port = impl.options.port;
    }
    if (port < 0) {
        return fail(Errc::io, std::format("cannot listen on {}:{} (is the port in use?)", impl.options.host,
                                          impl.options.port));
    }
    impl.port = port;
    impl.stopping = false;
    impl.listen_returned = false;
    impl.listener = std::jthread{[&impl] {
        (void)impl.server.listen_after_bind();
        impl.listen_returned = true;
    }};
    // httplib's wait_until_ready() would spin forever if listening failed;
    // wait for either outcome, with a deadline.
    const auto deadline = Clock::now() + kStartTimeout;
    while (!impl.server.is_running()) {
        if (impl.listen_returned || Clock::now() > deadline) {
            stop();
            return fail(Errc::io, std::format("the server on port {} did not start", port));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return ok();
}

int HttpServer::port() const noexcept {
    return impl_->port;
}

void HttpServer::stop() {
    Impl& impl = *impl_;
    if (!impl.listener.joinable()) {
        return;
    }
    impl.stopping = true;
    impl.server.stop();
    impl.listener.join();
}

}  // namespace rr::net
