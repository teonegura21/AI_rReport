// The OpenAI-compatible provider against a server that plays exact bytes:
// llama-server's stream shape, keep-alive comments, streams that end without
// [DONE], error events, error statuses and silences.
#include <rr/llm/openai_provider.hpp>

#include <chrono>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/net/http_server.hpp>
#include <rr/net/sse.hpp>
#include <rr/testkit/result.hpp>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using namespace std::chrono_literals;
using testing::ask;
using ::testing::HasSubstr;
using testkit::unwrap;
using Response = net::HttpServer::Response;
using Clock = std::chrono::steady_clock;

// Serves POST /v1/chat/completions with whatever the test decides, and keeps
// what it was sent.
class ModelServer {
public:
    explicit ModelServer(std::function<Response(const net::HttpServer::Request&)> answer) {
        server_.route(net::Method::post, "/v1/chat/completions",
                      [this, answer = std::move(answer)](const net::HttpServer::Request& request) {
                          {
                              const std::lock_guard lock(mutex_);
                              bodies_.push_back(request.body);
                              authorization_ = std::string{request.headers.get("Authorization").value_or("")};
                          }
                          return answer(request);
                      });
        unwrap(server_.start());
    }

    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}/", server_.port()); }
    [[nodiscard]] json::Json last_body() const { return unwrap(json::parse(last_raw_body())); }
    [[nodiscard]] std::string last_raw_body() const {
        const std::lock_guard lock(mutex_);
        return bodies_.back();
    }
    [[nodiscard]] std::string authorization() const {
        const std::lock_guard lock(mutex_);
        return authorization_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> bodies_;
    std::string authorization_;
    net::HttpServer server_{net::HttpServer::Options{.host = "127.0.0.1", .port = 0}};
};

// An event stream that writes `pieces` as they are, `pause` apart.
Response stream_of(std::vector<std::string> pieces, std::chrono::milliseconds pause = 0ms) {
    return Response::event_stream(
        [pieces = std::move(pieces), pause](net::HttpServer::ChunkWriter& writer) -> Result<void> {
            for (const std::string& piece : pieces) {
                if (!writer.write(piece)) {
                    return fail(Errc::cancelled, "gone");
                }
                std::this_thread::sleep_for(pause);
            }
            return ok();
        });
}

std::string event(std::string_view data) {
    return net::format_sse_event(data);
}

// llama-server's stream for a thinking model: role chunk, reasoning, content,
// finish, usage with timings, [DONE].
std::vector<std::string> llama_server_stream() {
    return {
        event(
            R"({"choices":[{"delta":{"content":null,"role":"assistant"},"finish_reason":null,"index":0}],"model":"commander","object":"chat.completion.chunk"})"),
        event(
            R"({"choices":[{"delta":{"reasoning_content":"The user wants "},"finish_reason":null,"index":0}],"model":"commander"})"),
        event(
            R"({"choices":[{"delta":{"reasoning_content":"a capital."},"finish_reason":null,"index":0}],"model":"commander"})"),
        event(
            R"({"choices":[{"delta":{"content":"Par"},"finish_reason":null,"index":0}],"model":"commander"})"),
        event(
            R"({"choices":[{"delta":{"content":"is"},"finish_reason":null,"index":0}],"model":"commander"})"),
        event(R"({"choices":[{"delta":{},"finish_reason":"stop","index":0}],"model":"commander"})"),
        event(
            R"({"choices":[],"model":"commander","usage":{"completion_tokens":6,"prompt_tokens":237,"total_tokens":243,"prompt_tokens_details":{"cached_tokens":236}},"timings":{"cache_n":236,"prompt_n":1,"prompt_per_second":32.3,"predicted_n":6,"predicted_per_second":52.94}})"),
        event("[DONE]"),
    };
}

OpenAiProvider provider_for(const ModelServer& server, OpenAiOptions options = {}) {
    static const net::HttpClient http;
    options.name = "runtime";
    options.base_url = server.url();
    options.caps = {.json_schema = true, .images = true, .thinking = true};
    return OpenAiProvider{http, std::move(options)};
}

