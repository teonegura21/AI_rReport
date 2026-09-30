// End-to-end tests of the runtime: a real Supervisor with rr_fake_engine
// processes behind it, driven over HTTP like the hub drives it.
#include <rr/supervisor/supervisor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <latch>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/hash.hpp>
#include <rr/fake_engine/stream.hpp>
#include <rr/json/json.hpp>
#include <rr/net/http_client.hpp>
#include <rr/net/sse.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::supervisor {
namespace {

using namespace std::chrono_literals;
using runtime_api::EngineState;
using ::testing::HasSubstr;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kToken = "t0ken-for-tests";

std::string test_catalog() {
    return std::format(R"toml(
[[model]]
id = "chat"
role = "commander"
file = "chat.gguf"
sha256 = "{0}"
[[model]]
id = "embed"
role = "embedder"
file = "embed.gguf"
sha256 = "{0}"
[[model]]
id = "slow"
role = "infantry"
file = "slow.gguf"
sha256 = "{0}"
[[model]]
id = "stuck"
role = "infantry"
file = "stuck.gguf"
sha256 = "{0}"
[[model]]
id = "crashy"
role = "infantry"
file = "crashy.gguf"
sha256 = "{0}"

[[profile]]
name = "test"
  [[profile.engine]]
  model = "chat"
  extra_args = ["--fake-load-ms", "150"]
  [[profile.engine]]
  model = "embed"
  [[profile.engine]]
  model = "slow"
  extra_args = ["--fake-token-ms", "150"]
  [[profile.engine]]
  model = "stuck"
  extra_args = ["--fake-never-ready"]

[[profile]]
name = "crash"
  [[profile.engine]]
  model = "crashy"
  extra_args = ["--fake-exit-after-ms", "400"]
)toml",
                       std::string(64, 'a'));
}

// A running runtime with fake engines, on ephemeral ports.
struct Pod {
    testkit::TempDir dir{"rr_supervisor_test"};
    FakeClock clock;
    net::HttpClient client;
    int first_engine_port = testkit::free_port_range(4);  // engines listen here, in profile order
    std::unique_ptr<Supervisor> supervisor;

    explicit Pod(const std::string& profile = "test",
                 const std::function<void(SupervisorSettings&)>& customize = nullptr) {
        SupervisorSettings settings;
        settings.host = "127.0.0.1";
        settings.port = 0;
        settings.token = Secret{std::string{kToken}};
        settings.catalog = unwrap(parse_catalog(test_catalog()));
        settings.profile = profile;
        settings.models_dir = dir / "models";
        settings.blobs_dir = dir / "blobs";
        settings.llama_server = RR_FAKE_ENGINE;
        settings.first_engine_port = first_engine_port;
        settings.engine_startup_timeout = 20s;
        settings.engine_stop_grace = 2s;
        settings.engine_restart_backoff = 50ms;
        settings.engine_poll_interval = 50ms;
        settings.watchdog_interval = 20ms;
        settings.stream_keep_alive = 100ms;
        settings.stream_head_patience = 300ms;
        settings.clock = &clock;
        settings.version = "test-version";
        settings.query_gpus = [] {
            return Result<std::vector<runtime_api::GpuInfo>>{std::vector<runtime_api::GpuInfo>{
                {.index = 0, .name = "Fake GPU", .memory_total_mib = 49152, .driver_version = "0.0"}}};
        };
        if (customize) {
            customize(settings);
        }
        supervisor = unwrap(Supervisor::create(std::move(settings)));
        unwrap(supervisor->start());
    }

    [[nodiscard]] std::string url(std::string_view path) const {
        return std::format("http://127.0.0.1:{}{}", supervisor->port(), path);
    }
    [[nodiscard]] static net::Headers auth() { return {{"Authorization", std::format("Bearer {}", kToken)}}; }

    [[nodiscard]] Result<net::HttpClient::Response> get(std::string_view path,
                                                        net::Headers headers = auth()) const {
        return client.send({.url = url(path), .headers = std::move(headers)});
    }
    [[nodiscard]] Result<net::HttpClient::Response> post(std::string_view path, std::string body) const {
        net::Headers headers = auth();
        headers.add("Content-Type", "application/json");
        return client.send({.method = net::Method::post,
                            .url = url(path),
                            .headers = std::move(headers),
                            .body = std::move(body)});
    }

