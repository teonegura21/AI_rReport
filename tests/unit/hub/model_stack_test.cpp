#include <rr/hub/model_stack.hpp>

#include <atomic>
#include <filesystem>
#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/llm/accounting.hpp>
#include <rr/llm/replay.hpp>
#include <rr/net/http_server.hpp>
#include <rr/net/sse.hpp>
#include <rr/runtime_client/runtime_client.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::hub {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;
using Response = net::HttpServer::Response;

// A model server that answers "hi", after failing its first `failures` calls
// with a 503 (an engine restarting: transient). It counts every request.
class ModelServer {
public:
    explicit ModelServer(int failures = 0) : failures_(failures) {
        server_.route(net::Method::post, "/v1/chat/completions", [this](const net::HttpServer::Request& request) {
            const int call = ++calls_;
            {
                const std::lock_guard lock(mutex_);
                authorization_ = request.headers.get("Authorization").value_or("");
            }
            if (call <= failures_) {
                return Response::json(
                    503, R"({"error":{"code":503,"message":"restarting","type":"unavailable_error"}})");
            }
            return Response::event_stream([](net::HttpServer::ChunkWriter& writer) -> Result<void> {
                for (
                    const char* data :
                    {R"({"choices":[{"delta":{"content":"hi"},"finish_reason":null,"index":0}],"model":"chat"})",
                     R"({"choices":[{"delta":{},"finish_reason":"stop","index":0}],"model":"chat"})",
                     R"({"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":1}})", "[DONE]"}) {
                    if (!writer.write(net::format_sse_event(data))) {
                        return fail(Errc::cancelled, "gone");
                    }
                }
                return ok();
            });
        });
        unwrap(server_.start());
    }

    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}", server_.port()); }
    [[nodiscard]] int calls() const { return calls_.load(); }
    [[nodiscard]] std::string authorization() const {
        const std::lock_guard lock(mutex_);
        return authorization_;
    }

private:
    int failures_;
    std::atomic<int> calls_{0};
    mutable std::mutex mutex_;
    std::string authorization_;
    net::HttpServer server_{net::HttpServer::Options{.host = "127.0.0.1", .port = 0}};
};

