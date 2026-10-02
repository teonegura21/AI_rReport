// idle_army.hpp — an army whose model is never asked: for the parts of a
// stage that only size missions, or explain how one ended.
#pragma once

#include <optional>
#include <utility>

#include <rr/agents/army.hpp>
#include <rr/core/clock.hpp>
#include <rr/llm/chat.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/testkit/result.hpp>

namespace rr::pipeline::testing {

// One engine, the commander, holding `ctx_per_slot` tokens in a sequence.
class IdleArmy {
public:
    explicit IdleArmy(int ctx_per_slot = 8'192) {
        army_.emplace(testkit::unwrap(agents::Army::create(
            agents::Army::Options{.engines = {agents::Engine{.model = "qwen-commander",
                                                             .checksum = "sha-q",
                                                             .roles = {"commander"},
                                                             .family = "qwen",
                                                             .slots = 1,
                                                             .context = ctx_per_slot,
                                                             .ctx_per_slot = ctx_per_slot,
                                                             .thinking = thinking()}},
                                  .job_id = "J1"},
            oracle_, clock_)));
    }

    [[nodiscard]] const agents::Army& army() const { return *army_; }

private:
    // An army's engines think with a budget, so that every escalation ends.
    static llm::SamplingPreset thinking() {
        return llm::SamplingPreset{
            .thinking = true,
            .thinking_budget = llm::ThinkingBudget{.tokens = 500, .closing = "Answer now."}};
    }

    oracle::Oracle oracle_;
    FakeClock clock_;
    std::optional<agents::Army> army_;
};

}  // namespace rr::pipeline::testing
