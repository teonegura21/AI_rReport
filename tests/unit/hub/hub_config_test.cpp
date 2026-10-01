#include <rr/hub/hub_config.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::hub {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using testkit::unwrap;

const HubFacts kHome{.home = "/home/ada"};

Result<HubSettings> settings(std::vector<std::string> overrides = {}, const HubFacts& facts = kHome) {
    RR_TRY_ASSIGN(const config::Config config,
                  config::Config::load({.defaults_toml = hub_defaults(),
                                        .env_prefix = std::string{kHubEnvPrefix},
                                        .environment = std::vector<std::pair<std::string, std::string>>{},
                                        .overrides = std::move(overrides)}));
    return hub_settings_from_config(config, facts);
}

TEST(HubConfig, TheBuiltInDefaultsAreAWorkingConfiguration) {
    const HubSettings loaded = unwrap(settings());
    EXPECT_EQ(loaded.data_dir, "/home/ada/.local/share/rreport");
    EXPECT_EQ(loaded.library, "default");
    EXPECT_EQ(loaded.library_database(), "/home/ada/.local/share/rreport/libraries/default/library.db");
    EXPECT_FALSE(loaded.runtime.has_value()) << "no pod configured: the hub still works, without models";
    EXPECT_FALSE(loaded.runpod.has_value());
    EXPECT_EQ(loaded.llm_mode, LlmMode::live);
    EXPECT_EQ(loaded.recordings, "/home/ada/.local/share/rreport/recordings");
    EXPECT_EQ(loaded.call_limits.idle_timeout, 60s);
    EXPECT_EQ(loaded.call_limits.call_timeout, 30min);
    EXPECT_EQ(loaded.call_attempts, 4);
    EXPECT_EQ(loaded.ready_timeout, 20min);
    EXPECT_EQ(loaded.ytdlp, "yt-dlp") << "found in PATH";
    EXPECT_EQ(loaded.ytdlp_timeout, 3min);
}

TEST(HubConfig, YtDlpIsTheUsers) {
    const HubSettings given = unwrap(settings({"media.ytdlp=/opt/tools/yt-dlp", "media.ytdlp_timeout=90s"}));
    EXPECT_EQ(given.ytdlp, "/opt/tools/yt-dlp");
    EXPECT_EQ(given.ytdlp_timeout, 90s);
    EXPECT_ERRC(settings({"media.ytdlp="}), Errc::invalid_argument);
    EXPECT_ERRC(settings({"media.ytdlp_timeout=0s"}), Errc::invalid_argument);
}

TEST(HubConfig, TheDataDirectoryFollowsXdg) {
    EXPECT_EQ(unwrap(settings({}, {.home = "/home/ada", .xdg_data_home = "/data"})).data_dir,
              "/data/rreport");
    EXPECT_EQ(unwrap(settings({}, {.home = "/home/ada", .xdg_data_home = "relative"})).data_dir,
              "/home/ada/.local/share/rreport")
        << "the XDG specification says to ignore relative paths";
    EXPECT_EQ(unwrap(settings({"hub.data_dir=/srv/rreport"}, {})).data_dir, "/srv/rreport");
    const auto nowhere = settings({}, {});
    ASSERT_ERRC(nowhere, Errc::invalid_argument);
    EXPECT_THAT(nowhere.error().message(), HasSubstr("set hub.data_dir"));
}

