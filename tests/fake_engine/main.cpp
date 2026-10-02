// rr_fake_engine — a stand-in for llama-server in the supervisor's tests.
//
// It understands the part of llama-server's interface the supervisor uses:
//   --port P --alias ID          (other llama-server flags are ignored, but
//   --parallel N --ctx-size C     these shape /props, as they shape a real
//   --kv-unified                  engine's slots)
//   GET  /health                 503 "Loading model" until loaded, then 200
//   GET  /props                  {"default_generation_settings": {"n_ctx": per slot},
//                                 "total_slots": N, …}: C / N, or C with --kv-unified
//   POST /v1/chat/completions    JSON, or SSE tokens ending in [DONE] with "stream": true;
//                                a prompt of a slot's context or more (about four
//                                characters a token) is refused with 400, as llama-server
//                                refuses it ("exceed_context_size_error")
//   POST /v1/embeddings          a fixed vector
//   GET  /stats                  {"completions": N, "abandoned_streams": M,
//                                 "abandoned_requests": K, "hung": H}
//
// Process switches script the engine's life, passed like any other engine
// flag (the catalog's extra_args):
//   --fake-load-ms N        stay "loading" for N ms (default 100)
//   --fake-never-ready      never finish loading
//   --fake-exit-after-ms N  exit with code 3 after N ms (a crash)
//   --fake-token-ms N       pause between streamed tokens (default 20)
//   --fake-slot-context N   /props reports N tokens per slot, whatever the
//                           arithmetic (llama.cpp capping it at the model's
//                           training context)
//   --fake-no-props         no /props (404), as from a server that cannot say
//   --fake-props-ms N       answer /props after N ms
//   --fake-prompt-cache     reuse prompt prefixes, as llama-server does by
//                           default: a prompt's longest common prefix with
//                           the last 16 read is "cached" (usage
//                           prompt_tokens_details.cached_tokens, timings
//                           cache_n), at least its last token read again;
//                           "cache_prompt": false in a request opts out.
//                           Off by default, so other tests see no cache
//   --fake-oracle           answer every chat call as the oracle does
//                           (rr/oracle/answers.hpp), which knows each of the
//                           pipeline's missions and its right answer from the
//                           request alone: the end-to-end tests run the real
//                           pipeline on it (plan P22). A mission is known by
//                           its answer schema's description, which names its
//                           prompt ("… (prompts/atomize)"), or, answered in
//                           prose, by its task's first line; one the oracle
//                           does not know is refused with 400. Every choice
//                           answers the same: an oracle has one right answer.
//                           Only where the hub is built, with the oracle
//   --fake-hang-after N     answer N chat calls, then hang every later one:
//                           its head sent, nothing more, its client probed
//                           every 10 ms until it leaves (counted in /stats'
//                           "hung"). A job killed while waiting on one has
//                           nothing in progress on this engine
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <mutex>
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

#if defined(RR_FAKE_ENGINE_ORACLE)
#include <memory>

#include <rr/llm/chat.hpp>
#include <rr/oracle/answers.hpp>
#include <rr/oracle/oracle.hpp>
#endif

