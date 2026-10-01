// rr_fake_engine — a stand-in for llama-server in the supervisor's tests.
//
// It understands the part of llama-server's interface the supervisor uses:
//   --port P --alias ID          (other llama-server flags are ignored)
//   GET  /health                 503 "Loading model" until loaded, then 200
//   POST /v1/chat/completions    JSON, or SSE tokens ending in [DONE] with "stream": true
//   POST /v1/embeddings          a fixed vector
//   GET  /stats                  {"completions": N, "abandoned_streams": M,
//                                 "abandoned_requests": K}
//
// Process switches script the engine's life, passed like any other engine
// flag (the catalog's extra_args):
//   --fake-load-ms N        stay "loading" for N ms (default 100)
//   --fake-never-ready      never finish loading
//   --fake-exit-after-ms N  exit with code 3 after N ms (a crash)
//   --fake-token-ms N       pause between streamed tokens (default 20)
//
// A streamed answer is shaped as llama-server shapes it (rr/fake_engine/stream.hpp):
// a role chunk, reasoning then content pieces, a finish chunk, the usage when
// stream_options.include_usage asks for it, and [DONE].
//
// Knobs in a chat request's JSON body script one call:
//   "reply": "text"         answer this, in pieces of a few bytes (default:
//                           five tokens, tok0 … tok4)
//   "reply_by_turn": [...]  the n-th text for a conversation with n assistant
//                           turns so far (a model that fixes its answer)
//   "n": N                  the OpenAI field: N choices (at most 16), streamed
//                           interleaved, each with its finish and usage
//   "replies": [...]        with n: choice i answers replies[i] (default: the
//                           same answer for every choice)
//   "reasoning": "text"     think this first (delta.reasoning_content)
//   "max_tokens": N         the OpenAI field: at most N pieces, reasoning
//                           included ("length" when the answer is cut)
//   "ignore_eos": true      llama-server's field: with no reply and
//                           max_tokens, exactly N tokens ("length"), as a
//                           benchmark asks; without it the answer ends on
//                           its own, after five tokens
//   "error_after": N        stream N tokens, then an error event and the end
//                           with no [DONE], as llama-server reports a failure
//                           mid-answer
//   "fail": true            answer 400, as llama-server does for a bad request
//   "head_delay_ms": N      hold the status and headers back for N ms (a busy
//                           engine: llama-server answers when a slot is free);
//                           a client gone by then counts as an abandoned request
//   "first_token_ms": N     after the headers, stay silent for N ms (reading
//                           the prompt)
//   "break_after": N        stream N tokens, then break off (a crash mid-answer)
//   "break_mid_event": true with break_after: first send the first line of
//                           the next event, so the break falls inside it
//   "split_pause_ms": N     write each event in two halves, N ms apart
//   "echo_headers": true    (not streamed) answer with the request's headers,
//                           names in lower case
//
// While silent, the engine probes its client every 10 ms, as llama-server
// does, so a client that leaves is noticed (and counted) at once. It exits
// when its parent process goes away, so a crashed test leaves no orphans.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

#include <rr/core/strings.hpp>
#include <rr/fake_engine/stream.hpp>
#include <rr/json/json.hpp>
#include <rr/net/http_server.hpp>
#include <rr/net/sse.hpp>

namespace {

using namespace std::chrono_literals;
using rr::net::HttpServer;
using Response = HttpServer::Response;
using Clock = std::chrono::steady_clock;

struct Options {
    int port = 0;
    std::string alias = "fake";
    int load_ms = 100;
    bool never_ready = false;
    int exit_after_ms = -1;
    int token_ms = 20;
};

Options parse(const std::vector<std::string_view>& args) {
    Options options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        const auto next = [&]() -> std::string_view { return index + 1 < args.size() ? args[++index] : ""; };
        if (arg == "--port") {
            options.port = std::stoi(std::string{next()});
        } else if (arg == "--alias") {
            options.alias = std::string{next()};
        } else if (arg == "--fake-load-ms") {
            options.load_ms = std::stoi(std::string{next()});
        } else if (arg == "--fake-never-ready") {
            options.never_ready = true;
        } else if (arg == "--fake-exit-after-ms") {
            options.exit_after_ms = std::stoi(std::string{next()});
        } else if (arg == "--fake-token-ms") {
            options.token_ms = std::stoi(std::string{next()});
        }
    }
    return options;
}

