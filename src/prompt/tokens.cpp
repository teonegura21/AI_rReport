#include <rr/prompt/tokens.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

#include <rr/core/assert.hpp>

namespace rr::prompt {

TokenEstimator::TokenEstimator(double tokens_per_character) : tokens_per_character_(tokens_per_character) {
    RR_ASSERT(tokens_per_character > 0.0, "a text of characters has tokens");
}

std::int64_t TokenEstimator::estimate(std::string_view text) const noexcept {
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(text.size()) * tokens_per_character_));
}

std::int64_t TokenEstimator::estimate(const Rendered& prompt) const noexcept {
    return estimate(prompt.system) + estimate(prompt.user) + (2 * kTokensPerMessage);
}

bool TokenEstimator::fits(const Rendered& prompt, std::int64_t max_tokens,
                          std::int64_t context) const noexcept {
    return estimate(prompt) + max_tokens <= context;
}

Result<TokenEstimator> calibrate(std::span<const Observation> observations, double quantile) {
    if (std::isnan(quantile) || quantile < 0.5 || quantile > 1.0) {
        return fail(Errc::invalid_argument,
                    std::format("a calibration quantile is between 0.5 and 1, not {}", quantile));
    }
    std::vector<double> ratios;
    for (const Observation& observation : observations) {
        if (observation.characters > 0 && observation.tokens > 0) {
            ratios.push_back(static_cast<double>(observation.tokens) /
                             static_cast<double>(observation.characters));
        }
    }
    if (ratios.empty()) {
        return fail(Errc::invalid_argument, "no call with a prompt and a token count to calibrate from");
    }
    std::ranges::sort(ratios);
    // The smallest ratio at or above `quantile` of them (nearest rank).
    const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(ratios.size())));
    return TokenEstimator{ratios[std::max<std::size_t>(rank, 1) - 1]};
}

}  // namespace rr::prompt
