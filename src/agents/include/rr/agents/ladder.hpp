// rr/agents/ladder.hpp — where a mission goes when its answers fail.
//
// Why this exists
//   A small model that fails a narrow task often succeeds once it thinks; a
//   larger model often succeeds where the small one did not; and a model
//   that refuses will refuse again, however long it thinks (ADR-0011,
//   ADR-0017, ADR-0018). So a mission climbs a ladder, one rung at a time,
//   and only as far as it must (rungs are rr::agents::Step):
//     1. as ordered: its role's engine, thinking or not as it asked;
//     2. thinking: the same engine, thinking;
//     3. commander: the commander's engine, thinking, when that is another
//        engine than the mission's own;
//     4. other family: only after a refusal, an engine of a family that has
//        not refused, thinking (the commander's first).
//   A rung is passed over when the mission does not allow it (Escalation),
//   when it would repeat a rung already climbed (the same engine, thinking
//   the same way), when its engine's family has refused, or when its
//   engine cannot hold the mission. The ladder only climbs: a rung passed
//   over is never come back to.
//
//   The ladder decides only *where*. Whether to climb at all (no answer was
//   accepted), and what each rung asks for (rungs past the first ask for one
//   answer), is the army's (rr/agents/army.hpp).
//
//   Without a provider that can switch thinking on (llm::ProviderCaps), no
//   rung thinks, and the thinking rung, which would repeat the first, is
//   never climbed.
//
// Thread-safety: a plain value; one mission's ladder is used by one thread at a time.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <rr/agents/mission.hpp>

namespace rr::agents {

struct Rung {
    Step step = Step::as_ordered;
    std::size_t engine = 0;  // an index into the army's engines
    bool thinking = false;

    friend bool operator==(const Rung&, const Rung&) = default;
};

class Ladder {
public:
    // Whether a rung's engine can hold the mission, as that rung would ask it.
    using Holds = std::function<bool(const Rung&)>;

    // The ladder of a mission served by engines[own]. `commander` is the
    // engine serving the commander role, if there is one; `asks_thinking`,
    // whether the mission's own request thinks; `can_think`, whether the
    // provider can switch thinking on.
    Ladder(std::span<const Engine> engines, std::size_t own, std::optional<std::size_t> commander,
           bool asks_thinking, bool can_think, Escalation escalation);

    // The first rung: as ordered. Climbing starts there.
    [[nodiscard]] Rung first() const noexcept { return candidates_.front(); }

    // The rung after the last one climbed, which `refused` says its models
    // refused; none at the top. The rung returned counts as climbed.
    [[nodiscard]] std::optional<Rung> next(bool refused, const Holds& holds);

    // The families that have refused so far.
    [[nodiscard]] const std::set<std::string>& refused() const noexcept { return refused_; }

private:
    [[nodiscard]] bool climbed(const Rung& rung) const;

    std::span<const Engine> engines_;
    std::vector<Rung> candidates_;  // rungs 1–3 that the mission allows, in order
    std::vector<Rung> others_;      // rung 4's engines, the commander's first
    std::size_t position_ = 0;      // the last rung climbed, in candidates_ then others_
    std::vector<Rung> climbed_;
    std::set<std::string> refused_;
};

}  // namespace rr::agents