// A knob from the request body, or `fallback` when absent or of another type.
template <class T>
T knob(const rr::json::Json& body, const char* name, T fallback) {
    const auto found = body.find(name);
    if (found == body.end()) {
        return fallback;
    }
    if constexpr (std::is_same_v<T, bool>) {
        return found->is_boolean() ? found->get<bool>() : fallback;
    } else {
        return found->is_number_integer() ? found->get<T>() : fallback;
    }
}

// Waits `duration`, probing the client every 10 ms. False once it has left.
bool wait_for(HttpServer::ChunkWriter& writer, std::chrono::milliseconds duration) {
    const auto until = Clock::now() + duration;
    while (Clock::now() < until) {
        if (!writer.write({})) {
            return false;
        }
        std::this_thread::sleep_for(std::min<Clock::duration>(10ms, until - Clock::now()));
    }
    return writer.write({});
}

struct StreamScript {
    int first_token_ms = 0;
    int break_after = -1;
    bool break_mid_event = false;
    int split_pause_ms = 0;
    int error_after = -1;
};

// `text` in pieces of at most four bytes, never splitting a UTF-8 sequence.
std::vector<std::string> pieces(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = std::min(start + 4, text.size());
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            ++end;
        }
        out.emplace_back(text.substr(start, end - start));
        start = end;
    }
    return out;
}