    // Waits until `model`'s engine reaches `state`.
    [[nodiscard]] bool wait_for(std::string_view model, EngineState state,
                                std::chrono::milliseconds within = 20s) const {
        const auto deadline = Clock::now() + within;
        while (Clock::now() < deadline) {
            for (const auto& engine : supervisor->health().engines) {
                if (engine.model == model && engine.state == state) {
                    return true;
                }
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    [[nodiscard]] runtime_api::EngineInfo engine(std::string_view model) const {
        for (const auto& engine : supervisor->health().engines) {
            if (engine.model == model) {
                return engine;
            }
        }
        throw std::runtime_error("no such engine");
    }
};

// Polls `done` every 10 ms until it holds or `within` passes.
bool eventually(const std::function<bool()>& done, std::chrono::milliseconds within = 5s) {
    const auto deadline = Clock::now() + within;
    while (!done()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// A counter of the fake engine at `engine_index` (profile order), asked
// directly: the engines listen on their own ports.
std::int64_t engine_stat(const Pod& pod, int engine_index, const char* name) {
    const auto stats = unwrap(pod.client.send(
        {.url = std::format("http://127.0.0.1:{}/stats", pod.first_engine_port + engine_index)}));
    return unwrap(json::parse(stats.body))[name].get<std::int64_t>();
}
std::int64_t abandoned_streams(const Pod& pod, int engine_index) {
    return engine_stat(pod, engine_index, "abandoned_streams");
}

// What the "chat" engine streams by default, llama-server's way: a role
// chunk, five tokens (tok0 … tok4), a finish chunk, then [DONE].
const fake_engine::StreamPlan kChatAnswer = fake_engine::default_plan("chat");
std::string token_event(int index) {
    return fake_engine::content_event(kChatAnswer, std::format("tok{}", index));
}
std::vector<std::string> all_tokens() {
    return fake_engine::events(kChatAnswer);
}

std::size_t count(std::string_view text, std::string_view needle) {
    std::size_t found = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + 1)) {
        ++found;
    }
    return found;
}

constexpr std::string_view kKeepAlive = ": keep-alive\n";

// One streamed chat call through the pod, recorded as the hub would see it.
struct StreamedCall {
    Result<net::HttpClient::Response> result = fail(Errc::internal, "not run");
    int status = 0;
    std::optional<Clock::duration> head_after;  // when the status arrived, from the start
    std::string raw;                            // every byte of the body
    std::vector<std::string> events;            // the data of each parsed event
};

StreamedCall stream_chat(const Pod& pod, std::string body, const std::stop_token& stop = {},
                         const std::function<void()>& on_head = nullptr) {
    StreamedCall call;
    net::SseParser parser;
    net::Headers headers = Pod::auth();
    headers.add("Content-Type", "application/json");
    const auto started = Clock::now();
    call.result =
        pod.client.stream({.method = net::Method::post,
                           .url = pod.url("/v1/chat/completions"),
                           .headers = std::move(headers),
                           .body = std::move(body)},
                          {.on_head = [&](int status, const net::Headers&) -> Result<void> {
                               call.status = status;
                               call.head_after = Clock::now() - started;
                               if (on_head) {
                                   on_head();
                               }
                               return ok();
                           },
                           .on_chunk =
                               [&](std::string_view chunk) {
                                   call.raw.append(chunk);
                                   return parser.feed(chunk, [&](const net::SseEvent& event) -> Result<void> {
                                       call.events.push_back(event.data);
                                       return ok();
                                   });
                               }},
                          stop);
    return call;
}

// ---------------------------------------------------------------------------
// Configuration and authentication
// ---------------------------------------------------------------------------
TEST(Supervisor, RequiresATokenAndAKnownProfile) {
    SupervisorSettings settings;
    settings.catalog = unwrap(parse_catalog(test_catalog()));
    settings.profile = "test";
    settings.blobs_dir = "/tmp/unused";
    EXPECT_ERRC(Supervisor::create(settings), Errc::invalid_argument) << "no token";
    settings.token = Secret{"x"};
    settings.profile = "90gb";
    EXPECT_ERRC(Supervisor::create(settings), Errc::not_found);
}

TEST(Supervisor, OnlyPingIsOpenEverythingElseNeedsTheToken) {
    const Pod pod;
    EXPECT_EQ(unwrap(pod.get("/ping", {})).body, "ok");

    const auto anonymous = unwrap(pod.get("/v1/health", {}));
    EXPECT_EQ(anonymous.status, 401);
    EXPECT_THAT(std::string{anonymous.headers.get("WWW-Authenticate").value_or("")}, HasSubstr("Bearer"));
    EXPECT_EQ(unwrap(pod.get("/v1/health", {{"Authorization", "Bearer wrong"}})).status, 401);
    EXPECT_EQ(unwrap(pod.get("/v1/blobs/lib/x", {})).status, 401);
    EXPECT_EQ(unwrap(pod.get("/v1/health", {{"Authorization", std::format("bearer {}", kToken)}})).status,
              200)
        << "the auth scheme is case-insensitive";
}

// ---------------------------------------------------------------------------
// Engines, health and the proxy
// ---------------------------------------------------------------------------
TEST(Supervisor, HealthReportsGpusAndEnginesAsTheyLoad) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    ASSERT_TRUE(pod.wait_for("embed", EngineState::ready));
    const auto report =
        unwrap(runtime_api::health_report_from_json(unwrap(json::parse(unwrap(pod.get("/v1/health")).body))));
    EXPECT_EQ(report.version, "test-version");
    EXPECT_EQ(report.profile, "test");
    ASSERT_EQ(report.gpus.size(), 1U);
    EXPECT_EQ(report.gpus[0].name, "Fake GPU");
    ASSERT_EQ(report.engines.size(), 4U);
    EXPECT_EQ(report.engines[0].role, "commander");
    EXPECT_EQ(pod.engine("stuck").state, EngineState::starting) << "never finishes loading";
    EXPECT_EQ(report.stops_in_s, std::nullopt) << "idle stop is off by default";

    const auto models = unwrap(json::parse(unwrap(pod.get("/v1/models")).body));
    ASSERT_EQ(models["data"].size(), 4U);
    EXPECT_EQ(models["data"][0]["id"], "chat");
    EXPECT_EQ(models["data"][0]["state"], "ready");
}

TEST(Supervisor, ProxiesCallsToTheEngineServingTheModel) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const auto answer = unwrap(pod.post("/v1/chat/completions", R"({"model":"chat","messages":[]})"));
    EXPECT_EQ(answer.status, 200);
    EXPECT_THAT(answer.body, HasSubstr("hello from chat"));
    EXPECT_EQ(answer.headers.get("Content-Type"), "application/json");

    ASSERT_TRUE(pod.wait_for("embed", EngineState::ready));
    EXPECT_THAT(unwrap(pod.post("/v1/embeddings", R"({"model":"embed","input":"x"})")).body,
                HasSubstr("embedding"));
}

TEST(Supervisor, StreamsTokensAsTheEngineProducesThem) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    net::SseParser parser;
    std::vector<std::string> events;
    std::optional<Clock::time_point> first;
    net::Headers headers = Pod::auth();
    headers.add("Content-Type", "application/json");
    const auto started = Clock::now();
    const auto response = unwrap(net::expect_success(
        pod.client.stream({.method = net::Method::post,
                           .url = pod.url("/v1/chat/completions"),
                           .headers = headers,
                           .body = R"({"model":"chat","stream":true,"messages":[]})"},
                          {.on_chunk = [&](std::string_view chunk) {
                              return parser.feed(chunk, [&](const net::SseEvent& event) -> Result<void> {
                                  first = first.value_or(Clock::now());
                                  events.push_back(event.data);
                                  return ok();
                              });
                          }})));
    const auto finished = Clock::now();
    EXPECT_EQ(response.headers.get("Content-Type"), "text/event-stream");
    EXPECT_EQ(events, all_tokens());
    ASSERT_TRUE(first.has_value());
    EXPECT_LT(*first - started, finished - started - 40ms) << "tokens must not be buffered until the end";
}

