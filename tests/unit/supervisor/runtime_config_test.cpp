#include <rr/supervisor/runtime_config.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::supervisor {
namespace {

using namespace std::chrono_literals;
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
    EXPECT_TRUE(loaded.stop_command.empty()) << "off RunPod, idleness is only logged";
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

TEST(RuntimeConfig, OnRunpodIdleStopStopsThePod) {
    const SupervisorSettings on_runpod =
        unwrap(settings({{"RR_RUNTIME_TOKEN", "t"}}, {}, {.gpus = {gpu(24'576)}, .runpod_pod_id = "pod123"}));
    EXPECT_EQ(on_runpod.stop_command, (std::vector<std::string>{"runpodctl", "stop", "pod", "pod123"}));

    const SupervisorSettings configured =
        unwrap(settings({{"RR_RUNTIME_TOKEN", "t"}, {"RR_RUNTIME_IDLE__STOP_COMMAND", "systemctl,poweroff"}},
                        {}, {.gpus = {gpu(24'576)}, .runpod_pod_id = "pod123"}));
    EXPECT_EQ(configured.stop_command, (std::vector<std::string>{"systemctl", "poweroff"}))
        << "a configured command wins";
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
