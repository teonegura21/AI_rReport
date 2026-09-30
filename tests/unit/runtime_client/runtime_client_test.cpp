// The hub's runtime client against a real Supervisor with rr_fake_engine
// processes behind it: the protocol both sides implement, end to end. A
// hand-written server stands in where the real one cannot misbehave on cue.
#include <rr/runtime_client/runtime_client.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/core/hash.hpp>
#include <rr/json/json.hpp>
#include <rr/net/http_server.hpp>
#include <rr/supervisor/supervisor.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "test_runtime.hpp"

namespace rr::runtime_client {
namespace {

using namespace std::chrono_literals;
using runtime_api::EngineState;
using ::testing::HasSubstr;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;
using testing::Runtime;

const net::RetryPolicy kQuickRetries{.max_attempts = 3, .initial_delay = 10ms, .max_delay = 50ms};

TEST(RuntimeClient, WaitsForTheEnginesThenReadsTheirHealth) {
    const Runtime runtime{R"("--fake-load-ms", "400")"};
    const net::HttpClient http;
    const RuntimeClient client{http, runtime.endpoint(), kQuickRetries};
    EXPECT_FALSE(client.endpoint().url.ends_with('/')) << "the endpoint's trailing slash is trimmed";
    ASSERT_OK(client.ping());

    const auto started = Clock::now();
    const runtime_api::HealthReport ready = unwrap(client.wait_until_ready(20s, {}, 50ms));
    EXPECT_GE(Clock::now() - started, 300ms) << "the engine took 400 ms to load";
    EXPECT_EQ(ready.profile, "test");
    EXPECT_EQ(ready.version, "test-version");
    ASSERT_EQ(ready.engines.size(), 2U);
    EXPECT_EQ(ready.engines[0].state, EngineState::ready);
    ASSERT_EQ(ready.gpus.size(), 1U);
    EXPECT_EQ(ready.gpus[0].name, "Fake GPU");

    const auto models = unwrap(client.models());
    ASSERT_EQ(models.size(), 2U);
    EXPECT_EQ(models[0], (ModelInfo{.id = "chat", .role = "commander", .state = "ready"}));
    EXPECT_EQ(models[1].id, "embed");
}

TEST(RuntimeClient, AWrongTokenEndsTheWaitAtOnce) {
    const Runtime runtime;
    const net::HttpClient http;
    const RuntimeClient client{http, runtime.endpoint("not-the-token"), kQuickRetries};
    ASSERT_OK(client.ping()) << "ping needs no token";
    EXPECT_ERRC(client.health(), Errc::permission_denied);

    const auto started = Clock::now();
    EXPECT_ERRC(client.wait_until_ready(20s, {}, 50ms), Errc::permission_denied);
    EXPECT_LT(Clock::now() - started, 5s) << "waiting cannot fix a wrong token";
}

TEST(RuntimeClient, GivesUpWaitingAndSaysWhatItSaw) {
    const Runtime runtime{R"("--fake-never-ready")"};
    const net::HttpClient http;
    const RuntimeClient client{http, runtime.endpoint(), kQuickRetries};
    const auto waited = client.wait_until_ready(600ms, {}, 50ms);
    ASSERT_ERRC(waited, Errc::timeout);
    EXPECT_THAT(waited.error().message(), HasSubstr("chat starting"));
    EXPECT_THAT(waited.error().message(), HasSubstr("embed ready"));

    // Non-const: request_stop() is a non-const member in the C++ standard.
    std::stop_source stop;  // NOLINT(misc-const-correctness)
    stop.request_stop();
    EXPECT_ERRC(client.wait_until_ready(20s, stop.get_token(), 50ms), Errc::cancelled);
}

TEST(RuntimeClient, HeartbeatsKeepThePodAwake) {
    Runtime runtime;
    const net::HttpClient http;
    const RuntimeClient client{http, runtime.endpoint(), kQuickRetries};
    runtime.clock().advance(10min);
    EXPECT_GE(unwrap(client.health()).idle_s, 600);
    ASSERT_OK(client.heartbeat());
    EXPECT_EQ(unwrap(client.health()).idle_s, 0);
}

TEST(RuntimeClient, UploadsBlobsOnceAndTheRuntimeChecksThem) {
    const Runtime runtime;
    const net::HttpClient http;
    const RuntimeClient client{http, runtime.endpoint(), kQuickRetries};
    const std::string frame = "a JPEG frame, supposedly";
    const auto file = runtime.dir().write_file("frame.jpg", frame);
    const Blake3Digest hash = blake3(frame);

    EXPECT_FALSE(unwrap(client.has_blob("lib", hash))) << "HEAD answers 404 for a missing blob";
    ASSERT_OK(client.put_blob("lib", hash, file));
    EXPECT_TRUE(unwrap(client.has_blob("lib", hash)));
    EXPECT_FALSE(unwrap(client.has_blob("other-lib", hash))) << "libraries are apart";

    EXPECT_ERRC(client.put_blob("lib", blake3("something else"), file), Errc::integrity)
        << "a blob is named by its content's hash";
    EXPECT_ERRC(client.has_blob("../escape", hash), Errc::invalid_argument);
}

// Model calls through the runtime's relay to its engines, as the hub makes them.
TEST(RuntimeClient, ChatsWithTheEnginesThroughTheRelay) {
    const Runtime runtime;
    const net::HttpClient http;
    ASSERT_OK(RuntimeClient(http, runtime.endpoint()).wait_until_ready(20s, {}, 50ms));
    const auto provider = runtime_provider(http, runtime.endpoint());
    EXPECT_EQ(provider->name(), "runtime");

    std::string streamed;
    const llm::ChatResponse answer =
        unwrap(provider->chat({.model = "chat", .messages = {llm::Message::user("Say five tokens.")}},
                              {.on_content = [&](std::string_view text) { streamed += text; }}, {}));
    EXPECT_EQ(answer.content, "tok0tok1tok2tok3tok4");
    EXPECT_EQ(streamed, answer.content);
    EXPECT_EQ(answer.finish, llm::FinishReason::stop);
    EXPECT_EQ(answer.model, "chat");
    EXPECT_EQ(answer.usage.prompt_tokens, 4) << "16 characters of prompt";
    EXPECT_EQ(answer.usage.completion_tokens, 5);
    EXPECT_EQ(answer.generation_tokens_per_second, 50.0) << "the engine's own timings came through";

    const llm::ChatResponse thought =
        unwrap(provider->chat({.model = "chat",
                               .messages = {llm::Message::user("?")},
                               .extra = {{"reasoning", "Let me think."}, {"reply", "Done."}}},
                              {}, {}));
    EXPECT_EQ(thought.reasoning, "Let me think.");
    EXPECT_EQ(thought.content, "Done.");
}

TEST(RuntimeClient, FailuresBehindTheRelayKeepTheirMeaning) {
    const Runtime runtime;
    const net::HttpClient http;
    ASSERT_OK(RuntimeClient(http, runtime.endpoint()).wait_until_ready(20s, {}, 50ms));
    const auto provider = runtime_provider(http, runtime.endpoint());
    const auto ask = [&](json::Json knobs) {
        return provider->chat(
            {.model = "chat", .messages = {llm::Message::user("hi")}, .extra = std::move(knobs)}, {}, {});
    };
    EXPECT_ERRC(ask({{"fail", true}}), Errc::invalid_argument) << "the engine's 400";
    EXPECT_ERRC(ask({{"break_after", 2}}), Errc::network) << "an engine that died mid-answer: retried";
    EXPECT_ERRC(ask({{"error_after", 2}}), Errc::unavailable) << "an engine's own error event";
    EXPECT_OK(ask({{"first_token_ms", 400}})) << "silence filled with keep-alives is not a failure";
    EXPECT_ERRC(provider->chat({.model = "ghost", .messages = {llm::Message::user("hi")}}, {}, {}),
                Errc::not_found);
}

TEST(RuntimeClient, SaysSoWhenNothingAnswers) {
    const net::HttpClient http;
    const int port = testkit::free_port_range(1);
    const RuntimeClient client{http,
                               {.url = std::format("http://127.0.0.1:{}", port), .token = Secret{"x"}},
                               net::RetryPolicy::none()};
    EXPECT_ERRC(client.ping(), Errc::network);
    const auto waited = client.wait_until_ready(300ms, {}, 50ms);
    ASSERT_ERRC(waited, Errc::timeout) << "a pod that is still booting does not answer either";
    EXPECT_THAT(waited.error().message(), HasSubstr("last seen: network"));
}

TEST(RuntimeClient, APodStillBootingBehindRunpodsProxyIsWaitedFor) {
    // RunPod's proxy while the pod boots: no route yet (404), then nothing
    // listening behind the route (502), then the runtime itself.
    std::atomic<int> polls{0};
    net::HttpServer proxy{net::HttpServer::Options{.host = "127.0.0.1", .port = 0}};
    proxy.route(net::Method::get, "/v1/health", [&polls](const auto&) {
        const int poll = ++polls;
        if (poll == 1) {
            return net::HttpServer::Response::text(404, "404 page not found");
        }
        if (poll == 2) {
            return net::HttpServer::Response::text(502, "Bad Gateway");
        }
        runtime_api::HealthReport report;
        report.engines = {{.model = "chat", .role = "commander", .state = EngineState::ready}};
        return net::HttpServer::Response::json(200, json::dump(runtime_api::to_json(report)));
    });
    ASSERT_OK(proxy.start());
    const net::HttpClient http;
    const RuntimeClient client{
        http,
        {.url = std::format("http://127.0.0.1:{}", proxy.port()), .token = Secret{"x"}},
        net::RetryPolicy::none()};
    const runtime_api::HealthReport ready = unwrap(client.wait_until_ready(20s, {}, 20ms));
    EXPECT_EQ(ready.engines.size(), 1U);
    EXPECT_EQ(polls.load(), 3) << "the 404 and the 502 were waited out";
}

TEST(RuntimeClient, RefusesARuntimeOfAnotherProtocol) {
    net::HttpServer impostor{net::HttpServer::Options{.host = "127.0.0.1", .port = 0}};
    impostor.route(net::Method::get, "/ping",
                   [](const auto&) { return net::HttpServer::Response::text(200, "pong"); });
    impostor.route(net::Method::get, "/v1/health", [](const auto&) {
        runtime_api::HealthReport report;
        report.protocol = "2";
        return net::HttpServer::Response::json(200, json::dump(runtime_api::to_json(report)));
    });
    ASSERT_OK(impostor.start());
    const net::HttpClient http;
    const RuntimeClient client{
        http,
        {.url = std::format("http://127.0.0.1:{}", impostor.port()), .token = Secret{"x"}},
        net::RetryPolicy::none()};
    EXPECT_ERRC(client.ping(), Errc::unsupported) << "something answers, but not a runtime";
    const auto health = client.health();
    ASSERT_ERRC(health, Errc::unsupported);
    EXPECT_THAT(health.error().message(), HasSubstr("speaks protocol v2"));
    EXPECT_ERRC(client.wait_until_ready(20s, {}, 50ms), Errc::unsupported)
        << "and waiting will not change that";
}

}  // namespace
}  // namespace rr::runtime_client