TEST(Supervisor, AnswersForUnknownModelsBadRequestsAndLoadingEngines) {
    const Pod pod;
    EXPECT_ERRC(net::expect_success(pod.post("/v1/chat/completions", R"({"model":"ghost"})")),
                Errc::not_found);
    EXPECT_ERRC(net::expect_success(pod.post("/v1/chat/completions", "not json")), Errc::invalid_argument);
    EXPECT_ERRC(net::expect_success(pod.post("/v1/chat/completions", R"({"messages":[]})")),
                Errc::invalid_argument);
    EXPECT_ERRC(net::expect_success(pod.post("/v1/chat/completions", R"({"model":"chat","stream":"yes"})")),
                Errc::invalid_argument)
        << "not a 500";

    const auto loading = net::expect_success(pod.post("/v1/chat/completions", R"({"model":"stuck"})"));
    ASSERT_ERRC(loading, Errc::unavailable);
    EXPECT_EQ(loading.error().retry_after(), 10s) << "come back when it has loaded";
}

TEST(Supervisor, PassesEngineErrorsThroughIntact) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const auto response = unwrap(pod.post("/v1/chat/completions", R"({"model":"chat","fail":true})"));
    EXPECT_EQ(response.status, 400);
    EXPECT_THAT(response.body, HasSubstr("the fake engine was asked to fail"));
}

