#include <rr/hub/job_army.hpp>

#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/config/config.hpp>
#include <rr/core/file_io.hpp>
#include <rr/json/json.hpp>
#include <rr/library/library.hpp>
#include <rr/net/http_server.hpp>
#include <rr/runtime_api/protocol.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::hub {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;
using Response = net::HttpServer::Response;

// The 48 GB profile as its pod reports it, every engine ready.
runtime_api::HealthReport pod_48gb() {
    const auto engine = [](std::string model, std::string role, int slots, int ctx_per_slot) {
        std::string checksum = std::format("sha256 of {}", model);
        return runtime_api::EngineInfo{.model = std::move(model),
                                       .role = std::move(role),
                                       .state = runtime_api::EngineState::ready,
                                       .slots = slots,
                                       .context = slots * ctx_per_slot,
                                       .ctx_per_slot = ctx_per_slot,
                                       .model_sha256 = std::move(checksum)};
    };
    return runtime_api::HealthReport{.version = "0.1.0",
                                     .profile = "48gb",
                                     .engines = {engine("qwen3.8-27b-q4", "commander", 4, 32768),
                                                 engine("gemma-4-26b-a4b", "infantry", 16, 4096)}};
}

// A runtime whose engines are all ready, behind the token "good".
class FakeRuntime {
public:
    FakeRuntime() {
        server_.route(net::Method::get, "/v1/health", [](const net::HttpServer::Request& request) {
            if (request.headers.get("Authorization") != "Bearer good") {
                return Response::error(Error{Errc::permission_denied, "a valid bearer token is required"});
            }
            return Response::json(200, json::dump(runtime_api::to_json(pod_48gb())));
        });
        unwrap(server_.start());
    }

    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}", server_.port()); }

private:
    net::HttpServer server_{net::HttpServer::Options{.host = "127.0.0.1", .port = 0}};
};

class JobArmyTest : public ::testing::Test {
protected:
    JobArmyTest() {
        const config::Config config = unwrap(config::Config::load(
            {.defaults_toml = hub_defaults(),
             .env_prefix = std::string{kHubEnvPrefix},
             .environment = std::vector<std::pair<std::string, std::string>>{},
             .overrides = {std::format("hub.data_dir={}", (dir_ / "data").string()), "llm.mode=record",
                           std::format("runtime.url={}", runtime_.url()), "runtime.token=good",
                           "runtime.ready_timeout=10s"}}));
        settings_ = unwrap(hub_settings_from_config(config, HubFacts{.home = "/home/ada"}));
    }

    JobArmy::Inputs inputs(const HubSettings& given) {
        return JobArmy::Inputs{.settings = &given,
                               .http = &http_,
                               .database = &database_,
                               .clock = &clock_,
                               .job_id = "J1",
                               .progress = [this](std::string_view line) { progress_.emplace_back(line); }};
    }

    [[nodiscard]] HubSettings& settings() { return settings_; }
    [[nodiscard]] std::vector<std::string>& progress() { return progress_; }

private:
    testkit::TempDir dir_;
    FakeRuntime runtime_;
    HubSettings settings_;
    net::HttpClient http_;
    db::Database database_ = unwrap(library::open_database(dir_ / "library.db"));
    FakeClock clock_;
    std::vector<std::string> progress_;
};

TEST_F(JobArmyTest, RecordingRaisesTheArmyOnThePodsEnginesAndKeepsThemForAReplay) {
    JobArmy forces{inputs(settings())};
    agents::Army* army = unwrap(forces.raise());
    ASSERT_EQ(army->engines().size(), 2U);
    EXPECT_EQ(army->engines()[0].model, "qwen3.8-27b-q4");
    EXPECT_EQ(army->engines()[0].checksum, "sha256 of qwen3.8-27b-q4");
    EXPECT_EQ(army->engines()[1].slots, 16);
    EXPECT_EQ(unwrap(forces.raise()), army) << "raised once";
    EXPECT_THAT(progress().back(), HasSubstr("army: raised on qwen3.8-27b-q4 (commander, 4 slots)"));

    const json::Json kept = unwrap(json::parse(unwrap(read_file(recorded_engines(settings())))));
    EXPECT_EQ(unwrap(runtime_api::health_report_from_json(kept)), pod_48gb());

    HubSettings replay = settings();
    replay.llm_mode = LlmMode::replay;
    replay.runtime.reset();  // a replay calls no runtime
    JobArmy replayed{inputs(replay)};
    agents::Army* again = unwrap(replayed.raise());
    ASSERT_EQ(again->engines().size(), 2U);
    EXPECT_EQ(again->engines()[0], army->engines()[0]) << "the same army as the one recorded";
    EXPECT_EQ(again->engines()[1], army->engines()[1]);
}

TEST_F(JobArmyTest, AReplayWithoutRecordedEnginesNeedsTheUser) {
    HubSettings replay = settings();
    replay.llm_mode = LlmMode::replay;
    JobArmy forces{inputs(replay)};
    const Result<agents::Army*> army = forces.raise();
    EXPECT_ERRC(army, Errc::needs_user);
    EXPECT_THAT(army.error().message(), HasSubstr("record the job first"));
}

TEST_F(JobArmyTest, WithoutARuntimeAndItsTokenNoArmyCanBeRaised) {
    HubSettings live = settings();
    live.llm_mode = LlmMode::live;
    live.runtime->token = Secret{};
    JobArmy untold{inputs(live)};
    const Result<agents::Army*> army = untold.raise();
    EXPECT_ERRC(army, Errc::needs_user) << "the user sets it: not a pod's refusal";
    EXPECT_THAT(army.error().message(), HasSubstr("RR_RUNTIME_TOKEN"));
    live.runtime.reset();
    JobArmy podless{inputs(live)};
    EXPECT_ERRC(podless.raise(), Errc::needs_user);
}

TEST_F(JobArmyTest, AFailureToRaiseIsTriedAgainLater) {
    HubSettings live = settings();
    live.llm_mode = LlmMode::live;
    live.runtime->token = Secret{std::string{"bad"}};
    JobArmy forces{inputs(live)};
    EXPECT_FALSE(forces.raise().has_value());
    live.runtime->token = Secret{std::string{"good"}};
    EXPECT_TRUE(forces.raise().has_value()) << "a pod that did not answer may answer now";
}

}  // namespace
}  // namespace rr::hub
