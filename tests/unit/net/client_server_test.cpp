// End-to-end tests: a real HttpServer on an ephemeral loopback port, driven
// by the real HttpClient.
#include <atomic>
#include <chrono>
#include <csignal>
#include <format>
#include <latch>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/hash.hpp>
#include <rr/core/scope_exit.hpp>
#include <rr/net/http_client.hpp>
#include <rr/net/http_server.hpp>
#include <rr/net/retry.hpp>
#include <rr/net/sse.hpp>
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

// State the routes share with the tests.
struct ServerState {
    std::atomic<int> private_uploads_run{0};
    std::atomic<int> flaky_calls{0};
    std::atomic<bool> slow_stream_ended{false};
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
        return Response::event_stream([](HttpServer::ChunkWriter& writer) {
            for (int index = 0; index < 5; ++index) {
                if (!writer.write(format_sse_event(std::format(R"({{"n":{}}})", index)))) {
                    return;
                }
                std::this_thread::sleep_for(60ms);
            }
            (void)writer.write(format_sse_event("[DONE]"));
        });
    });
    server.route(Method::get, "/slow-sse", [&state](const auto&) {
        return Response::event_stream([&state](HttpServer::ChunkWriter& writer) {
            (void)writer.write(format_sse_event("first"));
            while (!writer.stopping()) {
                std::this_thread::sleep_for(20ms);
                if (!writer.write(format_sse_comment("keep-alive"))) {
                    break;  // the client went away
                }
            }
            state.slow_stream_ended = true;
        });
    });
    server.route(Method::get, "/stall", [](const auto&) {
        return Response::event_stream([](HttpServer::ChunkWriter& writer) {
            (void)writer.write(format_sse_event("only one"));
            for (int tick = 0; tick < 300 && !writer.stopping(); ++tick) {
                std::this_thread::sleep_for(10ms);  // silence: the client's idle timeout fires
            }
        });
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

TEST_F(ClientServer, StopFinishesPromptlyWhileAStreamIsOpen) {
    std::latch streaming{1};
    const std::jthread reader{[&] {
        (void)client().stream({.url = url("/slow-sse")}, {.on_chunk = [&](std::string_view) -> Result<void> {
                                  streaming.count_down();
                                  return ok();
                              }});
    }};
    streaming.wait();
    const auto started = Clock::now();
    server().stop();
    EXPECT_LT(Clock::now() - started, 3s);
    EXPECT_TRUE(state().slow_stream_ended.load()) << "the producer saw stopping()";
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