TEST(Supervisor, AClientThatLeavesCancelsTheEnginesWork) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("slow", EngineState::ready));
    std::stop_source cancel;
    net::Headers headers = Pod::auth();
    headers.add("Content-Type", "application/json");
    const auto result =
        pod.client.stream({.method = net::Method::post,
                           .url = pod.url("/v1/chat/completions"),
                           .headers = headers,
                           .body = R"({"model":"slow","stream":true})"},
                          {.on_chunk = [&](std::string_view) -> Result<void> {
                              cancel.request_stop();  // the hub gives up after the first token
                              return ok();
                          }},
                          cancel.get_token());
    EXPECT_ERRC(result, Errc::cancelled);
    // The slow engine (third in the profile) sees its connection close and
    // stops generating, long before its five tokens (150 ms apart) run out.
    EXPECT_TRUE(eventually([&] { return abandoned_streams(pod, 2) == 1; }))
        << "the supervisor must drop the engine's stream when its client leaves";
}

TEST(Supervisor, AClientThatLeavesWhileTheEngineReadsCancelsIt) {
    // Keep-alives 10 s apart, as in production (15 s): a keep-alive write
    // would notice the departure too, but only when the next one is due.
    const Pod pod{"test", [](SupervisorSettings& settings) { settings.stream_keep_alive = 10s; }};
    ASSERT_TRUE(pod.wait_for("slow", EngineState::ready));
    std::stop_source cancel;
    // The engine answers 200 at once, then "reads the prompt" for 5 s; the
    // hub gives up as soon as it has the status.
    const StreamedCall call = stream_chat(pod, R"({"model":"slow","stream":true,"first_token_ms":5000})",
                                          cancel.get_token(), [&] { cancel.request_stop(); });
    EXPECT_ERRC(call.result, Errc::cancelled);
    EXPECT_TRUE(eventually([&] { return abandoned_streams(pod, 2) == 1; }, 2s))
        << "a silent relay probes its client every tick, and frees the engine at once";
}

TEST(Supervisor, AClientThatLeavesBeforeTheEngineAnswersCancelsIt) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    // A plain call to a busy engine (it answers after 1 s); the hub gives up
    // after 200 ms. The relay, still waiting for the engine's status, must
    // notice and hang up, so the engine finds nobody listening.
    std::stop_source cancel;
    const std::jthread canceller{[&] {
        std::this_thread::sleep_for(200ms);
        cancel.request_stop();
    }};
    net::Headers headers = Pod::auth();
    headers.add("Content-Type", "application/json");
    EXPECT_ERRC(pod.client.send({.method = net::Method::post,
                                 .url = pod.url("/v1/chat/completions"),
                                 .headers = headers,
                                 .body = R"({"model":"chat","head_delay_ms":1000})"},
                                cancel.get_token()),
                Errc::cancelled);
    EXPECT_TRUE(eventually([&] { return engine_stat(pod, 0, "abandoned_requests") == 1; }, 3s));
}

// ---------------------------------------------------------------------------
// The relay: silences, late answers and broken answers
// ---------------------------------------------------------------------------
TEST(Supervisor, KeepsASilentStreamAliveWithComments) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const StreamedCall call = stream_chat(pod, R"({"model":"chat","stream":true,"first_token_ms":600})");
    ASSERT_OK(call.result);
    EXPECT_EQ(call.events, all_tokens()) << "comments never change the events";
    const std::size_t first_data = call.raw.find("data:");
    ASSERT_NE(first_data, std::string::npos);
    EXPECT_GE(count(std::string_view{call.raw}.substr(0, first_data), kKeepAlive), 2U)
        << "600 ms of silence, and a comment is due after every 100 ms of it";
}

