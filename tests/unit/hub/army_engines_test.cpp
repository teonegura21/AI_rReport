#include <rr/hub/army_engines.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/agents/army.hpp>
#include <rr/config/config.hpp>
#include <rr/core/clock.hpp>
#include <rr/hub/hub_config.hpp>
#include <rr/testkit/result.hpp>

namespace rr::hub {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;
using Presets = std::map<std::string, llm::SamplingPreset, std::less<>>;

// The presets rReport ships: [sampling.*] in config/default.toml.
Presets shipped_presets() {
    const config::Config config =
        unwrap(config::Config::load({.defaults_toml = hub_defaults(),
                                     .env_prefix = std::string{kHubEnvPrefix},
                                     .environment = std::vector<std::pair<std::string, std::string>>{},
                                     .overrides = {}}));
    return unwrap(hub_settings_from_config(config, HubFacts{.home = "/home/ada"})).sampling;
}

runtime_api::EngineInfo engine(std::string model, std::string role, int slots, int context, int ctx_per_slot,
                               bool kv_unified = false) {
    return runtime_api::EngineInfo{.model = std::move(model),
                                   .role = std::move(role),
                                   .state = runtime_api::EngineState::ready,
                                   .slots = slots,
                                   .context = context,
                                   .ctx_per_slot = ctx_per_slot,
                                   .kv_unified = kv_unified};
}

// The 48 GB profile, as its pod reports it.
runtime_api::HealthReport pod_48gb() {
    return runtime_api::HealthReport{.profile = "48gb",
                                     .engines = {engine("qwen3.8-27b-q4", "commander", 4, 32768, 32768, true),
                                                 engine("gemma-4-26b-a4b", "infantry", 16, 65536, 4096),
                                                 engine("qwen3-embedding-0.6b", "embedder", 4, 8192, 2048)}};
}

// A provider the army is built over, and never calls here.
class Unused final : public llm::LlmProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "unused"; }
    [[nodiscard]] llm::ProviderCaps caps() const noexcept override {
        return {.json_schema = true, .thinking = true};
    }
    [[nodiscard]] Result<llm::ChatResponse> chat(const llm::ChatRequest& /*request*/,
                                                 const llm::StreamSink& /*sink*/,
                                                 const std::stop_token& /*stop*/) const override {
        return fail(Errc::internal, "not called");
    }
};

TEST(ArmyEngines, TheCommanderAndTheInfantryWithTheirFamiliesPresets) {
    const Presets presets = shipped_presets();
    const std::vector<agents::Engine> engines = unwrap(army_engines(pod_48gb(), presets));
    ASSERT_EQ(engines.size(), 2U) << "the embedder takes no missions";

    const agents::Engine& commander = engines[0];
    EXPECT_EQ(commander.model, "qwen3.8-27b-q4");
    EXPECT_EQ(commander.roles, std::vector<std::string>{"commander"});
    EXPECT_EQ(commander.family, "qwen");
    EXPECT_EQ(commander.slots, 4);
    EXPECT_EQ(commander.context, 32768);
    EXPECT_EQ(commander.ctx_per_slot, 32768);
    EXPECT_TRUE(commander.kv_unified);
    EXPECT_EQ(commander.direct, presets.at("qwen_direct"));
    EXPECT_EQ(commander.thinking, presets.at("qwen_thinking"));

    const agents::Engine& infantry = engines[1];
    EXPECT_EQ(infantry.family, "gemma");
    EXPECT_EQ(infantry.ctx_per_slot, 4096);
    EXPECT_FALSE(infantry.kv_unified);
    EXPECT_EQ(infantry.thinking, presets.at("gemma_thinking"));

    // The shipped presets pass the army's own checks: an army can be raised.
    const Unused provider;
    const FakeClock clock;
    EXPECT_OK(agents::Army::create(agents::Army::Options{.engines = engines}, provider, clock));
}

TEST(ArmyEngines, APodWithOneOfTheRolesServesBothFromIt) {
    const runtime_api::HealthReport commander_only{
        .profile = "24gb", .engines = {engine("qwen3.8-27b-q4", "commander", 1, 16384, 16384)}};
    const std::vector<agents::Engine> alone = unwrap(army_engines(commander_only, shipped_presets()));
    ASSERT_EQ(alone.size(), 1U);
    EXPECT_EQ(alone[0].roles, (std::vector<std::string>{"commander", "infantry"}));

    const runtime_api::HealthReport light{
        .profile = "16gb",
        .engines = {engine("gemma-4-e4b", "infantry", 8, 32768, 4096),
                    engine("qwen3-embedding-0.6b", "embedder", 4, 8192, 2048)}};
    EXPECT_EQ(unwrap(army_engines(light, shipped_presets()))[0].roles,
              (std::vector<std::string>{"infantry", "commander"}));
}

TEST(ArmyEngines, SaysWhyThereIsNoArmy) {
    runtime_api::HealthReport starting = pod_48gb();
    starting.engines[0].state = runtime_api::EngineState::starting;
    const Result<std::vector<agents::Engine>> waiting = army_engines(starting, shipped_presets());
    EXPECT_ERRC(waiting, Errc::unavailable);
    EXPECT_THAT(waiting.error().message(), HasSubstr("'qwen3.8-27b-q4' is starting"));

    const runtime_api::HealthReport embedder_only{
        .profile = "tiny", .engines = {engine("qwen3-embedding-0.6b", "embedder", 4, 8192, 2048)}};
    const Result<std::vector<agents::Engine>> none = army_engines(embedder_only, shipped_presets());
    EXPECT_ERRC(none, Errc::unsupported);
    EXPECT_THAT(none.error().message(), HasSubstr("'tiny'"));

    runtime_api::HealthReport twice = pod_48gb();
    twice.engines.push_back(engine("gemma-4-12b", "infantry", 6, 24576, 4096));
    EXPECT_ERRC(army_engines(twice, shipped_presets()), Errc::invalid_argument);

    const runtime_api::HealthReport stranger{.profile = "custom",
                                             .engines = {engine("llama-3.1-8b", "infantry", 4, 16384, 4096)}};
    const Result<std::vector<agents::Engine>> unconfigured = army_engines(stranger, shipped_presets());
    EXPECT_ERRC(unconfigured, Errc::invalid_argument);
    EXPECT_THAT(unconfigured.error().message(), HasSubstr("[sampling.llama_direct]"));
}

}  // namespace
}  // namespace rr::hub
