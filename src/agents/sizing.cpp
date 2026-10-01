#include <rr/agents/sizing.hpp>

#include <algorithm>
#include <format>

namespace rr::agents {

Result<std::int64_t> prompt_tokens(const llm::ChatRequest& request, const prompt::TokenEstimator& estimator) {
    std::int64_t tokens = 0;
    for (const llm::Message& message : request.messages) {
        tokens += prompt::TokenEstimator::kTokensPerMessage;
        for (const llm::ContentPart& part : message.content) {
            if (part.kind != llm::ContentPart::Kind::text) {
                return fail(Errc::unsupported,
                            std::format("a {} message holds an image, whose tokens cannot be estimated",
                                        llm::to_string(message.role)));
            }
            tokens += estimator.estimate(part.value);
        }
    }
    return tokens;
}

Result<Footprint> footprint_of(const llm::ChatRequest& request, const prompt::TokenEstimator& estimator) {
    if (!request.max_tokens.has_value()) {
        return fail(Errc::invalid_argument, "a request without max_tokens cannot be sized");
    }
    RR_TRY_ASSIGN(const std::int64_t prompt, prompt_tokens(request, estimator));
    return Footprint{.prompt = prompt, .answer = *request.max_tokens};
}

int choices_that_fit(const Engine& engine, const Footprint& footprint) noexcept {
    if (footprint.answer < 1 || footprint.prompt + footprint.answer > engine.ctx_per_slot) {
        return 0;
    }
    if (!engine.kv_unified) {
        return engine.slots;  // each answer's sequence has a slot's own share
    }
    // One prompt, kept once, and each answer's room.
    const std::int64_t answers = (engine.context - footprint.prompt) / footprint.answer;
    return static_cast<int>(std::clamp<std::int64_t>(answers, 0, engine.slots));
}

std::int64_t reservation(const Engine& engine, const Footprint& footprint, int choices) noexcept {
    return engine.kv_unified ? footprint.prompt + (choices * footprint.answer) : 0;
}

}  // namespace rr::agents