TEST(Supervisor, KeepAlivesNeverSplitAnEvent) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    // Every event arrives in two halves, 300 ms apart: silences in mid-line,
    // three times the keep-alive interval.
    const StreamedCall call = stream_chat(pod, R"({"model":"chat","stream":true,"split_pause_ms":300})");
    ASSERT_OK(call.result);
    EXPECT_EQ(call.events, all_tokens());
}

TEST(Supervisor, AnswersAtOnceWhenTheEngineHoldsItsHeaders) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const StreamedCall call = stream_chat(pod, R"({"model":"chat","stream":true,"head_delay_ms":2000})");
    ASSERT_OK(call.result);
    EXPECT_EQ(call.status, 200);
    ASSERT_TRUE(call.head_after.has_value());
    EXPECT_LT(*call.head_after, 1000ms) << "the relay answered for the busy engine";
    EXPECT_GE(count(call.raw, kKeepAlive), 2U) << "and kept the connection alive meanwhile";
    EXPECT_EQ(call.events, all_tokens());
}

TEST(Supervisor, AnEarlyEngineErrorKeepsItsStatus) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const StreamedCall call = stream_chat(pod, R"({"model":"chat","stream":true,"fail":true})");
    ASSERT_OK(call.result);
    EXPECT_EQ(call.status, 400);
    EXPECT_THAT(call.result->body, HasSubstr("the fake engine was asked to fail"));
}

TEST(Supervisor, ALateEngineErrorArrivesInTheStream) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const StreamedCall call =
        stream_chat(pod, R"({"model":"chat","stream":true,"fail":true,"head_delay_ms":1000})");
    ASSERT_OK(call.result) << "a complete stream: its one event is the error";
    EXPECT_EQ(call.status, 200) << "the relay had already answered";
    ASSERT_EQ(call.events.size(), 1U);
    const auto error = net::error_from_event(call.events[0]);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), Errc::invalid_argument) << "what the engine's 400 would have said: not retried";
    EXPECT_THAT(error->message(), HasSubstr("the fake engine was asked to fail"));
}

TEST(Supervisor, AStreamThatBreaksOffLooksBroken) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const StreamedCall call = stream_chat(pod, R"({"model":"chat","stream":true,"break_after":2})");
    EXPECT_ERRC(call.result, Errc::network)
        << "truncated: the hub retries instead of trusting a partial answer";
    ASSERT_EQ(call.events.size(), 4U);
    EXPECT_EQ(call.events[0], fake_engine::role_event(kChatAnswer));
    EXPECT_EQ(call.events[1], token_event(0));
    EXPECT_EQ(call.events[2], token_event(1));
    const auto error = net::error_from_event(call.events[3]);
    ASSERT_TRUE(error.has_value()) << "and says why, where an event fits";
    EXPECT_EQ(error->code(), Errc::network);
    EXPECT_THAT(error->describe(), HasSubstr("broke off"));
}

TEST(Supervisor, ABreakInsideAnEventLeavesNoGarbage) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    // The engine sends one whole line of its third event, then breaks off:
    // an error event here would merge with that line into a garbage event.
    const StreamedCall call =
        stream_chat(pod, R"({"model":"chat","stream":true,"break_after":2,"break_mid_event":true})");
    EXPECT_ERRC(call.result, Errc::network);
    EXPECT_EQ(call.events, (std::vector<std::string>{fake_engine::role_event(kChatAnswer), token_event(0),
                                                     token_event(1)}))
        << "the unfinished event is dropped by the parser, and nothing was added to it";
    EXPECT_EQ(call.raw.find("\"error\""), std::string::npos) << "no error event where none fits";
}

TEST(Supervisor, NonStreamedCallsWaitForTheEngine) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const auto answer = unwrap(pod.post("/v1/chat/completions", R"({"model":"chat","head_delay_ms":800})"));
    EXPECT_EQ(answer.status, 200);
    EXPECT_EQ(answer.headers.get("Content-Type"), "application/json");
    EXPECT_THAT(answer.body, HasSubstr("hello from chat")) << "no early answer, no comments: plain JSON";
}

