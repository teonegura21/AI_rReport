#include <rr/agents/ladder.hpp>

#include <algorithm>

#include <rr/core/assert.hpp>

namespace rr::agents {

Ladder::Ladder(std::span<const Engine> engines, std::size_t own, std::optional<std::size_t> commander,
               bool asks_thinking, bool can_think, Escalation escalation)
    : engines_(engines) {
    RR_ASSERT(own < engines.size(), "the mission's engine is one of the army's");
    RR_ASSERT(commander.value_or(own) < engines.size(), "the commander is one of the army's");
    candidates_.push_back(
        Rung{.step = Step::as_ordered, .engine = own, .thinking = asks_thinking && can_think});
    if (escalation.thinking && can_think) {
        candidates_.push_back(Rung{.step = Step::thinking, .engine = own, .thinking = true});
    }
    if (escalation.commander && commander.has_value() && *commander != own) {
        candidates_.push_back(Rung{.step = Step::commander, .engine = *commander, .thinking = can_think});
    }
    if (escalation.other_family) {
        if (commander.has_value()) {
            others_.push_back(Rung{.step = Step::other_family, .engine = *commander, .thinking = can_think});
        }
        for (std::size_t engine = 0; engine < engines.size(); ++engine) {
            if (engine != commander) {
                others_.push_back(Rung{.step = Step::other_family, .engine = engine, .thinking = can_think});
            }
        }
    }
    climbed_.push_back(candidates_.front());
}

std::optional<Rung> Ladder::next(bool refused, const Holds& holds) {
    if (refused) {
        refused_.insert(engines_[climbed_.back().engine].family);
    }
    // Rung 4 exists only once a family has refused.
    const std::size_t total = candidates_.size() + (refused_.empty() ? 0 : others_.size());
    for (std::size_t at = position_ + 1; at < total; ++at) {
        const Rung& rung = at < candidates_.size() ? candidates_[at] : others_[at - candidates_.size()];
        if (climbed(rung) || refused_.contains(engines_[rung.engine].family) || !holds(rung)) {
            continue;
        }
        position_ = at;
        climbed_.push_back(rung);
        return rung;
    }
    return std::nullopt;
}

bool Ladder::climbed(const Rung& rung) const {
    return std::ranges::any_of(climbed_, [&](const Rung& done) {
        return done.engine == rung.engine && done.thinking == rung.thinking;
    });
}

}  // namespace rr::agents
