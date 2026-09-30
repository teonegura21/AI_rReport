#include <rr/llm/replay.hpp>

#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include <rr/core/file_io.hpp>

namespace rr::llm {

namespace {

// Recordings are text answers; a file this large is not one of ours.
constexpr std::size_t kMaxRecordingBytes = std::size_t{64} << 20U;

json::Json response_to_json(const ChatResponse& response) {
    return {{"content", response.content},
            {"reasoning", response.reasoning},
            {"finish_reason", to_string(response.finish)},
            {"model", response.model},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"completion_tokens", response.usage.completion_tokens},
              {"cached_prompt_tokens", response.usage.cached_prompt_tokens}}}};
}

Result<ChatResponse> response_from_json(const json::Json& document) {
    ChatResponse response;
    RR_TRY_ASSIGN(response.content, json::get_string(document, "content"));
    RR_TRY_ASSIGN(response.reasoning, json::get_string(document, "reasoning"));
    RR_TRY_ASSIGN(const std::string finish, json::get_string(document, "finish_reason"));
    response.finish = finish_reason_from_string(finish);
    RR_TRY_ASSIGN(response.model, json::get_string(document, "model"));
    RR_TRY_ASSIGN(response.usage.prompt_tokens, json::get_int(document, "usage.prompt_tokens"));
    RR_TRY_ASSIGN(response.usage.completion_tokens, json::get_int(document, "usage.completion_tokens"));
    RR_TRY_ASSIGN(response.usage.cached_prompt_tokens, json::get_int(document, "usage.cached_prompt_tokens"));
    return response;
}

// The response stored in a recording file's text, checked to be filed under `key`.
Result<ChatResponse> parse_recording(std::string_view text, std::string_view key) {
    RR_TRY_ASSIGN(const json::Json document, json::parse(text));
    RR_TRY_ASSIGN(const std::string recorded_key, json::get_string(document, "key"));
    if (recorded_key != key) {
        return fail(Errc::integrity, std::format("it records the call {}", recorded_key));
    }
    RR_TRY_ASSIGN(const json::Json* response, json::find(document, "response"));
    return response_from_json(*response);
}

}  // namespace

std::filesystem::path Recordings::path_for(std::string_view key) const {
    return directory_ / std::format("{}.json", key);
}

Result<ChatResponse> Recordings::load(std::string_view key) const {
    const std::filesystem::path file = path_for(key);
    auto text = read_file(file, kMaxRecordingBytes);
    if (!text.has_value()) {
        if (text.error().code() == Errc::not_found) {
            return fail(Errc::not_found,
                        std::format("no recording of this call ({}) in '{}'; record it first", key,
                                    directory_.string()));
        }
        return fail(std::move(text).error());
    }
    auto response = parse_recording(*text, key);
    if (!response.has_value()) {
        return fail(Errc::integrity, std::format("'{}' is not a usable recording: {}", file.string(),
                                                 response.error().message()));
    }
    return response;
}

Result<void> Recordings::store(std::string_view provider, const ChatRequest& request,
                               const ChatResponse& response) const {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        return fail(Errc::io, std::format("cannot create '{}': {}", directory_.string(), error.message()));
    }
    const std::string key = request_key(provider, request);
    const json::Json recording{{"key", key},
                               {"provider", provider},
                               {"request", to_json(request)},
                               {"response", response_to_json(response)}};
    return write_file_atomically(path_for(key), json::dump_pretty(recording) + "\n");
}

RecordingProvider::RecordingProvider(const LlmProvider& inner, Recordings recordings)
    : inner_(&inner)
    , recordings_(std::move(recordings)) {}

Result<ChatResponse> RecordingProvider::chat(const ChatRequest& request, const StreamSink& sink,
                                             const std::stop_token& stop) const {
    auto response = inner_->chat(request, sink, stop);
    if (response.has_value()) {
        // Recording is the point of this mode: a recording that cannot be
        // written fails the call, so a missing file cannot go unnoticed.
        RR_TRY(with_context(recordings_.store(inner_->name(), request, *response),
                            "while recording the answer"));
    }
    return response;
}

ReplayProvider::ReplayProvider(std::string name, ProviderCaps caps, Recordings recordings)
    : name_(std::move(name))
    , caps_(caps)
    , recordings_(std::move(recordings)) {}

Result<ChatResponse> ReplayProvider::chat(const ChatRequest& request, const StreamSink& sink,
                                          const std::stop_token& stop) const {
    if (stop.stop_requested()) {
        return fail(Errc::cancelled, "the call was cancelled");
    }
    RR_TRY_ASSIGN(ChatResponse response, recordings_.load(request_key(name_, request)));
    if (!response.reasoning.empty() && sink.on_reasoning) {
        sink.on_reasoning(response.reasoning);
    }
    if (!response.content.empty() && sink.on_content) {
        sink.on_content(response.content);
    }
    response.first_token = std::chrono::milliseconds{0};
    response.duration = std::chrono::milliseconds{0};
    return response;
}

}  // namespace rr::llm