TEST(Supervisor, ForwardsOnlyContentTypeAndAccept) {
    const Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    net::Headers headers = Pod::auth();
    headers.add("Content-Type", "application/json");
    headers.add("Accept", "application/json");
    headers.add("X-Conversation-Id", "keep-generating-after-disconnect");
    const auto response = unwrap(pod.client.send({.method = net::Method::post,
                                                  .url = pod.url("/v1/chat/completions"),
                                                  .headers = headers,
                                                  .body = R"({"model":"chat","echo_headers":true})"}));
    const json::Json seen = unwrap(json::parse(response.body))["headers"];
    EXPECT_EQ(seen.value("content-type", ""), "application/json");
    EXPECT_EQ(seen.value("accept", ""), "application/json");
    EXPECT_FALSE(seen.contains("authorization")) << "the pod's token stays at the door";
    EXPECT_FALSE(seen.contains("x-conversation-id")) << "llama-server would keep generating for nobody";
}

TEST(Supervisor, StoppingEndsSilentStreamsPromptly) {
    Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    std::latch answered{1};
    StreamedCall call;
    std::jthread hub{[&] {
        call = stream_chat(pod, R"({"model":"chat","stream":true,"first_token_ms":60000})", {},
                           [&] { answered.count_down(); });
    }};
    answered.wait();
    const auto started = Clock::now();
    pod.supervisor->stop();
    EXPECT_LT(Clock::now() - started, 3s) << "an engine that says nothing for a minute holds nothing up";
    hub.join();
    ASSERT_OK(call.result) << "nothing of an answer went out, so the error event is the whole body";
    ASSERT_EQ(call.events.size(), 1U);
    const auto error = net::error_from_event(call.events[0]);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), Errc::unavailable) << "retried elsewhere, or later";
}

TEST(Supervisor, StoppingEndsWaitsForSlowEnginesPromptly) {
    Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    // A plain call to an engine that will not answer for a minute.
    std::optional<Result<net::HttpClient::Response>> answer;
    std::jthread hub{
        [&] { answer = pod.post("/v1/chat/completions", R"({"model":"chat","head_delay_ms":60000})"); }};
    std::this_thread::sleep_for(300ms);  // the relay is waiting for the engine
    const auto started = Clock::now();
    pod.supervisor->stop();
    EXPECT_LT(Clock::now() - started, 3s) << "a waiting relay gives up when the runtime stops";
    hub.join();
    ASSERT_TRUE(answer.has_value());
    EXPECT_ERRC(net::expect_success(*answer), Errc::unavailable)
        << "shutting down: retried elsewhere, or later";
}

TEST(Supervisor, AStreamInFlightKeepsThePodAwake) {
    const testkit::TempDir marker_dir{"rr_supervisor_marker"};
    const auto marker = marker_dir / "stopped";
    Pod pod{"test", [&](SupervisorSettings& settings) {
                settings.idle_stop = 10min;
                settings.stop_command = {"sh", "-c", std::format("touch '{}'", marker.string())};
            }};
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    std::latch answered{1};
    std::jthread hub{[&] {
        (void)stream_chat(pod, R"({"model":"chat","stream":true,"first_token_ms":1500})", {},
                          [&] { answered.count_down(); });
    }};
    answered.wait();
    pod.clock.advance(11min);
    std::this_thread::sleep_for(300ms);  // fifteen watchdog ticks
    EXPECT_FALSE(std::filesystem::exists(marker)) << "a generation in progress is not idleness";
    hub.join();
    // The hub has read the whole stream, but the runtime records the end a
    // moment later, when the relay lets go of the request on its own thread.
    // Move the clock only then, or the idle period starts after the jump.
    // (stops_in_s is reported only while no request is in flight.)
    ASSERT_TRUE(eventually([&] { return pod.supervisor->health().stops_in_s.has_value(); }))
        << "the runtime saw the stream end";
    pod.clock.advance(11min);
    EXPECT_TRUE(eventually([&] { return std::filesystem::exists(marker); }))
        << "after it ends, idleness counts";
}