TEST(OpenAiProvider, ReadsAStreamedAnswer) {
    const ModelServer server{[](const auto&) { return stream_of(llama_server_stream(), 5ms); }};
    const OpenAiProvider provider = provider_for(server, {.api_key = Secret{"sekrit"}});
    std::vector<std::string> deltas;
    const StreamSink sink{
        .on_content = [&](std::string_view text) { deltas.push_back("content:" + std::string{text}); },
        .on_reasoning = [&](std::string_view text) { deltas.push_back("thought:" + std::string{text}); }};
    const ChatResponse answer = unwrap(provider.chat(ask("capital of France?"), sink, {}));

    EXPECT_EQ(answer.content, "Paris");
    EXPECT_EQ(answer.reasoning, "The user wants a capital.");
    EXPECT_EQ(answer.finish, FinishReason::stop);
    EXPECT_EQ(answer.model, "commander");
    EXPECT_EQ(answer.usage,
              (Usage{.prompt_tokens = 237, .completion_tokens = 6, .cached_prompt_tokens = 236}));
    EXPECT_EQ(answer.prompt_tokens_per_second, 32.3);
    EXPECT_EQ(answer.generation_tokens_per_second, 52.94);
    ASSERT_TRUE(answer.first_token.has_value());
    EXPECT_LE(*answer.first_token, answer.duration);
    EXPECT_EQ(deltas, (std::vector<std::string>{"thought:The user wants ", "thought:a capital.",
                                                "content:Par", "content:is"}))
        << "the null content of the role chunk is not a token";
    EXPECT_EQ(server.authorization(), "Bearer sekrit");
}

TEST(OpenAiProvider, SendsWhatLlamaServerReads) {
    const ModelServer server{[](const auto&) { return stream_of(llama_server_stream()); }};
    const OpenAiProvider provider = provider_for(server);
    ChatRequest request = ask("an atom, please");
    request.messages.insert(request.messages.begin(), Message::system("Be exact."));
    request.max_tokens = 512;
    request.temperature = 0.2;
    request.seed = 42;
    request.json_schema = R"({"type":"object"})";
    request.schema_name = "atom";
    request.thinking = false;
    request.extra = {{"cache_prompt", false}};
    ASSERT_OK(provider.chat(request, {}, {}));

    // Not const: a const operator[] asserts on a missing key, which aborts the
    // whole test program; this one yields null, and the expectation fails.
    json::Json body = server.last_body();
    EXPECT_EQ(body["model"], "commander");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["stream_options"]["include_usage"], true) << "without it, no usage is reported";
    EXPECT_EQ(body["messages"][0], (json::Json{{"role", "system"}, {"content", "Be exact."}}));
    EXPECT_EQ(body["max_tokens"], 512);
    EXPECT_EQ(body["temperature"], 0.2);
    EXPECT_EQ(body["seed"], 42);
    EXPECT_EQ(body["response_format"]["type"], "json_schema");
    EXPECT_EQ(body["response_format"]["json_schema"]["schema"], (json::Json{{"type", "object"}}))
        << "OpenAI's shape: the one llama-server reads the schema from";
    EXPECT_EQ(body["response_format"]["json_schema"]["name"], "atom");
    EXPECT_EQ(body["chat_template_kwargs"]["enable_thinking"], false);
    EXPECT_EQ(body["reasoning_effort"], "none");
    EXPECT_EQ(body["cache_prompt"], false) << "extras pass through";
}

TEST(OpenAiProvider, SendsSamplingAndThinkingAsLlamaServerReadsThem) {
    ChatRequest request = ask("plan the article");
    request.top_k = 20;
    request.min_p = 0.05;
    request.presence_penalty = 0.0;
    request.repeat_penalty = 1.0;
    request.thinking = true;
    request.thinking_budget = ThinkingBudget{.tokens = 4096, .closing = "Answer now."};
    request.reasoning_effort = ReasoningEffort::medium;
    // Not const: a const operator[] asserts on a missing key, which aborts the
    // whole test program; this one yields null, and the expectation fails.
    json::Json body = unwrap(chat_request_body(request, Dialect::llama_server, {.thinking = true}));
    EXPECT_EQ(body["top_k"], 20);
    EXPECT_EQ(body["min_p"], 0.05);
    EXPECT_EQ(body["presence_penalty"], 0.0);
    EXPECT_EQ(body["repeat_penalty"], 1.0);
    EXPECT_EQ(body["chat_template_kwargs"], (json::Json{{"enable_thinking", true}}));
    EXPECT_EQ(body["reasoning_effort"], "medium") << "handed to the chat template";
    EXPECT_EQ(body["reasoning_budget_tokens"], 4096);
    EXPECT_EQ(body["reasoning_budget_message"], "Answer now.") << "said before the forced end of thinking";

    request.thinking_budget->closing.clear();
    request.reasoning_effort.reset();
    json::Json plain = unwrap(chat_request_body(request, Dialect::llama_server, {.thinking = true}));
    EXPECT_FALSE(plain.contains("reasoning_budget_message")) << "no closing: the end tag alone";
    EXPECT_FALSE(plain.contains("reasoning_effort")) << "the template's own level";
}

