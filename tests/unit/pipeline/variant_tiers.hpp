// variant_tiers.hpp — tiers of a test's own, for the settings no shipped tier
// has (StageContext::tiers).
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <rr/pipeline/stage.hpp>
#include <rr/reason/tier.hpp>

namespace rr::pipeline::testing {

// Finds every tier as the shipped `base`, changed as `change` says, under the
// name the job gives.
inline FindTier variant_of(std::string base, std::function<void(reason::Tier&)> change) {
    return
        [base = std::move(base), change = std::move(change)](std::string_view name) -> Result<reason::Tier> {
            RR_TRY_ASSIGN(reason::Tier tier, reason::builtin_tier(base));
            tier.name = std::string{name};
            change(tier);
            return tier;
        };
}

}  // namespace rr::pipeline::testing
