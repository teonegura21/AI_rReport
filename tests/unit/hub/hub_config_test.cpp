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