TEST(OpenAiProvider, TheSchemaReachesTheServerInItsOwnOrder) {
    const ModelServer server{[](const auto&) { return stream_of(llama_server_stream()); }};
    ChatRequest request = ask("Is the sentence supported?");
    // Not alphabetical: sorted, "decision" would come first, and the model
    // would decide before quoting its evidence.
    const std::string schema =
        R"({"type":"object","properties":{"quote":{"type":"string"},"rationale":{"type":"string"},)"
        R"("decision":{"enum":["supported","unsupported"]}},"required":["quote","rationale","decision"]})";
    request.json_schema = schema;
    ASSERT_OK(provider_for(server).chat(request, {}, {}));
    EXPECT_NE(server.last_raw_body().find(schema), std::string::npos)
        << "llama.cpp's grammar follows the order it reads: " << server.last_raw_body();
}

TEST(OpenAiProvider, ImagesTravelAsParts) {
    ChatRequest request = ask("");
    request.messages[0].content = {{.kind = ContentPart::Kind::text, .value = "What is on this slide?"},
                                   {.kind = ContentPart::Kind::image_url, .value = "rr-blob://lib/abc"}};
    const json::Json body = unwrap(chat_request_body(request, Dialect::llama_server, {.images = true}));
    EXPECT_EQ(body["messages"][0]["content"],
              json::Json::parse(R"([{"type":"text","text":"What is on this slide?"},
                                    {"type":"image_url","image_url":{"url":"rr-blob://lib/abc"}}])"));
}

TEST(OpenAiProvider, RefusesWhatItCannotCarry) {
    ChatRequest with_image = ask("");
    with_image.messages[0].content.push_back(
        {.kind = ContentPart::Kind::image_url, .value = "data:image/png;base64,"});
    EXPECT_ERRC(chat_request_body(with_image, Dialect::llama_server, {}), Errc::unsupported);

    ChatRequest with_schema = ask("x");
    with_schema.json_schema = "{}";
    EXPECT_ERRC(chat_request_body(with_schema, Dialect::llama_server, {}), Errc::unsupported);
    with_schema.json_schema = R"({"type": "object",)";
    EXPECT_ERRC(chat_request_body(with_schema, Dialect::llama_server, {.json_schema = true}),
                Errc::invalid_argument)
        << "a schema that is not JSON";

    ChatRequest thinking = ask("x");
    thinking.thinking = true;
    EXPECT_ERRC(chat_request_body(thinking, Dialect::openai, {.thinking = true}), Errc::unsupported)
        << "a plain OpenAI API has no standard switch for it";
    EXPECT_OK(chat_request_body(thinking, Dialect::llama_server, {.thinking = true}));

    ChatRequest top_k = ask("x");
    top_k.top_k = 20;
    EXPECT_ERRC(chat_request_body(top_k, Dialect::openai, {}), Errc::unsupported)
        << "top_k, min_p, repeat_penalty and the thinking fields are llama-server's";
    EXPECT_OK(chat_request_body(top_k, Dialect::llama_server, {}));
    ChatRequest standard = ask("x");
    standard.presence_penalty = 0.0;
    EXPECT_OK(chat_request_body(standard, Dialect::openai, {})) << "presence_penalty is OpenAI's own";

    ChatRequest not_a_number = ask("x");
    not_a_number.temperature = std::numeric_limits<double>::quiet_NaN();
    EXPECT_ERRC(chat_request_body(not_a_number, Dialect::llama_server, {}), Errc::invalid_argument)
        << "NaN would be sent as null: the server's default, silently";
    ChatRequest effort_alone = ask("x");
    effort_alone.reasoning_effort = ReasoningEffort::low;
    EXPECT_ERRC(chat_request_body(effort_alone, Dialect::llama_server, {.thinking = true}),
                Errc::invalid_argument);

    ChatRequest overriding = ask("x");
    overriding.extra = {{"model", "other"}};
    EXPECT_ERRC(chat_request_body(overriding, Dialect::llama_server, {}), Errc::invalid_argument);
    overriding.extra = json::Json::array();
    EXPECT_ERRC(chat_request_body(overriding, Dialect::llama_server, {}), Errc::invalid_argument);
}