class Recorder final : public llm::CallRecorder {
public:
    Result<void> record(const llm::CallRecord& call) override {
        const std::lock_guard lock(mutex_);
        calls_.push_back(call);
        return ok();
    }
    [[nodiscard]] std::vector<llm::CallRecord> calls() const {
        const std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<llm::CallRecord> calls_;
};

llm::ChatRequest hello() {
    return {.model = "chat", .messages = {llm::Message::user("Say hi.")}, .temperature = 0.0};
}

class ModelStackTest : public ::testing::Test {
protected:
    [[nodiscard]] HubSettings settings_with(const ModelServer& server, LlmMode mode) const {
        return HubSettings{.runtime = runtime_client::Endpoint{.url = server.url(), .token = Secret{"t0ken"}},
                           .llm_mode = mode,
                           .recordings = dir_ / "recordings",
                           .call_attempts = 3};
    }
    [[nodiscard]] const testkit::TempDir& dir() const { return dir_; }
    [[nodiscard]] const net::HttpClient& http() const { return http_; }
    [[nodiscard]] Recorder& recorder() { return recorder_; }

private:
    testkit::TempDir dir_;
    net::HttpClient http_;
    Recorder recorder_;
};

TEST_F(ModelStackTest, EveryLiveCallGoesToTheRuntimeAndIsAccounted) {
    const ModelServer runtime;
    const ModelStack stack = unwrap(ModelStack::build(settings_with(runtime, LlmMode::live), http(),
                                                      recorder(), SystemClock::instance(), {}));
    EXPECT_EQ(stack.destination(), std::format("the runtime at {}", runtime.url()));

    const llm::ChatResponse answer = unwrap(stack.provider().chat(hello(), {}, {}));
    EXPECT_EQ(answer.first().content, "hi");
    EXPECT_EQ(runtime.calls(), 1) << "ADR-0017: the runtime is the one model endpoint";
    EXPECT_EQ(runtime.authorization(), "Bearer t0ken");
    ASSERT_EQ(recorder().calls().size(), 1U);
    EXPECT_EQ(recorder().calls()[0].provider, runtime_client::kRuntimeProviderName);
    EXPECT_FALSE(std::filesystem::exists(dir() / "recordings")) << "live mode records nothing";
}

TEST_F(ModelStackTest, WithoutARuntimeNoModelCanBeCalled) {
    const ModelServer runtime;
    HubSettings settings = settings_with(runtime, LlmMode::live);
    settings.runtime.reset();
    const auto stack = ModelStack::build(settings, http(), recorder(), SystemClock::instance(), {});
    ASSERT_ERRC(stack, Errc::invalid_argument);
    EXPECT_THAT(stack.error().message(), HasSubstr("set runpod.pod_id"));
}

TEST_F(ModelStackTest, TransientFailuresAreRetriedAndEachAttemptAccounted) {
    const ModelServer flaky{1};
    const ModelStack stack = unwrap(ModelStack::build(settings_with(flaky, LlmMode::live), http(), recorder(),
                                                      SystemClock::instance(), {}));
    EXPECT_EQ(unwrap(stack.provider().chat(hello(), {}, {})).first().content, "hi");
    EXPECT_EQ(flaky.calls(), 2);
    ASSERT_EQ(recorder().calls().size(), 2U) << "retries above accounting: every attempt is a record";
    EXPECT_TRUE(recorder().calls()[0].error.has_value());
    EXPECT_EQ(recorder().calls()[0].request_key, recorder().calls()[1].request_key) << "attempts of one call";

    const ModelServer flaky_again{1};
    Recorder once;
    const ModelStack bench = unwrap(ModelStack::build(settings_with(flaky_again, LlmMode::live), http(), once,
                                                      SystemClock::instance(), {.retries = false}));
    EXPECT_ERRC(bench.provider().chat(hello(), {}, {}), Errc::unavailable) << "a failure is a result";
    EXPECT_EQ(flaky_again.calls(), 1);
}

TEST_F(ModelStackTest, RecordModeKeepsEachCallOnce) {
    const ModelServer flaky{1};
    const ModelStack stack = unwrap(ModelStack::build(settings_with(flaky, LlmMode::record), http(),
                                                      recorder(), SystemClock::instance(), {}));
    ASSERT_OK(stack.provider().chat(hello(), {}, {}));
    const llm::Recordings recordings{dir() / "recordings"};
    const std::string key = llm::request_key(runtime_client::kRuntimeProviderName, hello());
    EXPECT_TRUE(std::filesystem::exists(recordings.path_for(key))) << "filed under the runtime's name";
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{dir() / "recordings"}, {}), 1)
        << "recorded once, after the retry succeeded";
}

TEST_F(ModelStackTest, AReplayCallsNoModelAndNeedsNoRuntime) {
    const ModelServer runtime;
    // Record one call, live.
    ASSERT_OK(unwrap(ModelStack::build(settings_with(runtime, LlmMode::record), http(), recorder(),
                                       SystemClock::instance(), {}))
                  .provider()
                  .chat(hello(), {}, {}));
    ASSERT_EQ(runtime.calls(), 1);

    HubSettings replay = settings_with(runtime, LlmMode::replay);
    replay.runtime.reset();
    Recorder replayed;
    const ModelStack stack = unwrap(ModelStack::build(replay, http(), replayed, SystemClock::instance(), {}));
    EXPECT_THAT(stack.destination(), HasSubstr("recorded answers in"));
    EXPECT_EQ(unwrap(stack.provider().chat(hello(), {}, {})).first().content, "hi");
    EXPECT_EQ(runtime.calls(), 1) << "no model was called";
    ASSERT_EQ(replayed.calls().size(), 1U) << "a replayed call is accounted, at its recorded cost";
    EXPECT_EQ(replayed.calls()[0].usage.prompt_tokens, 3);

    llm::ChatRequest unrecorded = hello();
    unrecorded.messages[0] = llm::Message::user("Something new.");
    EXPECT_ERRC(stack.provider().chat(unrecorded, {}, {}), Errc::not_found);
}

}  // namespace
}  // namespace rr::hub
