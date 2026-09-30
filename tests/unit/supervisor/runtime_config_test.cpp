#include <rr/supervisor/runtime_config.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/fakes/fake_runpod.hpp>
#include <rr/testkit/result.hpp>

namespace rr::supervisor {
namespace {

using namespace std::chrono_literals;
using fakes::FakeRunpod;
using ::testing::HasSubstr;
using testkit::unwrap;

Catalog test_catalog() {
    const std::string sha(64, 'a');
    return unwrap(parse_catalog(std::format(R"toml(
[[model]]
id = "small"
role = "commander"
file = "small.gguf"
sha256 = "{0}"
[[model]]
id = "big"
role = "commander"
file = "big.gguf"
sha256 = "{0}"

[[profile]]
name = "24gb"
min_vram_mib = 20000
  [[profile.engine]]
  model = "small"
[[profile]]
name = "80gb"
min_vram_mib = 70000
  [[profile.engine]]
  model = "big"
)toml",
                                            sha)));
}

runtime_api::GpuInfo gpu(std::int64_t memory_mib) {
    return runtime_api::GpuInfo{.index = 0, .name = "GPU", .memory_total_mib = memory_mib};
}

// Loads the runtime's configuration as the executable does.
Result<config::Config> load(Environment environment, std::vector<std::string> overrides = {}) {
    RR_TRY_ASSIGN(Environment resolved, resolve_environment_aliases(std::move(environment)));
    return config::Config::load(config::LoadOptions{.defaults_toml = runtime_defaults(),
                                                    .env_prefix = std::string{kRuntimeEnvPrefix},
                                                    .environment = std::move(resolved),
                                                    .overrides = std::move(overrides)});
}

Result<SupervisorSettings> settings(Environment environment, std::vector<std::string> overrides = {},
                                    const RuntimeFacts& facts = {.gpus = {gpu(24'576)}}) {
    RR_TRY_ASSIGN(const config::Config config, load(std::move(environment), std::move(overrides)));
    return settings_from_config(config, test_catalog(), facts);
}

TEST(RuntimeConfig, TheBuiltInDefaultsAreAWorkingConfiguration) {
    const SupervisorSettings loaded = unwrap(settings({{"RR_RUNTIME_TOKEN", "hub-token"}}));
    EXPECT_TRUE(loaded.token.matches("hub-token"));
    EXPECT_EQ(loaded.host, "0.0.0.0");
    EXPECT_EQ(loaded.port, 8080);
    EXPECT_EQ(loaded.profile, "24gb");
    EXPECT_EQ(loaded.models_dir, "/workspace/models");
    EXPECT_EQ(loaded.first_engine_port, 18080);
    EXPECT_EQ(loaded.stream_keep_alive, 15s);
    EXPECT_EQ(loaded.stream_head_patience, 1s);
    EXPECT_EQ(loaded.idle_stop, 15min);
    EXPECT_TRUE(loaded.stop_command.empty() && !loaded.stop_pod) << "off RunPod, idleness is only logged";
    EXPECT_EQ(loaded.max_upload_bytes, std::uint64_t{8} << 30U);
}

TEST(RuntimeConfig, TheTokenIsRequired) {
    const auto missing = settings({});
    ASSERT_ERRC(missing, Errc::invalid_argument);
    EXPECT_THAT(missing.error().message(), HasSubstr("RR_RUNTIME_TOKEN"));
}

TEST(RuntimeConfig, AliasesAgreeWithTheirCanonicalNamesOrFail) {
    EXPECT_TRUE(unwrap(settings({{"RR_RUNTIME_SERVER__TOKEN", "t"}})).token.matches("t"));
    EXPECT_OK(settings({{"RR_RUNTIME_TOKEN", "t"}, {"RR_RUNTIME_SERVER__TOKEN", "t"}}));
    EXPECT_ERRC(settings({{"RR_RUNTIME_TOKEN", "one"}, {"RR_RUNTIME_SERVER__TOKEN", "other"}}),
                Errc::invalid_argument);

    const config::Config config = unwrap(load({{"HF_TOKEN", "hf_x"}}));
    EXPECT_EQ(unwrap(config.get_secret("models.hub_token")).expose(), "hf_x");
}

TEST(RuntimeConfig, AutoPicksWhatTheSmallestGpuHolds) {
    const Catalog catalog = test_catalog();
    EXPECT_EQ(unwrap(resolve_profile(catalog, "auto", {gpu(81'920)})), "80gb");
    EXPECT_EQ(unwrap(resolve_profile(catalog, "auto", {gpu(81'920), gpu(24'576)})), "24gb");
    EXPECT_ERRC(resolve_profile(catalog, "auto", {gpu(8'192)}), Errc::unsupported) << "nothing fits";
    EXPECT_ERRC(resolve_profile(catalog, "auto", {}), Errc::unavailable);
    EXPECT_EQ(unwrap(resolve_profile(catalog, "80gb", {})), "80gb") << "a named profile needs no GPU query";
    EXPECT_ERRC(resolve_profile(catalog, "12gb", {}), Errc::not_found);
}

// A pod on RunPod, as the runtime sees it: its id, and the key RunPod gives it.
RuntimeFacts on_runpod() {
    return {
        .gpus = {gpu(24'576)}, .runpod_pod_id = "pod123", .runpod_api_key = std::string{FakeRunpod::kPodKey}};
}

std::vector<std::string> aimed_at(const FakeRunpod& runpod) {
    return {std::format("idle.runpod_api_url={}", runpod.api_url()),
            std::format("idle.runpod_graphql_url={}", runpod.graphql_url())};
}

TEST(RuntimeConfig, OnRunpodAnIdlePodStopsItselfThroughTheApi) {
    FakeRunpod runpod;
    runpod.add_pod("pod123", "RUNNING");
    const SupervisorSettings loaded =
        unwrap(settings({{"RR_RUNTIME_TOKEN", "t"}}, aimed_at(runpod), on_runpod()));
    EXPECT_TRUE(loaded.stop_command.empty()) << "not runpodctl";
    ASSERT_TRUE(loaded.stop_pod);
    ASSERT_OK(loaded.stop_pod());
    EXPECT_EQ(runpod.status("pod123"), "EXITED");
    EXPECT_EQ(runpod.graphql_stops(), 1) << "REST refuses the pod's own key; GraphQL takes it";
}

TEST(RuntimeConfig, AKeyOrACommandOfOnesOwnWins) {
    FakeRunpod runpod;
    runpod.add_pod("pod123", "RUNNING");
    std::vector<std::string> overrides = aimed_at(runpod);
    overrides.push_back(std::format("idle.runpod_api_key={}", FakeRunpod::kAccountKey));
    const SupervisorSettings own_key = unwrap(settings({{"RR_RUNTIME_TOKEN", "t"}}, overrides, on_runpod()));
    ASSERT_OK(own_key.stop_pod());
    EXPECT_EQ(runpod.status("pod123"), "EXITED");
    EXPECT_EQ(runpod.graphql_stops(), 0) << "the owner's key needs no fallback";

    const SupervisorSettings configured =
        unwrap(settings({{"RR_RUNTIME_TOKEN", "t"}, {"RR_RUNTIME_IDLE__STOP_COMMAND", "systemctl,poweroff"}},
                        aimed_at(runpod), on_runpod()));
    EXPECT_EQ(configured.stop_command, (std::vector<std::string>{"systemctl", "poweroff"}));
    EXPECT_FALSE(configured.stop_pod) << "a configured command wins";

    RuntimeFacts keyless = on_runpod();
    keyless.runpod_api_key.reset();
    EXPECT_FALSE(unwrap(settings({{"RR_RUNTIME_TOKEN", "t"}}, {}, keyless)).stop_pod)
        << "without a key, idleness is only logged";
    RuntimeFacts bad_id = on_runpod();
    bad_id.runpod_pod_id = "pod/../123";
    EXPECT_ERRC(settings({{"RR_RUNTIME_TOKEN", "t"}}, {}, bad_id), Errc::invalid_argument);
}

TEST(RuntimeConfig, ZeroTurnsIdleStopAndKeepAlivesOff) {
    const SupervisorSettings loaded =
        unwrap(settings({{"RR_RUNTIME_TOKEN", "t"}}, {"idle.stop_after=0s", "engines.stream_keep_alive=0s"}));
    EXPECT_EQ(loaded.idle_stop, std::nullopt);
    EXPECT_EQ(loaded.stream_keep_alive, std::nullopt);
}

TEST(RuntimeConfig, MistakesAreCaughtAtStartup) {
    const auto typo = load({{"RR_RUNTIME_TOKEN", "t"}, {"RR_RUNTIME_SERVER__PROT", "9000"}});
    ASSERT_ERRC(typo, Errc::invalid_argument);
    EXPECT_THAT(typo.error().message(), HasSubstr("did you mean 'server.port'"));
    EXPECT_ERRC(settings({{"RR_RUNTIME_TOKEN", "t"}}, {"server.port=70000"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({{"RR_RUNTIME_TOKEN", "t"}}, {"server.worker_threads=0"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({{"RR_RUNTIME_TOKEN", "t"}}, {"idle.stop_after=soon"}), Errc::invalid_argument)
        << "durations are read when the settings are made, still before anything starts";
}

}  // namespace
}  // namespace rr::supervisor