// The answer for one choice: `reply` (else the model's own, five tokens, or
// max_tokens of them with ignore_eos), cut at max_tokens.
rr::fake_engine::ChoicePlan choice_plan(const std::optional<std::string>& reply,
                                        const std::vector<std::string>& reasoning,
                                        const rr::json::Json& body) {
    rr::fake_engine::ChoicePlan choice{.reasoning = reasoning};
    const int max_tokens = knob(body, "max_tokens", -1);
    if (reply.has_value()) {
        choice.content = pieces(*reply);
    } else {
        // The model's own answer is five tokens. ignore_eos makes it go on past
        // its end, to exactly max_tokens: a benchmark's fixed-length run.
        const bool run_to_limit = knob(body, "ignore_eos", false) && max_tokens >= 0;
        const int count = run_to_limit ? std::min(max_tokens, 100'000) : 5;
        for (int index = 0; index < count; ++index) {
            choice.content.push_back(std::format("tok{}", index));
        }
        if (run_to_limit) {
            choice.finish_reason = "length";
        }
    }
    if (max_tokens >= 0) {
        const auto limit = static_cast<std::size_t>(max_tokens);
        if (choice.reasoning.size() + choice.content.size() > limit) {
            choice.finish_reason = "length";
            if (choice.reasoning.size() >= limit) {
                choice.reasoning.resize(limit);
                choice.content.clear();
            } else {
                choice.content.resize(limit - choice.reasoning.size());
            }
        }
    }
    return choice;
}

// What to stream for `body` (see the knobs above).
rr::fake_engine::StreamPlan plan_for(const rr::json::Json& body, const Options& options) {
    rr::fake_engine::StreamPlan plan{.model = options.alias, .token_ms = options.token_ms};
    if (const auto stream_options = body.find("stream_options");
        stream_options != body.end() && stream_options->is_object()) {
        plan.include_usage = knob(*stream_options, "include_usage", false);
    }
    // About four characters per token, as for English text.
    std::size_t characters = 0;
    std::size_t assistant_turns = 0;
    if (const auto messages = body.find("messages"); messages != body.end() && messages->is_array()) {
        for (const rr::json::Json& message : *messages) {
            if (message.is_object() && message.value("role", "") == "assistant") {
                ++assistant_turns;
            }
            const auto content = message.is_object() ? message.find("content") : message.end();
            if (content == message.end()) {
                continue;
            }
            if (content->is_string()) {
                characters += content->get<std::string>().size();
            } else if (content->is_array()) {
                for (const rr::json::Json& part : *content) {
                    if (part.is_object() && part.value("type", "") == "text") {
                        characters += part.value("text", "").size();
                    }
                }
            }
        }
    }
    plan.prompt_tokens = std::max<std::int64_t>(1, static_cast<std::int64_t>(characters / 4));

    std::optional<std::string> reply;
    if (const auto text = body.find("reply"); text != body.end() && text->is_string()) {
        reply = text->get<std::string>();
    } else if (const auto turns = body.find("reply_by_turn");
               turns != body.end() && turns->is_array() && !turns->empty()) {
        const rr::json::Json& chosen = (*turns)[std::min(assistant_turns, turns->size() - 1)];
        if (chosen.is_string()) {
            reply = chosen.get<std::string>();
        }
    }
    std::vector<std::string> reasoning;
    if (const auto thought = body.find("reasoning"); thought != body.end() && thought->is_string()) {
        reasoning = pieces(thought->get<std::string>());
    }
    const auto replies = body.find("replies");
    const int choices = std::clamp(knob(body, "n", 1), 1, 16);
    plan.choices.clear();
    for (int index = 0; index < choices; ++index) {
        std::optional<std::string> own = reply;
        if (replies != body.end() && replies->is_array() && std::cmp_less(index, replies->size()) &&
            (*replies)[static_cast<std::size_t>(index)].is_string()) {
            own = (*replies)[static_cast<std::size_t>(index)].get<std::string>();
        }
        plan.choices.push_back(choice_plan(own, reasoning, body));
    }
    return plan;
}

rr::Result<void> stream_answer(HttpServer::ChunkWriter& writer, const rr::fake_engine::StreamPlan& plan,
                               const StreamScript& script, std::atomic<int>& abandoned_streams) {
    const auto left = [&abandoned_streams]() -> rr::Result<void> {
        ++abandoned_streams;
        return rr::fail(rr::Errc::cancelled, "the client went away");
    };
    // Writes one event, in two halves when asked to (a pause mid-line).
    const auto send = [&](std::string_view data) {
        const std::string event = rr::net::format_sse_event(data);
        if (script.split_pause_ms <= 0) {
            return writer.write(event);
        }
        const std::size_t half = event.size() / 2;
        return writer.write(std::string_view{event}.substr(0, half)) &&
               wait_for(writer, std::chrono::milliseconds{script.split_pause_ms}) &&
               writer.write(std::string_view{event}.substr(half));
    };

    if (!wait_for(writer, std::chrono::milliseconds{script.first_token_ms})) {  // reading the prompt
        return left();
    }
    // Role chunks and tokens, the choices interleaved; tokens are counted for
    // break_after and error_after, and paced.
    int tokens_sent = 0;
    for (const rr::fake_engine::Scheduled& event : rr::fake_engine::body_events(plan)) {
        if (event.token && tokens_sent == script.break_after) {
            if (script.break_mid_event) {
                const std::string text = rr::net::format_sse_event(event.data);
                (void)writer.write(std::string_view{text}.substr(0, text.find('\n') + 1));  // one whole line
            }
            return rr::fail(rr::Errc::internal, "the fake engine broke off");  // truncates the transfer
        }
        if (event.token && tokens_sent == script.error_after) {
            (void)send(
                R"({"error":{"code":500,"message":"the fake engine failed mid-answer","type":"server_error"}})");
            return rr::ok();  // llama-server ends the stream after an error, without [DONE]
        }
        if (!send(event.data)) {
            return left();
        }
        if (event.token) {
            ++tokens_sent;
            if (!wait_for(writer, std::chrono::milliseconds{plan.token_ms})) {
                return left();
            }
        }
    }
    // Each choice's finish chunk, and its usage written with it.
    for (std::size_t index = 0; index < plan.choices.size(); ++index) {
        if (!send(rr::fake_engine::finish_event(plan, index)) ||
            (plan.include_usage && !send(rr::fake_engine::usage_event(plan, index)))) {
            return left();
        }
    }
    if (!send("[DONE]")) {
        return left();
    }
    return rr::ok();
}

// Exits once the parent is gone (the process was reparented).
void watch_parent() {
    const pid_t parent = ::getppid();
    while (::getppid() == parent) {
        std::this_thread::sleep_for(200ms);
    }
    std::_Exit(0);
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(std::vector<std::string_view>(argv + 1, argv + argc));
    const auto started = Clock::now();
    std::atomic<int> completions{0};
    std::atomic<int> abandoned_streams{0};
    std::atomic<int> abandoned_requests{0};

    HttpServer server{HttpServer::Options{.port = options.port}};
    server.route(rr::net::Method::get, "/health", [&](const auto&) {
        const bool loaded =
            !options.never_ready && Clock::now() - started >= std::chrono::milliseconds{options.load_ms};
        return loaded ? Response::json(200, R"({"status":"ok"})")
                      : Response::json(
                            503,
                            R"({"error":{"code":503,"message":"Loading model","type":"unavailable_error"}})");
    });
    server.route(rr::net::Method::get, "/stats", [&](const auto&) {
        return Response::json(
            200, std::format(R"({{"completions":{},"abandoned_streams":{},"abandoned_requests":{}}})",
                             completions.load(), abandoned_streams.load(), abandoned_requests.load()));
    });
    server.route(rr::net::Method::post, "/v1/chat/completions", [&](const HttpServer::Request& request) {
        ++completions;
        const auto body = rr::json::parse(request.body);
        if (!body.has_value() || !body->is_object()) {
            return Response::json(
                400, R"({"error":{"code":400,"message":"invalid JSON","type":"invalid_request_error"}})");
        }
        if (const int delay = knob(*body, "head_delay_ms", 0); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{delay});
            if (request.client_gone && request.client_gone()) {
                ++abandoned_requests;  // the caller gave up while this engine was busy
                return Response::json(503, R"({"error":{"code":503,"message":"nobody is listening"}})");
            }
        }
        if (knob(*body, "fail", false)) {
            return Response::json(
                400,
                R"({"error":{"code":400,"message":"the fake engine was asked to fail","type":"invalid_request_error"}})");
        }
        if (knob(*body, "echo_headers", false)) {
            rr::json::Json headers = rr::json::Json::object();
            for (const auto& [name, value] : request.headers) {
                headers[rr::to_lower_ascii(name)] = value;  // names are case-insensitive; JSON keys are not
            }
            return Response::json(200, rr::json::dump(rr::json::Json{{"headers", std::move(headers)}}));
        }
        if (!knob(*body, "stream", false)) {
            return Response::json(
                200,
                std::format(
                    R"({{"model":"{}","choices":[{{"message":{{"role":"assistant","content":"hello from {}"}}}}]}})",
                    options.alias, options.alias));
        }
        const StreamScript script{.first_token_ms = knob(*body, "first_token_ms", 0),
                                  .break_after = knob(*body, "break_after", -1),
                                  .break_mid_event = knob(*body, "break_mid_event", false),
                                  .split_pause_ms = knob(*body, "split_pause_ms", 0),
                                  .error_after = knob(*body, "error_after", -1)};
        return Response::event_stream(
            [&abandoned_streams, script, plan = plan_for(*body, options)](HttpServer::ChunkWriter& writer) {
                return stream_answer(writer, plan, script, abandoned_streams);
            });
    });
    server.route(rr::net::Method::post, "/v1/embeddings", [](const auto&) {
        return Response::json(200, R"({"data":[{"embedding":[0.1,0.2,0.3],"index":0}]})");
    });

    if (!server.start().has_value()) {
        (void)std::fprintf(stderr, "fake engine: cannot listen on port %d\n", options.port);
        return 2;
    }
    (void)std::printf("fake engine '%s' listening on port %d\n", options.alias.c_str(), server.port());
    (void)std::fflush(stdout);

    const std::jthread parent_watch{watch_parent};
    // Run until killed (SIGTERM ends the process by default), or crash on cue.
    if (options.exit_after_ms >= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{options.exit_after_ms});
        (void)std::fprintf(stderr, "fake engine: simulated crash\n");
        std::_Exit(3);
    }
    while (true) {
        std::this_thread::sleep_for(1h);
    }
}