TEST(HubConfig, TheRuntimeFollowsThePod) {
    const HubSettings on_runpod =
        unwrap(settings({"runpod.pod_id=abc123", "runpod.api_key=k", "runtime.token=t"}));
    ASSERT_TRUE(on_runpod.runtime.has_value());
    EXPECT_EQ(on_runpod.runtime->url, "https://abc123-8080.proxy.runpod.net");
    EXPECT_TRUE(on_runpod.runtime->token.matches("t"));
    ASSERT_TRUE(on_runpod.runpod.has_value());
    EXPECT_EQ(on_runpod.runpod->api_url, "https://api.runpod.io/v2");

    EXPECT_EQ(unwrap(settings({"runpod.pod_id=abc123", "runpod.port=9000"})).runtime->url,
              "https://abc123-9000.proxy.runpod.net");
    EXPECT_EQ(unwrap(settings({"runpod.pod_id=abc123", "runtime.url=http://127.0.0.1:8080"})).runtime->url,
              "http://127.0.0.1:8080")
        << "an explicit URL wins";

    EXPECT_ERRC(settings({"runtime.url=ftp://pod"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({"runpod.pod_id=abc/../x"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({"runpod.port=0"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({"hub.library=../other"}), Errc::invalid_argument);
}

TEST(HubConfig, ModelCallSettings) {
    EXPECT_EQ(unwrap(settings({"llm.mode=replay"})).llm_mode, LlmMode::replay);
    EXPECT_EQ(unwrap(settings({"llm.mode=record", "llm.recordings=/rec"})).recordings, "/rec");
    const auto unknown = settings({"llm.mode=sometimes"});
    ASSERT_ERRC(unknown, Errc::invalid_argument);
    EXPECT_THAT(unknown.error().message(), HasSubstr("live, record or replay"));
    EXPECT_FALSE(unwrap(settings({"llm.call_timeout=0s"})).call_limits.call_timeout.has_value())
        << "0s: no limit";
    EXPECT_ERRC(settings({"llm.attempts=0"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({"llm.attempts=21"}), Errc::invalid_argument);
}

TEST(HubConfig, SamplingPresetsComeFromTheModelCards) {
    const HubSettings loaded = unwrap(settings());
    ASSERT_EQ(loaded.sampling.size(), 4U);

    const llm::SamplingPreset& qwen = loaded.sampling.at("qwen_thinking");
    EXPECT_EQ(qwen.temperature, 1.0);
    EXPECT_EQ(qwen.top_p, 0.95);
    EXPECT_EQ(qwen.top_k, 20);
    EXPECT_TRUE(qwen.thinking);
    ASSERT_TRUE(qwen.thinking_budget.has_value());
    EXPECT_EQ(qwen.thinking_budget->tokens, 8192);
    EXPECT_THAT(qwen.thinking_budget->closing, HasSubstr("must answer now")) << "TOML's \\n escapes apply";
    EXPECT_EQ(qwen.reasoning_effort, llm::ReasoningEffort::medium) << "not the template's xhigh";

    const llm::SamplingPreset& drafting = loaded.sampling.at("qwen_direct");
    EXPECT_EQ(drafting.temperature, 0.7);
    EXPECT_EQ(drafting.top_p, 0.8);
    EXPECT_FALSE(drafting.thinking);
    EXPECT_FALSE(drafting.thinking_budget.has_value()) << "-1: no budget";
    EXPECT_FALSE(drafting.reasoning_effort.has_value()) << "\"\": none";
    EXPECT_EQ(drafting.presence_penalty, 0.0) << "not the card's 1.5: penalties paraphrase quotes";

    EXPECT_EQ(loaded.sampling.at("gemma_direct").top_k, 64);
    for (const auto& [name, preset] : loaded.sampling) {
        llm::ChatRequest probe;
        llm::apply(preset, probe);
        EXPECT_THAT(llm::penalty_problems(probe), ::testing::IsEmpty()) << name << " penalizes nothing";
    }
}

TEST(HubConfig, ABadPresetFailsAtStartup) {
    const auto hot = settings({"sampling.qwen_direct.top_p=1.5"});
    ASSERT_ERRC(hot, Errc::invalid_argument);
    EXPECT_THAT(hot.error().message(), HasSubstr("sampling.qwen_direct: top_p"));

    const auto budget_without_thinking = settings({"sampling.gemma_direct.thinking_budget=512"});
    ASSERT_ERRC(budget_without_thinking, Errc::invalid_argument);
    EXPECT_THAT(budget_without_thinking.error().message(), HasSubstr("budget needs thinking"));

    const auto effort = settings({"sampling.qwen_thinking.reasoning_effort=extreme"});
    ASSERT_ERRC(effort, Errc::invalid_argument);
    EXPECT_THAT(effort.error().message(), HasSubstr("low, medium, high, xhigh or empty"));

    EXPECT_ERRC(settings({"sampling.qwen_thinking.thinking_budget=-2"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({"sampling.qwen_thinking.top_k=99999999999"}), Errc::invalid_argument);
    EXPECT_ERRC(settings({"sampling.qwen_thinking.temperatur=1"}), Errc::invalid_argument)
        << "a misspelt key is refused by rr_config, as everywhere";
}

TEST(HubConfig, ConfigurationFilesAreFoundWhereTheyLive) {
    const testkit::TempDir home;
    const HubFacts facts{.home = home.path().string()};
    ConfigFiles files = config_files(std::nullopt, std::nullopt, facts);
    EXPECT_FALSE(files.config.has_value()) << "none written yet";
    EXPECT_FALSE(files.secrets.has_value());

    (void)home.write_file(".config/rreport/config.toml", "");
    (void)home.write_file(".config/rreport/secrets.toml", "");
    files = config_files(std::nullopt, std::nullopt, facts);
    EXPECT_EQ(files.config, home / ".config/rreport/config.toml");
    EXPECT_FALSE(files.config_required) << "a default location is optional";
    EXPECT_EQ(files.secrets, home / ".config/rreport/secrets.toml");

    files = config_files(std::nullopt, std::nullopt,
                         {.home = home.path().string(), .config_env = "/etc/rr.toml"});
    EXPECT_EQ(files.config, "/etc/rr.toml");
    EXPECT_TRUE(files.config_required) << "a file the user named must exist";
    files = config_files(std::filesystem::path{"/opt/flag.toml"}, std::filesystem::path{"/opt/s.toml"},
                         {.config_env = "/etc/rr.toml"});
    EXPECT_EQ(files.config, "/opt/flag.toml") << "the flag wins";
    EXPECT_TRUE(files.config_required) << "a file named on the command line must exist too";
    EXPECT_EQ(files.secrets, "/opt/s.toml");

    const testkit::TempDir xdg;
    (void)xdg.write_file("rreport/config.toml", "");
    EXPECT_EQ(config_files(std::nullopt, std::nullopt,
                           {.home = home.path().string(), .xdg_config_home = xdg.path().string()})
                  .config,
              xdg / "rreport/config.toml");
}

}  // namespace
}  // namespace rr::hub
