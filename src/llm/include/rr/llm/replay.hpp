// rr/llm/replay.hpp — answers recorded once, then replayed without a model.
//
// Why this exists
//   Pipeline tests (plan §8, layer L3) run whole videos through every stage.
//   Against live models they would need a GPU, be slow, and differ from run
//   to run. Instead, answers are recorded once against real models and kept
//   next to the tests. Replaying them makes a pipeline run fast and exactly
//   repeatable, and a changed prompt shows up as a missing recording, not as
//   silently different output.
//
//   * A recording is one JSON file per call, named by the call's request key
//     (rr/llm/chat.hpp): <directory>/<key>.json. It holds the request too, so
//     people can read a recording and review its diff.
//   * RecordingProvider calls a live provider and records each successful
//     answer. Failures are not recorded: a replayed transient failure would
//     make a test depend on luck.
//   * ReplayProvider answers from recordings only. A request with no
//     recording fails with Errc::not_found and names the key; a test never
//     makes a live call by accident.
//   * Recordings are written atomically, so concurrent calls and an
//     interrupted recording run never leave half a file.
//
// Thread-safety: Recordings, RecordingProvider and ReplayProvider are safe to
//   use from many threads.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <rr/core/result.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/provider.hpp>

namespace rr::llm {

class Recordings {
public:
    explicit Recordings(std::filesystem::path directory) : directory_(std::move(directory)) {}

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] std::filesystem::path path_for(std::string_view key) const;

    // The answer recorded under `key`: Errc::not_found if there is none,
    // Errc::integrity if the file is not a recording.
    [[nodiscard]] Result<ChatResponse> load(std::string_view key) const;

    // Records `response` as the answer `provider` gave to `request`,
    // replacing any earlier recording of the same call.
    [[nodiscard]] Result<void> store(std::string_view provider, const ChatRequest& request,
                                     const ChatResponse& response) const;

private:
    std::filesystem::path directory_;
};

class RecordingProvider final : public LlmProvider {
public:
    // `inner` must outlive this provider. The directory is created on demand.
    RecordingProvider(const LlmProvider& inner, Recordings recordings);

    [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
    [[nodiscard]] ProviderCaps caps() const noexcept override { return inner_->caps(); }
    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& stop) const override;

private:
    const LlmProvider* inner_;
    Recordings recordings_;
};

class ReplayProvider final : public LlmProvider {
public:
    // Stands in for the provider called `name` (keys include the provider's
    // name, so it must be the recording provider's name).
    ReplayProvider(std::string name, ProviderCaps caps, Recordings recordings);

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] ProviderCaps caps() const noexcept override { return caps_; }
    // The recorded answer: its reasoning and content reach the sink whole.
    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& stop) const override;

private:
    std::string name_;
    ProviderCaps caps_;
    Recordings recordings_;
};

}  // namespace rr::llm
