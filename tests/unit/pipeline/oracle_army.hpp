// oracle_army.hpp — an army whose models are the oracle (rr/oracle/oracle.hpp),
// making the faults a test injects where it says: what the tests of the
// stages that ask models run on.
#pragma once

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/core/clock.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/sampling.hpp>
#include <rr/oracle/answers.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/stage.hpp>
#include <rr/testkit/result.hpp>

namespace rr::pipeline::testing {

// What each role's engine holds in a sequence, how many sequences it runs at
// once, and how long both think at most.
struct Sizes {
    int commander_holds = 32'768;
    int commander_slots = 4;
    int infantry_holds = 32'768;
    int infantry_slots = 4;
    int thinking_budget = 500;
};

// A Qwen commander and a Gemma infantry, sampling as their model cards say.
inline std::vector<agents::Engine> engines(const Sizes& sizes = {}) {
    const llm::SamplingPreset direct{.temperature = 1.0, .top_p = 0.95, .top_k = 64};
    const llm::SamplingPreset thinking{
        .temperature = 1.0,
        .top_p = 0.95,
        .top_k = 64,
        .thinking = true,
        .thinking_budget = llm::ThinkingBudget{.tokens = sizes.thinking_budget, .closing = "Answer now."}};
    return {agents::Engine{.model = "qwen-commander",
                           .checksum = "sha-q",
                           .roles = {"commander"},
                           .family = "qwen",
                           .slots = sizes.commander_slots,
                           .context = sizes.commander_holds * sizes.commander_slots,
                           .ctx_per_slot = sizes.commander_holds,
                           .direct = direct,
                           .thinking = thinking},
            agents::Engine{.model = "gemma-infantry",
                           .checksum = "sha-g",
                           .roles = {"infantry"},
                           .family = "gemma",
                           .slots = sizes.infantry_slots,
                           .context = sizes.infantry_holds * sizes.infantry_slots,
                           .ctx_per_slot = sizes.infantry_holds,
                           .direct = direct,
                           .thinking = thinking}};
}

// The army, its models the oracle (taught every stage's missions), with
// `faults` made where they say, and every request kept.
class OracleArmy {
public:
    explicit OracleArmy(std::vector<oracle::Fault> faults = {},
                        std::vector<agents::Engine> engines_used = engines(), agents::Ceilings ceilings = {})
        : faulty_(oracle_, std::move(faults))
        , recorder_(faulty_) {
        oracle::teach_stages(oracle_);
        army_.emplace(testkit::unwrap(agents::Army::create(
            agents::Army::Options{.engines = std::move(engines_used), .job_id = "J1", .ceilings = ceilings},
            recorder_, clock_)));
    }

    // The army refers to the models it was raised with.
    OracleArmy(const OracleArmy&) = delete;
    OracleArmy& operator=(const OracleArmy&) = delete;
    OracleArmy(OracleArmy&&) = delete;
    OracleArmy& operator=(OracleArmy&&) = delete;
    ~OracleArmy() = default;

    // What a stage is given to raise it.
    [[nodiscard]] RaiseArmy raise() {
        return [this]() -> Result<agents::Army*> { return &*army_; };
    }

    // The requests of the missions of `kind`, by mission (a squad's missions
    // run at once, in any order), each mission's in the order sent.
    [[nodiscard]] std::vector<llm::ChatRequest> requests(std::string_view kind) const {
        std::vector<llm::ChatRequest> found;
        for (const llm::ChatRequest& request : recorder_.requests()) {
            if (request.tag.purpose == kind) {
                found.push_back(request);
            }
        }
        std::ranges::stable_sort(found, {},
                                 [](const llm::ChatRequest& request) { return request.tag.mission_id; });
        return found;
    }

    // The faults made so far.
    [[nodiscard]] std::vector<oracle::Injection> injected() const { return faulty_.injected(); }

private:
    oracle::Oracle oracle_;
    oracle::FaultyProvider faulty_;
    oracle::RequestLog recorder_;
    FakeClock clock_;
    std::optional<agents::Army> army_;
};

}  // namespace rr::pipeline::testing
