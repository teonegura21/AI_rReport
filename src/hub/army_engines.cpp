#include <rr/hub/army_engines.hpp>

#include <algorithm>
#include <format>
#include <string_view>

namespace rr::hub {

namespace {

constexpr std::string_view kCommander = "commander";
constexpr std::string_view kInfantry = "infantry";

Result<llm::SamplingPreset> preset_for(const std::map<std::string, llm::SamplingPreset, std::less<>>& presets,
                                       const agents::Engine& engine, std::string_view how) {
    const std::string name = std::format("{}_{}", engine.family, how);
    const auto found = presets.find(name);
    if (found == presets.end()) {
        return fail(Errc::invalid_argument,
                    std::format("the pod serves '{}', of the {} family, and the configuration has no "
                                "[sampling.{}] to say how it samples",
                                engine.model, engine.family, name));
    }
    return found->second;
}

}  // namespace

Result<std::vector<agents::Engine>> army_engines(
    const runtime_api::HealthReport& report,
    const std::map<std::string, llm::SamplingPreset, std::less<>>& presets) {
    std::vector<agents::Engine> engines;
    for (const runtime_api::EngineInfo& info : report.engines) {
        if (info.role != kCommander && info.role != kInfantry) {
            continue;  // the embedder and the reranker take no missions
        }
        if (info.state != runtime_api::EngineState::ready) {
            return fail(Errc::unavailable, std::format("the {} engine '{}' is {}, not ready", info.role,
                                                       info.model, runtime_api::to_string(info.state)));
        }
        const bool served = std::ranges::any_of(
            engines, [&](const agents::Engine& engine) { return engine.roles.front() == info.role; });
        if (served) {
            return fail(Errc::invalid_argument,
                        std::format("two engines serve the {} role; the army needs one", info.role));
        }
        agents::Engine engine{.model = info.model,
                              .roles = {info.role},
                              .family = agents::family_of(info.model),
                              .slots = info.slots,
                              .context = info.context,
                              .ctx_per_slot = info.ctx_per_slot,
                              .kv_unified = info.kv_unified};
        RR_TRY_ASSIGN(engine.direct, preset_for(presets, engine, "direct"));
        RR_TRY_ASSIGN(engine.thinking, preset_for(presets, engine, "thinking"));
        engines.push_back(std::move(engine));
    }
    if (engines.empty()) {
        return fail(Errc::unsupported,
                    std::format("the pod's profile '{}' serves neither the commander nor the infantry",
                                report.profile));
    }
    if (engines.size() == 1) {
        // One model for both roles.
        agents::Engine& only = engines.front();
        only.roles.emplace_back(only.roles.front() == kCommander ? kInfantry : kCommander);
    }
    return engines;
}

}  // namespace rr::hub