TEST(OpenAiProvider, IgnoresKeepAliveComments) {
    std::vector<std::string> stream = llama_server_stream();
    stream.insert(stream.begin() + 3, ": keep-alive\n\n");
    stream.insert(stream.begin(), ": keep-alive\n\n");
    const ModelServer server{[&stream](const auto&) { return stream_of(stream); }};
    EXPECT_EQ(unwrap(provider_for(server).chat(ask("hi"), {}, {})).content, "Paris");
}

TEST(OpenAiProvider, AnAnswerWithoutDoneIsIncomplete) {
    std::vector<std::string> stream = llama_server_stream();
    stream.pop_back();  // no [DONE]
    const ModelServer server{[&stream](const auto&) { return stream_of(stream); }};
    const auto answer = provider_for(server).chat(ask("hi"), {}, {});
    ASSERT_ERRC(answer, Errc::network) << "transient: the call is tried again";
    EXPECT_THAT(answer.error().message(), HasSubstr("without its [DONE]"));
}

TEST(OpenAiProvider, AnErrorEventIsTheCallsError) {
    // llama-server mid-answer: an error event, then the end, no [DONE].
    const ModelServer engine{[](const auto&) {
        return stream_of(
            {event(R"({"choices":[{"delta":{"content":"Pa"},"index":0}]})"),
             event(
                 R"({"error":{"code":500,"message":"Context size has been exceeded.","type":"server_error"}})")});
    }};
    const auto failed = provider_for(engine).chat(ask("hi"), {}, {});
    ASSERT_ERRC(failed, Errc::unavailable);
    EXPECT_THAT(failed.error().message(), HasSubstr("Context size has been exceeded."));

    // The runtime's relay: our structured error, with its exact class.
    const ModelServer relay{[](const auto&) {
        return stream_of(
            {event(R"({"error":{"code":"invalid_argument","message":"the prompt is too long"}})")});
    }};
    EXPECT_ERRC(provider_for(relay).chat(ask("hi"), {}, {}), Errc::invalid_argument);
}

TEST(OpenAiProvider, ErrorStatusesKeepTheirClass) {
    const ModelServer too_long{[](const auto&) {
        return Response::json(
            400,
            R"j({"error":{"code":400,"message":"request (40123 tokens) exceeds the available context size (32768 tokens)","type":"exceed_context_size_error"}})j");
    }};
    const auto answer = provider_for(too_long).chat(ask("hi"), {}, {});
    ASSERT_ERRC(answer, Errc::invalid_argument) << "not retried: it would fail again";
    EXPECT_THAT(answer.error().message(), HasSubstr("exceeds the available context size"));
    EXPECT_THAT(answer.error().describe(), HasSubstr("while calling 'commander' at runtime"));

    const ModelServer loading{[](const auto&) {
        return Response::json(
            503, R"({"error":{"code":503,"message":"Loading model","type":"unavailable_error"}})");
    }};
    EXPECT_ERRC(provider_for(loading).chat(ask("hi"), {}, {}), Errc::unavailable);

    const ModelServer not_a_stream{[](const auto&) { return Response::json(200, R"({"choices":[]})"); }};
    EXPECT_ERRC(provider_for(not_a_stream).chat(ask("hi"), {}, {}), Errc::unsupported);

    const ModelServer garbage{[](const auto&) { return stream_of({event("not json"), event("[DONE]")}); }};
    EXPECT_ERRC(provider_for(garbage).chat(ask("hi"), {}, {}), Errc::unsupported);
}

TEST(OpenAiProvider, ASilentCallTimesOutAndACancelledOneStopsAtOnce) {
    const ModelServer silent{[](const auto&) {
        return Response::event_stream([](net::HttpServer::ChunkWriter& writer) -> Result<void> {
            for (int tick = 0; tick < 100 && writer.write({}); ++tick) {
                std::this_thread::sleep_for(50ms);
            }
            return ok();
        });
    }};
    const auto timed_out = provider_for(silent, {.idle_timeout = 300ms}).chat(ask("hi"), {}, {});
    EXPECT_ERRC(timed_out, Errc::timeout);

    std::stop_source stop;
    const std::jthread canceller{[&stop] {
        std::this_thread::sleep_for(200ms);
        stop.request_stop();
    }};
    const auto started = Clock::now();
    EXPECT_ERRC(provider_for(silent).chat(ask("hi"), {}, stop.get_token()), Errc::cancelled);
    EXPECT_LT(Clock::now() - started, 2s);
}

}  // namespace
}  // namespace rr::llm
