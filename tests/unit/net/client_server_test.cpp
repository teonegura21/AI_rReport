// End-to-end tests: a real HttpServer on an ephemeral loopback port, driven
// by the real HttpClient.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <format>
#include <latch>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <netinet/in.h>

#include <rr/core/hash.hpp>
#include <rr/core/scope_exit.hpp>
#include <rr/core/unique_fd.hpp>
#include <rr/net/http_client.hpp>
#include <rr/net/http_server.hpp>
#include <rr/net/retry.hpp>
#include <rr/net/sse.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::net {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;
using Response = HttpServer::Response;

constexpr std::string_view kToken = "Bearer s3cret";

std::string random_payload(std::size_t size) {
    std::mt19937 engine{42};
    std::uniform_int_distribution<int> byte{0, 255};
    std::string payload(size, '\0');
    for (char& c : payload) {
        c = static_cast<char>(byte(engine));
    }
    return payload;
}

// The padding of /heavy-head's head: far more than one loopback connection
// holds on its way (the sender's buffer grows to 4 MB by default, and a
// SlowReader's is pinned small), so the head waits for its client to read.
constexpr std::size_t kHeavyHead = std::size_t{32} << 20U;

// A client that reads only when told to: a raw connection whose receive
// buffer is pinned small, so what the server sends waits in the server's own
// buffer, and a write larger than both waits for the client.
class SlowReader {
public:
    SlowReader(int port, std::string_view path) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (!fd_.valid()) {
            ADD_FAILURE() << "cannot open a socket: " << std::system_category().message(errno);
            return;
        }
        const int small = 4096;  // pinned: the kernel no longer grows it
        (void)::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        const timeval patience{.tv_sec = 20, .tv_usec = 0};  // a broken test fails, and never hangs
        (void)::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
        const sockaddr_in address = testkit::detail::loopback(port);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
        if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            ADD_FAILURE() << "cannot connect: " << std::system_category().message(errno);
            return;
        }
        const std::string request =
            std::format("GET {} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
        if (::send(fd_.get(), request.data(), request.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(request.size())) {
            ADD_FAILURE() << "cannot send the request: " << std::system_category().message(errno);
        }
    }

    // Up to `count` bytes, as they come: fewer when the server closes first.
    std::string read(std::size_t count) {
        std::string received;
        std::vector<char> buffer(std::size_t{1} << 16U);
        while (received.size() < count) {
            const ssize_t got =
                ::recv(fd_.get(), buffer.data(), std::min(buffer.size(), count - received.size()), 0);
            if (got < 0) {
                ADD_FAILURE() << "cannot read: " << std::system_category().message(errno);
            }
            if (got <= 0) {
                break;
            }
            received.append(buffer.data(), static_cast<std::size_t>(got));
        }
        return received;
    }

    // Everything until the server closes the connection.
    std::string read_to_end() { return read(std::numeric_limits<std::size_t>::max()); }

private:
    UniqueFd fd_;
};

// State the routes share with the tests.
struct ServerState {
    std::atomic<int> private_uploads_run{0};
    std::atomic<int> flaky_calls{0};
    std::atomic<bool> slow_stream_ended{false};
    std::atomic<bool> unheeding_released{false};
    std::atomic<bool> waiting_to_stream{false};
    std::atomic<bool> head_seen{false};
    std::atomic<bool> waiting_for_leave{false};
    std::atomic<bool> noticed_leave{false};
    std::atomic<int> logged{0};
    std::mutex log_mutex;
    std::vector<HttpServer::AccessLogEntry> log;
};

class ClientServer : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<HttpServer>(HttpServer::Options{.worker_threads = 8});
        add_routes(*server_, state_);
        ASSERT_OK(server_->start());
    }
    void TearDown() override { server_->stop(); }

    [[nodiscard]] std::string url(std::string_view path) const {
        return std::format("http://127.0.0.1:{}{}", server_->port(), path);
    }
    [[nodiscard]] Result<HttpClient::Response> get(std::string_view path, Headers headers = {}) const {
        return client_.send(HttpClient::Request{.url = url(path), .headers = std::move(headers)});
    }
    [[nodiscard]] const HttpClient& client() const { return client_; }
    [[nodiscard]] HttpServer& server() const { return *server_; }
    [[nodiscard]] ServerState& state() { return state_; }

private:
    static void add_routes(HttpServer& server, ServerState& state);

    ServerState state_;
    std::unique_ptr<HttpServer> server_;
    HttpClient client_;
};