namespace {

using namespace std::chrono_literals;
using rr::net::HttpServer;
using Response = HttpServer::Response;
using Clock = std::chrono::steady_clock;

struct Options {
    int port = 0;
    std::string alias = "fake";
    int parallel = 1;
    int context = 4096;
    bool kv_unified = false;
    int load_ms = 100;
    bool never_ready = false;
    int exit_after_ms = -1;
    int token_ms = 20;
    int slot_context = -1;  // -1: by the arithmetic
    bool no_props = false;
    int props_ms = 0;
    bool prompt_cache = false;
    bool oracle = false;
    int hang_after = -1;  // -1: never
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
        } else if (arg == "--parallel") {
            options.parallel = std::max(1, std::stoi(std::string{next()}));
        } else if (arg == "--ctx-size") {
            options.context = std::stoi(std::string{next()});
        } else if (arg == "--kv-unified" || arg == "--no-kv-unified") {
            options.kv_unified = arg == "--kv-unified";
        } else if (arg == "--fake-load-ms") {
            options.load_ms = std::stoi(std::string{next()});
        } else if (arg == "--fake-never-ready") {
            options.never_ready = true;
        } else if (arg == "--fake-exit-after-ms") {
            options.exit_after_ms = std::stoi(std::string{next()});
        } else if (arg == "--fake-token-ms") {
            options.token_ms = std::stoi(std::string{next()});
        } else if (arg == "--fake-slot-context") {
            options.slot_context = std::stoi(std::string{next()});
        } else if (arg == "--fake-no-props") {
            options.no_props = true;
        } else if (arg == "--fake-props-ms") {
            options.props_ms = std::stoi(std::string{next()});
        } else if (arg == "--fake-prompt-cache") {
            options.prompt_cache = true;
        } else if (arg == "--fake-oracle") {
            options.oracle = true;
        } else if (arg == "--fake-hang-after") {
            options.hang_after = std::stoi(std::string{next()});
        }
    }
    return options;
}

// The most tokens one sequence may hold: the whole pool when the slots share
// it, an equal share otherwise (or what --fake-slot-context says).
int slot_context(const Options& options) {
    if (options.slot_context >= 0) {
        return options.slot_context;
    }
    return options.kv_unified ? options.context : options.context / options.parallel;
}

// llama-server's /props, the part the supervisor reads.
std::string props(const Options& options) {
    return rr::json::dump(rr::json::Json{{"default_generation_settings", {{"n_ctx", slot_context(options)}}},
                                         {"total_slots", options.parallel},
                                         {"model_alias", options.alias},
                                         {"build_info", "fake"}});
}

// The prompts read lately, whose prefixes a new prompt may reuse.
class PromptCache {
public:
    // The tokens of `prompt` (`tokens` in all) served from the cache, about
    // four characters each; at least the last token is read again, as
    // llama-server reads it for its logits. Remembers `prompt`.
    std::int64_t reuse(const std::string& prompt, std::int64_t tokens) {
        const std::lock_guard lock(mutex_);
        std::size_t longest = 0;
        for (const std::string& earlier : recent_) {
            const auto [mismatch, unused] = std::ranges::mismatch(prompt, earlier);
            longest = std::max(longest, static_cast<std::size_t>(mismatch - prompt.begin()));
        }
        recent_.push_front(prompt);
        if (recent_.size() > 16) {
            recent_.pop_back();
        }
        return std::clamp<std::int64_t>(static_cast<std::int64_t>(longest / 4), 0, tokens - 1);
    }

private:
    std::mutex mutex_;
    std::deque<std::string> recent_;
};

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

// A message's text: its content, or its text parts joined.
std::string message_text(const rr::json::Json& message) {
    const auto content = message.find("content");
    if (content == message.end()) {
        return {};
    }
    if (content->is_string()) {
        return content->get<std::string>();
    }
    std::string text;
    if (content->is_array()) {
        for (const rr::json::Json& part : *content) {
            if (part.is_object() && part.value("type", "") == "text") {
                text += part.value("text", "");
            }
        }
    }
    return text;
}

// The prompt, as far as the fake reads it.
struct Prompt {
    std::string text;            // each message's role and text, in order: what a prompt cache compares
    std::size_t characters = 0;  // of the messages' texts
    std::size_t assistant_turns = 0;
};

Prompt read_prompt(const rr::json::Json& body) {
    Prompt prompt;
    const auto messages = body.find("messages");
    if (messages == body.end() || !messages->is_array()) {
        return prompt;
    }
    for (const rr::json::Json& message : *messages) {
        if (!message.is_object()) {
            continue;
        }
        const std::string role = message.value("role", "");
        if (role == "assistant") {
            ++prompt.assistant_turns;
        }
        const std::string text = message_text(message);
        prompt.text += std::format("<{}>\n{}\n", role, text);
        prompt.characters += text.size();
    }
    return prompt;
}

