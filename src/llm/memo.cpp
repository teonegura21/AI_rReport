#include <rr/llm/memo.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/json/json.hpp>
#include <rr/log/log.hpp>

namespace rr::llm {

bool may_sample(const ChatRequest& request) noexcept {
    return !request.temperature.has_value() || *request.temperature > 0.0;
}

std::string memo_key(std::string_view provider, const ChatRequest& request, std::string_view fingerprint) {
    return json::content_hash(
               json::Json{{"memo", 1}, {"request", request_key(provider, request)}, {"model", fingerprint}})
        .hex();
}

std::int64_t derived_seed(std::int64_t salt, std::string_view stage, std::string_view mission, int sample) {
    RR_ASSERT(sample >= 0, "a sample's index is 0 or more");
    // The digest is kept: bytes() is a view into it.
    const Blake3Digest digest =
        json::content_hash(json::Json{{"salt", salt}, {"stage", stage}, {"mission", mission}});
    // Its first eight bytes, little-endian: the same on every machine.
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(digest.bytes()[index])) << (8 * index);
    }
    const auto base = static_cast<std::int64_t>(value & 0x7FFF'FFFFULL);  // below 2^31
    return base + sample;
}

MemoProvider::MemoProvider(const LlmProvider& inner, MemoStore& store, ModelFingerprint fingerprint)
    : inner_(&inner)
    , store_(&store)
    , fingerprint_(std::move(fingerprint)) {}

Result<ChatResponse> MemoProvider::chat(const ChatRequest& request, const StreamSink& sink,
                                        const std::stop_token& stop) const {
    if (may_sample(request) && !request.seed.has_value()) {
        return fail(
            Errc::invalid_argument,
            std::format("a request to '{}' may sample and has no seed: a job's requests carry derived "
                        "seeds (ADR-0020), or one remembered sample would answer for all the others",
                        request.model));
    }
    RR_TRY_ASSIGN(const std::string fingerprint,
                  with_context(fingerprint_(request.model),
                               std::format("naming the model behind '{}'", request.model)));
    const std::string key = memo_key(inner_->name(), request, fingerprint);
    // A memo that cannot be read is a fault to report, not a reason to pay
    // for the call again unnoticed.
    RR_TRY_ASSIGN(std::optional<ChatResponse> kept,
                  with_context(store_->find(key), "reading the job's memo"));
    if (kept.has_value()) {
        ++remembered_;
        for (std::size_t index = 0; index < kept->choices.size(); ++index) {
            const Choice& choice = kept->choices[index];
            if (!choice.reasoning.empty() && sink.on_reasoning) {
                sink.on_reasoning(index, choice.reasoning);
            }
            if (!choice.content.empty() && sink.on_content) {
                sink.on_content(index, choice.content);
            }
        }
        kept->first_token = std::chrono::milliseconds{0};
        kept->duration = std::chrono::milliseconds{0};
        return std::move(*kept);
    }
    RR_TRY_ASSIGN(ChatResponse response, inner_->chat(request, sink, stop));
    ++made_;
    // An answer already paid for is returned even if it cannot be kept: the
    // cost of the failure is one repeated call after a resume.
    if (auto stored = store_->keep(key, response); !stored.has_value()) {
        log::warn("an answer from '{}' was not kept in the job's memo, so a resume would ask again: {}",
                  request.model, stored.error().describe());
    }
    return response;
}

}  // namespace rr::llm