void ClientServer::add_routes(HttpServer& server, ServerState& state) {
    server.route(Method::get, "/hello", [](const auto&) { return Response::text(200, "hello"); });
    server.route(Method::post, "/echo", [](const HttpServer::Request& request) {
        Response response = Response::text(200, request.body);
        response.content_type = std::string{request.headers.get("Content-Type").value_or("none")};
        response.headers.add("X-Echo-Header", std::string{request.headers.get("x-custom").value_or("")});
        return response;
    });
    server.route(Method::get, "/items/:id", [](const HttpServer::Request& request) {
        return Response::text(200, std::format("{}|{}", request.path_param("id").value_or("?"),
                                               request.query_param("q").value_or("?")));
    });
    server.route_streaming(
        Method::put, "/blobs/:name", [](const HttpServer::Request& request, HttpServer::BodyReader& body) {
            Blake3Hasher hasher;
            std::size_t bytes = 0;
            const auto read = body.read([&](std::string_view chunk) -> Result<void> {
                hasher.update(chunk);
                bytes += chunk.size();
                return ok();
            });
            if (!read.has_value()) {
                return Response::error(read.error());
            }
            return Response::json(
                201, std::format(R"({{"name":"{}","bytes":{},"blake3":"{}"}})",
                                 request.path_param("name").value_or(""), bytes, hasher.finalize()));
        });
    server.route(Method::get, "/big",
                 [](const auto&) { return Response::text(200, std::string(2 << 20, 'b')); });
    server.route(Method::get, "/error", [](const auto&) {
        return Response::error(
            Error{Errc::integrity, "hash mismatch"}.with_context("while storing").with_retry_after(2000ms));
    });
    server.route(Method::get, "/plain-429", [](const auto&) { return Response::text(429, "slow down"); });
    server.route(Method::get, "/throw",
                 [](const auto&) -> Response { throw std::runtime_error("handler bug"); });
    server.route(Method::get, "/flaky", [&state](const auto&) {
        if (++state.flaky_calls < 3) {
            Response busy = Response::error(Error{Errc::unavailable, "warming up"});
            busy.headers.set("Retry-After", "0");
            return busy;
        }
        return Response::text(200, "ready");
    });
    server.route(Method::get, "/sse", [](const auto&) {
        return Response::event_stream([](HttpServer::ChunkWriter& writer) -> Result<void> {
            for (int index = 0; index < 5; ++index) {
                if (!writer.write(format_sse_event(std::format(R"({{"n":{}}})", index)))) {
                    return fail(Errc::cancelled, "the client went away");
                }
                std::this_thread::sleep_for(60ms);
            }
            if (!writer.write(format_sse_event("[DONE]"))) {
                return fail(Errc::cancelled, "the client went away");
            }
            return ok();
        });
    });
    server.route(Method::get, "/slow-sse", [&state](const auto&) {
        return Response::event_stream([&state](HttpServer::ChunkWriter& writer) -> Result<void> {
            const ScopeExit ended{[&state] { state.slow_stream_ended = true; }};
            (void)writer.write(format_sse_event("first"));
            while (!writer.stopping()) {
                std::this_thread::sleep_for(20ms);
                if (!writer.write(format_sse_comment("keep-alive"))) {
                    return fail(Errc::cancelled, "the client went away");
                }
            }
            return ok();  // an endless stream, closed by the server: complete as far as it goes
        });
    });
    server.route(Method::get, "/stall", [](const auto&) {
        return Response::event_stream([](HttpServer::ChunkWriter& writer) -> Result<void> {
            (void)writer.write(format_sse_event("only one"));
            for (int tick = 0; tick < 300 && !writer.stopping(); ++tick) {
                std::this_thread::sleep_for(10ms);  // silence: the client's idle timeout fires
            }
            return ok();
        });
    });
    server.route(Method::get, "/fail-midway", [](const auto&) {
        return Response::event_stream([](HttpServer::ChunkWriter& writer) -> Result<void> {
            (void)writer.write(format_sse_event("first"));
            return fail(Errc::network, "the upstream broke off");
        });
    });
    server.route(Method::get, "/throw-midway", [](const auto&) {
        return Response::event_stream([](HttpServer::ChunkWriter& writer) -> Result<void> {
            (void)writer.write(format_sse_event("first"));
            throw std::runtime_error("producer bug");
        });
    });
    server.route(Method::get, "/head-first", [&state](const auto&) {
        // Sends no body byte until the client has seen the head (or 5 s
        // passed), then says which came first.
        return Response::event_stream([&state](HttpServer::ChunkWriter& writer) -> Result<void> {
            const auto deadline = Clock::now() + 5s;
            while (!state.head_seen && Clock::now() < deadline) {
                std::this_thread::sleep_for(5ms);
            }
            if (!writer.write(
                    format_sse_event(state.head_seen ? "head came first" : "head waited for the body"))) {
                return fail(Errc::cancelled, "the client went away");
            }
            return ok();
        });
    });
    server.route(Method::get, "/unheeding-sse", [&state](const auto&) {
        // Runs until the test lets it end, never asking stopping().
        return Response::event_stream([&state](HttpServer::ChunkWriter& writer) -> Result<void> {
            if (!writer.write(format_sse_event("first"))) {
                return fail(Errc::cancelled, "the client went away");
            }
            const auto deadline = Clock::now() + 5s;
            while (!state.unheeding_released && Clock::now() < deadline) {
                std::this_thread::sleep_for(5ms);
            }
            if (!writer.write(format_sse_event("last"))) {
                return fail(Errc::cancelled, "the client went away");
            }
            return ok();
        });
    });
    server.route(Method::get, "/stream-after-leave", [&state](const HttpServer::Request& request) {
        // Answers with a stream once its client has left: its body never begins.
        state.waiting_to_stream = true;
        const auto deadline = Clock::now() + 5s;
        while (!(request.client_gone && request.client_gone()) && Clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        return Response::event_stream([](HttpServer::ChunkWriter& writer) -> Result<void> {
            if (!writer.write(format_sse_event("never read"))) {
                return fail(Errc::cancelled, "the client went away");
            }
            return ok();
        });
    });
    server.route(Method::get, "/heavy-head", [](const auto&) {
        // A stream whose head is too large to leave at once: answered, and
        // its body not begun, until its client reads (SlowReader).
        Response stream = Response::event_stream([](HttpServer::ChunkWriter& writer) -> Result<void> {
            if (!writer.write(format_sse_event(writer.stopping() ? "stopping" : "serving"))) {
                return fail(Errc::cancelled, "the client went away");
            }
            return ok();
        });
        stream.headers.add("X-Padding", std::string(kHeavyHead, 'p'));
        return stream;
    });
    server.route(Method::get, "/wait-for-leave", [&state](const HttpServer::Request& request) {
        state.waiting_for_leave = true;
        const auto deadline = Clock::now() + 5s;
        while (Clock::now() < deadline) {
            if (request.client_gone && request.client_gone()) {
                state.noticed_leave = true;
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        return Response::text(200, "done waiting");
    });
    server.route(Method::get, "/accept-encoding", [](const HttpServer::Request& request) {
        return Response::text(200, std::string{request.headers.get("Accept-Encoding").value_or("(none)")});
    });
    server.route(Method::get, "/redirect", [](const auto&) {
        Response moved = Response::empty(302);
        moved.headers.add("Location", "/hello");
        return moved;
    });
    server.route(Method::get, "/private/data", [](const auto&) { return Response::text(200, "classified"); });
    server.route_streaming(Method::put, "/private/upload",
                           [&state](const HttpServer::Request&, HttpServer::BodyReader&) {
                               ++state.private_uploads_run;
                               return Response::empty(204);
                           });
    server.set_guard([](const HttpServer::Request& request) -> std::optional<Response> {
        if (!request.path.starts_with("/private") || request.headers.get("Authorization") == kToken) {
            return std::nullopt;  // let it through
        }
        Response denied = Response::error(Error{Errc::permission_denied, "missing or wrong token"});
        denied.status = 401;  // not authenticated (403 would mean: known, but not allowed)
        return denied;
    });
    server.set_access_log([&state](const HttpServer::AccessLogEntry& entry) {
        const std::lock_guard lock(state.log_mutex);
        state.log.push_back(entry);
        ++state.logged;
    });
}

// ---------------------------------------------------------------------------
// Requests and responses
// ---------------------------------------------------------------------------
TEST_F(ClientServer, GetsATextResponse) {
    const auto response = unwrap(get("/hello"));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "hello");
    EXPECT_THAT(std::string{response.headers.get("content-type").value_or("")}, HasSubstr("text/plain"));
}

TEST_F(ClientServer, HeadAnswersWithoutABody) {
    const auto response = unwrap(client().send({.method = Method::head, .url = url("/hello")}));
    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.body.empty());
}

TEST_F(ClientServer, PostsBodiesAndHeaders) {
    const auto response =
        unwrap(client().send({.method = Method::post,
                              .url = url("/echo"),
                              .headers = {{"Content-Type", "application/json"}, {"X-Custom", "42"}},
                              .body = R"({"hello":"world"})"}));
    EXPECT_EQ(response.body, R"({"hello":"world"})");
    EXPECT_EQ(response.headers.get("Content-Type"), "application/json");
    EXPECT_EQ(response.headers.get("x-echo-header"), "42");
}

TEST_F(ClientServer, PassesPathAndQueryParameters) {
    const auto response = unwrap(get(std::format("/items/abc123?q={}", url_encode("a b&c"))));
    EXPECT_EQ(response.body, "abc123|a b&c");
}

TEST_F(ClientServer, StreamsALargeUploadFromAFile) {
    const testkit::TempDir dir{"rr_net_test"};
    const std::string payload = random_payload((std::size_t{5} << 20U) + 123);
    const auto file = dir.write_file("audio.opus", payload);
    const auto response = unwrap(expect_success(
        client().send({.method = Method::put, .url = url("/blobs/audio"), .body_file = file})));
    EXPECT_EQ(response.status, 201);
    EXPECT_EQ(response.body,
              std::format(R"({{"name":"audio","bytes":{},"blake3":"{}"}})", payload.size(), blake3(payload)));
}

TEST_F(ClientServer, ReceivesLargeBodiesAndEnforcesTheCap) {
    EXPECT_EQ(unwrap(get("/big")).body.size(), std::size_t{2} << 20U);
    const HttpClient small{HttpClient::Options{.max_response_bytes = 1000}};
    EXPECT_ERRC(small.send({.url = url("/big")}), Errc::io);
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
TEST_F(ClientServer, StructuredErrorsArriveIntact) {
    const auto response = unwrap(get("/error"));
    EXPECT_EQ(response.status, 422);
    const auto result = expect_success(response);
    ASSERT_ERRC(result, Errc::integrity);
    EXPECT_EQ(result.error().message(), "hash mismatch");
    EXPECT_EQ(result.error().context().front(), "while storing");
    EXPECT_EQ(result.error().retry_after(), 2000ms);
}

TEST_F(ClientServer, PlainErrorStatusesAreClassified) {
    EXPECT_ERRC(expect_success(get("/plain-429")), Errc::rate_limited);
    EXPECT_ERRC(expect_success(get("/no/such/route")), Errc::not_found);
}

TEST_F(ClientServer, AThrowingHandlerIsA500NotACrash) {
    const auto result = expect_success(get("/throw"));
    ASSERT_ERRC(result, Errc::internal);
    EXPECT_THAT(result.error().message(), HasSubstr("handler bug"));
    EXPECT_EQ(unwrap(get("/hello")).status, 200) << "the server keeps serving";
}

TEST_F(ClientServer, TransportFailuresAreErrors) {
    HttpServer closed{HttpServer::Options{}};
    ASSERT_OK(closed.start());
    const int port = closed.port();
    closed.stop();
    EXPECT_ERRC(client().send({.url = std::format("http://127.0.0.1:{}/", port)}), Errc::network);
    EXPECT_ERRC(client().send({.url = "notaurl://x"}), Errc::invalid_argument);
    EXPECT_ERRC(client().send({.url = "file:///etc/passwd"}), Errc::invalid_argument) << "http(s) only";
}

TEST_F(ClientServer, RetriesUntilTheServerIsReady) {
    const auto result =
        with_retries(RetryPolicy{.max_attempts = 5}, [&](int) { return expect_success(get("/flaky")); });
    EXPECT_EQ(unwrap(result).body, "ready");
    EXPECT_EQ(state().flaky_calls.load(), 3);
}

// ---------------------------------------------------------------------------
// The guard and body limits
// ---------------------------------------------------------------------------
TEST_F(ClientServer, TheGuardRejectsBeforeAnyHandlerRuns) {
    EXPECT_ERRC(expect_success(get("/private/data")), Errc::permission_denied);
    EXPECT_EQ(unwrap(get("/private/data")).status, 401);
    EXPECT_EQ(unwrap(get("/private/data", {{"Authorization", std::string{kToken}}})).body, "classified");

    EXPECT_ERRC(
        expect_success(client().send({.method = Method::put, .url = url("/private/upload"), .body = "x"})),
        Errc::permission_denied);
    EXPECT_EQ(state().private_uploads_run.load(), 0) << "the streaming handler never ran";
}

TEST(HttpServerLimits, BufferedRoutesRefuseOversizedBodiesBeforeReadingThem) {
    HttpServer server{HttpServer::Options{.max_body_bytes = 1024}};
    std::atomic<int> handled{0};
    server.route(Method::post, "/small", [&](const auto&) {
        ++handled;
        return Response::empty(204);
    });
    ASSERT_OK(server.start());
    const HttpClient client;
    const std::string url = std::format("http://127.0.0.1:{}/small", server.port());
    const auto too_big =
        unwrap(client.send({.method = Method::post, .url = url, .body = std::string(2048, 'x')}));
    EXPECT_EQ(too_big.status, 413);
    EXPECT_EQ(handled.load(), 0);
    EXPECT_EQ(unwrap(client.send({.method = Method::post, .url = url, .body = "ok"})).status, 204);
}

TEST(HttpServerLimits, SimultaneousConnectionsNeverWaitForKeepAliveTimeouts) {
    // Each open connection holds a worker for its whole life. With a pool
    // that fails to grow when many connections arrive at once (httplib's
    // own), the surplus waits until some keep-alive connection times out:
    // 5 s here. Released together by a latch, 16 clients against 2 workers
    // provoke exactly that race.
    HttpServer server{
        HttpServer::Options{.worker_threads = 2, .keep_alive_timeout = std::chrono::seconds{5}}};
    server.route(Method::get, "/ping", [](const auto&) { return Response::text(200, "pong"); });
    ASSERT_OK(server.start());
    const HttpClient client;
    const std::string url = std::format("http://127.0.0.1:{}/ping", server.port());
    constexpr int kClients = 16;
    std::latch go{kClients};
    std::atomic<int> failures{0};
    const auto started = Clock::now();
    {
        std::vector<std::jthread> clients;
        clients.reserve(kClients);
        for (int index = 0; index < kClients; ++index) {
            clients.emplace_back([&] {
                go.arrive_and_wait();
                for (int request = 0; request < 5; ++request) {
                    if (!client.send({.url = url}).has_value()) {
                        ++failures;
                    }
                }
            });
        }
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_LT(Clock::now() - started, 2500ms) << "some connection waited for a keep-alive timeout";
}

TEST(HttpServerLimits, APortInUseIsReported) {
    HttpServer first{HttpServer::Options{}};
    ASSERT_OK(first.start());
    HttpServer second{HttpServer::Options{.port = first.port()}};
    EXPECT_ERRC(second.start(), Errc::io);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------
TEST_F(ClientServer, SseEventsArriveAsTheyAreProducedNotAtTheEnd) {
    SseParser parser;
    std::vector<std::string> data;
    std::optional<Clock::time_point> first_event;
    int status_seen = 0;
    const auto started = Clock::now();
    const auto response = unwrap(expect_success(
        client().stream({.url = url("/sse"), .idle_timeout = 5s},
                        {.on_head = [&](int status, const Headers&) -> Result<void> {
                             status_seen = status;
                             return ok();
                         },
                         .on_chunk =
                             [&](std::string_view chunk) {
                                 return parser.feed(chunk, [&](const SseEvent& event) -> Result<void> {
                                     first_event = first_event.value_or(Clock::now());
                                     data.push_back(event.data);
                                     return ok();
                                 });
                             }})));
    const auto finished = Clock::now();
    EXPECT_EQ(status_seen, 200);
    EXPECT_EQ(response.headers.get("Content-Type"), "text/event-stream");
    EXPECT_EQ(data, (std::vector<std::string>{R"({"n":0})", R"({"n":1})", R"({"n":2})", R"({"n":3})",
                                              R"({"n":4})", "[DONE]"}));
    ASSERT_TRUE(first_event.has_value());
    // The producer sleeps 60 ms between events: a buffered transfer would
    // deliver the first event only at the end, ~300 ms after it was sent.
    EXPECT_LT(*first_event - started, finished - started - 150ms);
}

TEST_F(ClientServer, CancellingAStreamIsInstantAndTheServerNotices) {
    // httplib's Server constructor sets SIGPIPE to SIG_IGN for the whole
    // process. Restore the default here, so this test proves that our own
    // MSG_NOSIGNAL keeps a disconnecting client from killing the server.
    const auto previous_sigpipe = std::signal(SIGPIPE, SIG_DFL);
    const ScopeExit restore_sigpipe{[previous_sigpipe] { (void)std::signal(SIGPIPE, previous_sigpipe); }};
    std::stop_source cancel;
    const auto started = Clock::now();
    const auto result =
        client().stream({.url = url("/slow-sse")}, {.on_chunk = [&](std::string_view) -> Result<void> {
                            cancel.request_stop();  // stop as soon as anything arrives
                            return ok();
                        }},
                        cancel.get_token());
    EXPECT_ERRC(result, Errc::cancelled);
    EXPECT_LT(Clock::now() - started, 1s) << "the stop wakes the transfer; no waiting for a progress tick";
    // The server's next write fails (MSG_NOSIGNAL: no SIGPIPE) and its producer ends.
    const auto deadline = Clock::now() + 5s;
    while (!state().slow_stream_ended && Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(state().slow_stream_ended.load());
    EXPECT_EQ(unwrap(get("/hello")).status, 200) << "the server is alive";
}

TEST_F(ClientServer, CancellationWakesEvenASilentTransfer) {
    // /stall goes quiet after one event, so nothing but the stop request can
    // end the wait; without curl_multi_wakeup() the transfer would notice only
    // at its next poll timeout (up to a second).
    std::stop_source cancel;
    std::latch first_chunk{1};
    Clock::time_point stop_requested_at{};  // written by the canceller, read after join()
    std::jthread canceller{[&] {
        first_chunk.wait();
        std::this_thread::sleep_for(50ms);  // let the transfer settle into its poll
        stop_requested_at = Clock::now();
        cancel.request_stop();
    }};
    bool signalled = false;
    const auto result =
        client().stream({.url = url("/stall")}, {.on_chunk = [&](std::string_view) -> Result<void> {
                            if (!signalled) {
                                signalled = true;
                                first_chunk.count_down();
                            }
                            return ok();
                        }},
                        cancel.get_token());
    const auto returned_at = Clock::now();
    if (!signalled) {
        first_chunk.count_down();  // never leave the canceller waiting
    }
    canceller.join();
    EXPECT_ERRC(result, Errc::cancelled);
    EXPECT_LT(returned_at - stop_requested_at, 300ms);
}

TEST_F(ClientServer, AnIdleStreamTimesOut) {
    const auto started = Clock::now();
    EXPECT_ERRC(client().stream({.url = url("/stall"), .idle_timeout = 300ms}, {}), Errc::timeout);
    EXPECT_LT(Clock::now() - started, 2500ms);
}

TEST_F(ClientServer, HandlerFailuresAbortTheStream) {
    const auto rejected =
        client().stream({.url = url("/sse")}, {.on_chunk = [](std::string_view) -> Result<void> {
                            return fail(Errc::model_output_invalid, "bad token");
                        }});
    EXPECT_ERRC(rejected, Errc::model_output_invalid);

    const auto thrown = client().stream(
        {.url = url("/sse")},
        {.on_chunk = [](std::string_view) -> Result<void> { throw std::runtime_error("oops"); }});
    ASSERT_ERRC(thrown, Errc::internal);
    EXPECT_THAT(thrown.error().message(), HasSubstr("oops"));
}

TEST_F(ClientServer, ErrorBodiesOfStreamsAreBufferedNotStreamed) {
    int chunks = 0;
    const auto response =
        unwrap(client().stream({.url = url("/error")}, {.on_chunk = [&](std::string_view) -> Result<void> {
                                   ++chunks;
                                   return ok();
                               }}));
    EXPECT_EQ(chunks, 0);
    EXPECT_ERRC(expect_success(response), Errc::integrity);
}

// A producer that cannot finish must not produce a short body that looks
// complete: the client sees what was sent, then a truncated transfer.
TEST_F(ClientServer, AFailingProducerTruncatesTheBody) {
    for (const std::string_view path : {"/fail-midway", "/throw-midway"}) {
        SCOPED_TRACE(path);
        std::string received;
        const auto result =
            client().stream({.url = url(path)}, {.on_chunk = [&](std::string_view chunk) -> Result<void> {
                                received.append(chunk);
                                return ok();
                            }});
        EXPECT_EQ(received, format_sse_event("first")) << "what was sent before the failure arrives";
        EXPECT_ERRC(result, Errc::network) << "truncated, so the client retries instead of trusting it";
        EXPECT_EQ(unwrap(get("/hello")).status, 200) << "the server carries on";
    }
}

// A server that answers 200 and then thinks for a long time (a model reading
// its prompt) has still answered: the head must not wait for the body.
TEST_F(ClientServer, TheHeadArrivesBeforeTheFirstBodyByte) {
    std::vector<std::string> events;
    SseParser parser;
    const auto response =
        client().stream({.url = url("/head-first")},
                        {.on_head = [&](int status, const Headers&) -> Result<void> {
                             EXPECT_EQ(status, 200);
                             state().head_seen = true;  // releases the producer
                             return ok();
                         },
                         .on_chunk =
                             [&](std::string_view chunk) {
                                 return parser.feed(chunk, [&](const SseEvent& event) -> Result<void> {
                                     events.push_back(event.data);
                                     return ok();
                                 });
                             }});
    ASSERT_OK(expect_success(response));
    EXPECT_EQ(events, std::vector<std::string>{"head came first"});
}

TEST_F(ClientServer, TheHeadIsTheFinalResponses) {
    std::vector<int> heads;
    const HttpClient::StreamHandlers record{.on_head = [&](int status, const Headers&) -> Result<void> {
        heads.push_back(status);
        return ok();
    }};
    EXPECT_EQ(unwrap(client().stream({.url = url("/redirect"), .follow_redirects = true}, record)).status,
              200);
    EXPECT_EQ(heads, std::vector<int>{200}) << "a redirect that is followed is not the answer";
    heads.clear();
    EXPECT_EQ(unwrap(client().stream({.url = url("/redirect")}, record)).status, 302);
    EXPECT_EQ(heads, std::vector<int>{302});
}

TEST_F(ClientServer, HandlersCanSeeTheirClientLeave) {
    std::stop_source cancel;
    std::jthread canceller{[&] {
        const auto deadline = Clock::now() + 5s;
        while (!state().waiting_for_leave && Clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        cancel.request_stop();
    }};
    EXPECT_ERRC(client().send({.url = url("/wait-for-leave")}, cancel.get_token()), Errc::cancelled);
    canceller.join();
    const auto deadline = Clock::now() + 5s;
    while (!state().noticed_leave && Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(state().noticed_leave.load()) << "Request::client_gone reports the closed connection";
}

TEST_F(ClientServer, CompressionCanBeDeclinedForByteExactDownloads) {
    EXPECT_NE(unwrap(get("/accept-encoding")).body, "(none)")
        << "compressed encodings are accepted by default";
    EXPECT_EQ(unwrap(client().send({.url = url("/accept-encoding"), .accept_compression = false})).body,
              "(none)");
}

TEST_F(ClientServer, StopFinishesPromptlyWhileAStreamIsOpen) {
    std::latch streaming{1};
    const std::jthread reader{[&] {
        bool counted = false;  // a latch must not be counted below zero
        (void)client().stream({.url = url("/slow-sse")}, {.on_chunk = [&](std::string_view) -> Result<void> {
                                  if (!counted) {
                                      counted = true;
                                      streaming.count_down();
                                  }
                                  return ok();
                              }});
    }};
    streaming.wait();
    const auto started = Clock::now();
    server().stop();
    EXPECT_LT(Clock::now() - started, 3s);
    EXPECT_TRUE(state().slow_stream_ended.load()) << "the producer saw stopping()";
}

// httplib begins no streamed body once its listening socket is closed. A
// stream answered just before the stop, its head on its way and its producer
// not yet called, must still end whole, with its producer's last word: its
// client would take a truncated transfer for a broken one. Here the head is
// held on its way by a client that reads nothing of a head larger than the
// connection holds. A stream asked for meanwhile is refused, whole too.
TEST_F(ClientServer, AStreamAnsweredBeforeTheStopEndsWhole) {
    SlowReader heavy{server().port(), "/heavy-head"};
    EXPECT_EQ(heavy.read(1), "H") << "the head is on its way: the stream is answered";
    std::atomic<bool> stopped{false};
    std::jthread stopper{[&] {
        server().stop();
        stopped = true;
    }};
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(stopped.load()) << "the stop waits for the stream to begin (or the head was not held: is "
                                    "kHeavyHead more than the connection holds?)";
    EXPECT_ERRC(expect_success(get("/sse")), Errc::unavailable) << "a stream asked for while stopping";
    const std::string rest = heavy.read_to_end();
    stopper.join();
    const std::size_t head_end = rest.find("\r\n\r\n");
    ASSERT_NE(head_end, std::string::npos) << "the head arrives whole";
    const std::string event = format_sse_event("stopping");
    EXPECT_EQ(rest.substr(head_end + 4), std::format("{:x}\r\n{}\r\n0\r\n\r\n", event.size(), event))
        << "the producer's last word arrives, in a body that ends whole";
}

// A stream already begun needs the door open no longer: the stop closes it
// at once, however long the stream still runs, and the stream runs its course.
TEST_F(ClientServer, TheStopClosesTheDoorOnceTheStreamsAnsweredHaveBegun) {
    std::latch begun{1};
    std::string received;
    std::optional<Result<HttpClient::Response>> streamed;
    std::jthread reader{[&] {
        bool counted = false;  // a latch must not be counted below zero
        streamed = client().stream({.url = url("/unheeding-sse")},
                                   {.on_chunk = [&](std::string_view chunk) -> Result<void> {
                                       received.append(chunk);
                                       if (!counted) {
                                           counted = true;
                                           begun.count_down();
                                       }
                                       return ok();
                                   }});
    }};
    begun.wait();
    const int port = server().port();
    std::jthread stopper{[&] { server().stop(); }};
    const auto deadline = Clock::now() + 2s;
    while (testkit::can_connect(port) && Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(testkit::can_connect(port)) << "the door closes while the stream runs its course";
    state().unheeding_released = true;
    stopper.join();
    reader.join();
    ASSERT_TRUE(streamed.has_value());
    ASSERT_OK(expect_success(*streamed));
    EXPECT_EQ(received, format_sse_event("first") + format_sse_event("last"));
}

// A stream whose client left before its body began is let go of unbegun:
// it holds up no stop.
TEST_F(ClientServer, AStreamWhoseClientLeftHoldsUpNoStop) {
    std::stop_source cancel;
    std::jthread canceller{[&] {
        const auto deadline = Clock::now() + 5s;
        while (!state().waiting_to_stream && Clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        cancel.request_stop();
    }};
    EXPECT_ERRC(client().send({.url = url("/stream-after-leave")}, cancel.get_token()), Errc::cancelled);
    canceller.join();
    const auto answered = [&] {  // httplib logs a response once it is answered, before its head goes out
        const std::lock_guard lock(state().log_mutex);
        return std::ranges::any_of(state().log, [](const HttpServer::AccessLogEntry& entry) {
            return entry.path == "/stream-after-leave";
        });
    };
    const auto deadline = Clock::now() + 5s;
    while (!answered() && Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(answered());
    const auto started = Clock::now();
    server().stop();
    EXPECT_LT(Clock::now() - started, 2s) << "nothing waits for a body that will never begin";
}

TEST_F(ClientServer, AServerStartedAgainStreamsAgain) {
    server().stop();
    ASSERT_OK(server().start());
    std::string received;
    ASSERT_OK(expect_success(
        client().stream({.url = url("/sse")}, {.on_chunk = [&](std::string_view chunk) -> Result<void> {
                            received.append(chunk);
                            return ok();
                        }})));
    EXPECT_THAT(received, HasSubstr(format_sse_event("[DONE]"))) << "its streams are not refused";
}

// ---------------------------------------------------------------------------
// Concurrency and logging
// ---------------------------------------------------------------------------
TEST_F(ClientServer, ServesManyConcurrentRequestsFromOneClient) {
    constexpr int kThreads = 12;
    constexpr int kRequests = 20;
    std::atomic<int> failures{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([&, thread] {
                for (int request = 0; request < kRequests; ++request) {
                    const auto body = std::format("{}-{}", thread, request);
                    const auto echoed =
                        client().send({.method = Method::post, .url = url("/echo"), .body = body});
                    if (!echoed.has_value() || echoed->body != body) {
                        ++failures;
                    }
                }
            });
        }
    }
    EXPECT_EQ(failures.load(), 0);
    const auto deadline = Clock::now() + 2s;  // the log entry is written after the response
    while (state().logged.load() < kThreads * kRequests && Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(state().logged.load(), kThreads * kRequests);
}

TEST_F(ClientServer, TheAccessLogRecordsEachRequest) {
    (void)get("/hello");
    (void)get("/private/data");
    const auto deadline = Clock::now() + 2s;
    while (state().logged.load() < 2 && Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    const std::lock_guard lock(state().log_mutex);
    ASSERT_EQ(state().log.size(), 2U);
    EXPECT_EQ(state().log[0].method, "GET");
    EXPECT_EQ(state().log[0].path, "/hello");
    EXPECT_EQ(state().log[0].status, 200);
    EXPECT_EQ(state().log[0].remote_address, "127.0.0.1");
    EXPECT_EQ(state().log[1].status, 401);
}

}  // namespace
}  // namespace rr::net