#if defined(RR_FAKE_ENGINE_ORACLE)
// `body` as the oracle reads a request: its messages, in order.
rr::llm::ChatRequest oracle_request(const rr::json::Json& body) {
    rr::llm::ChatRequest request;
    const auto messages = body.find("messages");
    if (messages == body.end() || !messages->is_array()) {
        return request;
    }
    for (const rr::json::Json& message : *messages) {
        if (!message.is_object()) {
            continue;
        }
        const std::string role = message.value("role", "");
        std::string text = message_text(message);
        if (role == "system") {
            request.messages.push_back(rr::llm::Message::system(std::move(text)));
        } else if (role == "assistant") {
            request.messages.push_back(rr::llm::Message::assistant(std::move(text)));
        } else {
            request.messages.push_back(rr::llm::Message::user(std::move(text)));
        }
    }
    return request;
}

// The kind of mission `request` (from `body`) is, as the oracle knows kinds
// (rr/oracle/answers.hpp): its answer schema's description names its prompt
// ("… (prompts/atomize)"); an answer in prose has no schema, and its task,
// the first user message, opens with a line that says which.
rr::Result<std::string> mission_kind(const rr::json::Json& body, const rr::llm::ChatRequest& request) {
    if (const auto format = body.find("response_format"); format != body.end() && format->is_object()) {
        const rr::json::Json* schema = nullptr;
        if (const auto described = format->find("json_schema");
            described != format->end() && described->is_object()) {
            if (const auto found = described->find("schema");
                found != described->end() && found->is_object()) {
                schema = &*found;
            }
        }
        const std::string description = schema == nullptr ? std::string{} : schema->value("description", "");
        constexpr std::string_view kOpen = "(prompts/";
        const std::size_t open = description.find(kOpen);
        const std::size_t close = open == std::string::npos ? open : description.find(')', open);
        if (close == std::string::npos) {
            return rr::fail(rr::Errc::invalid_argument,
                            "the answer's schema names no prompt: the oracle knows a mission by its prompt");
        }
        return description.substr(open + kOpen.size(), close - open - kOpen.size());
    }
    const auto task =
        std::ranges::find(request.messages, rr::llm::MessageRole::user, &rr::llm::Message::role);
    const std::string opening = task == request.messages.end() ? std::string{} : task->text();
    if (opening.starts_with("Answer with the section's paragraphs only")) {
        return std::string{"write"};
    }
    if (opening.starts_with("Answer with the two paragraphs only")) {
        return std::string{"seam"};
    }
    return rr::fail(rr::Errc::invalid_argument,
                    std::format("the oracle knows no mission answered in prose whose task opens \"{}\"",
                                opening.substr(0, opening.find('\n'))));
}

// What the oracle answers `body`: every choice the same.
rr::Result<std::string> oracle_answer(const rr::oracle::Oracle& oracle, const rr::json::Json& body) {
    rr::llm::ChatRequest request = oracle_request(body);
    RR_TRY_ASSIGN(request.tag.purpose, mission_kind(body, request));
    RR_TRY_ASSIGN(const rr::llm::ChatResponse answered, oracle.chat(request, {}, {}));
    return answered.first().content;
}
#endif