// ---------------------------------------------------------------------------
// Blobs
// ---------------------------------------------------------------------------
TEST(Supervisor, BlobUploadsAreVerifiedAndCheckable) {
    const Pod pod;
    const std::string audio = "pretend these are 60 MB of audio";
    const std::string hash = blake3(audio).hex();
    const auto upload = [&](std::string_view library, std::string_view name, const std::string& body) {
        return pod.client.send({.method = net::Method::put,
                                .url = pod.url(runtime_api::blob_path(library, name)),
                                .headers = Pod::auth(),
                                .body = body});
    };
    const auto head = [&](std::string_view library, std::string_view name) {
        return pod.client.send({.method = net::Method::head,
                                .url = pod.url(runtime_api::blob_path(library, name)),
                                .headers = Pod::auth()});
    };

    EXPECT_EQ(unwrap(head("default", hash)).status, 404);
    const auto created = unwrap(upload("default", hash, audio));
    EXPECT_EQ(created.status, 201);
    EXPECT_THAT(created.body, HasSubstr(hash));
    EXPECT_EQ(unwrap(upload("default", hash, audio)).status, 200) << "already there";
    const auto present = unwrap(head("default", hash));
    EXPECT_EQ(present.status, 200);
    EXPECT_EQ(present.headers.get("X-Blob-Size"), std::to_string(audio.size()));
    EXPECT_EQ(unwrap(head("other-library", hash)).status, 404) << "libraries are separate";

    EXPECT_ERRC(net::expect_success(upload("default", blake3("something else").hex(), audio)),
                Errc::integrity);
    EXPECT_EQ(unwrap(head("default", blake3("something else").hex())).status, 404) << "nothing was stored";
    EXPECT_ERRC(net::expect_success(upload("Bad.Name", hash, audio)), Errc::invalid_argument);
    EXPECT_ERRC(net::expect_success(upload("default", "not-a-hash", audio)), Errc::invalid_argument);
    EXPECT_EQ(unwrap(pod.get(runtime_api::blob_path("default", hash))).status, 405) << "no downloads";
}

// ---------------------------------------------------------------------------
// Stopping the pod, and engine restarts
// ---------------------------------------------------------------------------
TEST(Supervisor, HeartbeatsKeepThePodAliveAndIdlenessStopsIt) {
    const testkit::TempDir marker_dir{"rr_supervisor_marker"};
    const auto marker = marker_dir / "stopped";
    Pod pod{"test", [&](SupervisorSettings& settings) {
                settings.idle_stop = 10min;
                settings.stop_command = {"sh", "-c", std::format("touch '{}'", marker.string())};
            }};
    const auto stops_in = [&] {
        const auto report = unwrap(
            runtime_api::health_report_from_json(unwrap(json::parse(unwrap(pod.get("/v1/health")).body))));
        return report.stops_in_s.value_or(-1);
    };
    EXPECT_EQ(stops_in(), 600);

    pod.clock.advance(9min);
    EXPECT_EQ(stops_in(), 60) << "health checks are not activity";
    EXPECT_EQ(unwrap(pod.post("/v1/heartbeat", "")).status, 204);
    EXPECT_EQ(stops_in(), 600) << "a heartbeat resets the timer";

    pod.clock.advance(11min);
    EXPECT_TRUE(eventually([&] { return std::filesystem::exists(marker); })) << "the stop command ran";
    EXPECT_TRUE(pod.supervisor->stop_requested());
}

TEST(Supervisor, WithoutACommandAnIdlePodStopsItselfAndRetries) {
    std::atomic<int> calls{0};
    Pod pod{"test", [&calls](SupervisorSettings& settings) {
                settings.idle_stop = 10min;
                settings.stop_pod = [&calls]() -> Result<void> {
                    ++calls;
                    return fail(Errc::unavailable, "RunPod is having a moment");
                };
            }};
    pod.clock.advance(11min);
    EXPECT_TRUE(eventually([&] { return calls.load() == 1; })) << "RunPod's API is asked";
    std::this_thread::sleep_for(200ms);  // ten watchdog ticks
    EXPECT_EQ(calls.load(), 1) << "not on every tick";
    pod.clock.advance(6min);
    EXPECT_TRUE(eventually([&] { return calls.load() == 2; })) << "a pod still up is asked again";
}

TEST(Supervisor, ACrashedEngineIsRestarted) {
    const Pod pod{"crash"};
    const auto deadline = Clock::now() + 15s;
    while (pod.engine("crashy").restarts < 2 && Clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    const runtime_api::EngineInfo info = pod.engine("crashy");
    EXPECT_GE(info.restarts, 2);
    ASSERT_TRUE(info.last_error.has_value());
    EXPECT_THAT(*info.last_error, HasSubstr("exit code 3"));
}

TEST(Supervisor, StoppingIsPromptAndTakesTheEnginesDown) {
    Pod pod;
    ASSERT_TRUE(pod.wait_for("chat", EngineState::ready));
    const auto started = Clock::now();
    pod.supervisor->stop();
    EXPECT_LT(Clock::now() - started, 5s);
    EXPECT_EQ(pod.engine("chat").state, EngineState::stopped);
}

}  // namespace
}  // namespace rr::supervisor