// What to stream for `body` (see the knobs above).
rr::fake_engine::StreamPlan plan_for(const rr::json::Json& body, const Prompt& prompt,
                                     const Options& options) {
    rr::fake_engine::StreamPlan plan{.model = options.alias, .token_ms = options.token_ms};
    if (const auto stream_options = body.find("stream_options");
        stream_options != body.end() && stream_options->is_object()) {
        plan.include_usage = knob(*stream_options, "include_usage", false);
    }
    // About four characters per token, as for English text.
    plan.prompt_tokens = std::max<std::int64_t>(1, static_cast<std::int64_t>(prompt.characters / 4));
    const std::size_t assistant_turns = prompt.assistant_turns;

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
    std::atomic<int> hung{0};
    PromptCache cache;
#if defined(RR_FAKE_ENGINE_ORACLE)
    std::unique_ptr<rr::oracle::Oracle> oracle;
    if (options.oracle) {
        oracle = std::make_unique<rr::oracle::Oracle>();
        rr::oracle::teach_stages(*oracle);
    }
#else
    if (options.oracle) {
        (void)std::fprintf(stderr, "fake engine: --fake-oracle needs the oracle, built with the hub\n");
        return 2;
    }
#endif

    HttpServer server{HttpServer::Options{.port = options.port}};
    server.route(rr::net::Method::get, "/health", [&](const auto&) {
        const bool loaded =
            !options.never_ready && Clock::now() - started >= std::chrono::milliseconds{options.load_ms};
        return loaded ? Response::json(200, R"({"status":"ok"})")
                      : Response::json(
                            503,
                            R"({"error":{"code":503,"message":"Loading model","type":"unavailable_error"}})");
    });
    if (!options.no_props) {
        server.route(rr::net::Method::get, "/props", [&options](const auto&) {
            std::this_thread::sleep_for(std::chrono::milliseconds{options.props_ms});
            return Response::json(200, props(options));
        });
    }
    server.route(rr::net::Method::get, "/stats", [&](const auto&) {
        return Response::json(
            200, std::format(
                     R"({{"completions":{},"abandoned_streams":{},"abandoned_requests":{},"hung":{}}})",
                     completions.load(), abandoned_streams.load(), abandoned_requests.load(), hung.load()));
    });
    server.route(rr::net::Method::post, "/v1/chat/completions", [&](const HttpServer::Request& request) {
        const int call = ++completions;
        auto body = rr::json::parse(request.body);
        if (!body.has_value() || !body->is_object()) {
            return Response::json(
                400, R"({"error":{"code":400,"message":"invalid JSON","type":"invalid_request_error"}})");
        }
        if (options.hang_after >= 0 && call > options.hang_after) {
            ++hung;
            return Response::event_stream([](HttpServer::ChunkWriter& writer) -> rr::Result<void> {
                while (writer.write({})) {  // nothing but probes, until the client leaves
                    std::this_thread::sleep_for(10ms);
                }
                return rr::fail(rr::Errc::cancelled, "the client went away");
            });
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
        const Prompt prompt = read_prompt(*body);
#if defined(RR_FAKE_ENGINE_ORACLE)
        if (oracle != nullptr) {
            auto answered = oracle_answer(*oracle, *body);
            if (!answered.has_value()) {
                return Response::json(400,
                                      rr::json::dump(rr::json::Json{{"error",
                                                                     {{"code", 400},
                                                                      {"message", answered.error().message()},
                                                                      {"type", "invalid_request_error"}}}}));
            }
            (*body)["reply"] = *std::move(answered);  // every choice's (see plan_for())
        }
#endif
        rr::fake_engine::StreamPlan plan = plan_for(*body, prompt, options);
        // As llama-server does, a prompt its slot cannot hold is refused before
        // any token.
        if (const int context = slot_context(options); plan.prompt_tokens >= context) {
            return Response::json(
                400, rr::json::dump(rr::json::Json{
                         {"error",
                          {{"code", 400},
                           {"message", "the request exceeds the available context size, try increasing it"},
                           {"type", "exceed_context_size_error"},
                           {"n_prompt_tokens", plan.prompt_tokens},
                           {"n_ctx", context}}}}));
        }
        if (options.prompt_cache && knob(*body, "cache_prompt", true)) {
            plan.cached_tokens = cache.reuse(prompt.text, plan.prompt_tokens);
        }
        return Response::event_stream(
            [&abandoned_streams, script, plan = std::move(plan)](HttpServer::ChunkWriter& writer) {
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
