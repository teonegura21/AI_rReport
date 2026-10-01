#include <rr/hub/doctor.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/fakes/fake_runpod.hpp>
#include <rr/json/json.hpp>
#include <rr/net/http_server.hpp>
#include <rr/runtime_api/protocol.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::hub {
namespace {

using fakes::FakeRunpod;
using ::testing::HasSubstr;
using testkit::unwrap;
using Response = net::HttpServer::Response;

constexpr std::string_view kHubVersion = "0.1.0 (test)";

// A runtime that says what the test wants: /ping, and /v1/health behind the token.
class FakeRuntime {
public:
    FakeRuntime() {
        server_.set_guard([](const net::HttpServer::Request& request) -> std::optional<Response> {
            if (request.path != "/ping" && request.headers.get("Authorization") != "Bearer good") {
                return Response::error(Error{Errc::permission_denied, "a valid bearer token is required"});
            }
            return std::nullopt;
        });
        server_.route(net::Method::get, "/ping", [](const auto&) { return Response::text(200, "ok"); });
        server_.route(net::Method::get, "/v1/health", [this](const auto&) {
            const std::lock_guard lock(mutex_);
            return Response::json(200, json::dump(runtime_api::to_json(report_)));
        });
        unwrap(server_.start());
        set(runtime_api::EngineState::ready, std::string{kHubVersion});
    }

    void set(runtime_api::EngineState state, std::string version) {
        const std::lock_guard lock(mutex_);
        report_ = runtime_api::HealthReport{
            .version = std::move(version),
            .profile = "24gb",
            .gpus = {{.index = 0, .name = "RTX 4090", .memory_total_mib = 24564, .memory_used_mib = 20000}},
            .engines = {{.model = "commander", .role = "commander", .state = state, .slots = 2}}};
    }

    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}", server_.port()); }

private:
    std::mutex mutex_;
    runtime_api::HealthReport report_;
    net::HttpServer server_{net::HttpServer::Options{.host = "127.0.0.1", .port = 0}};
};

class DoctorTest : public ::testing::Test {
protected:
    [[nodiscard]] HubSettings& settings() { return settings_; }
    [[nodiscard]] const testkit::TempDir& dir() const { return dir_; }
    [[nodiscard]] FakeRunpod& runpod() { return runpod_; }
    [[nodiscard]] FakeRuntime& runtime() { return runtime_; }

    [[nodiscard]] std::vector<Finding> run() const {
        return run_doctor(DoctorInputs{.settings = &settings_,
                                       .http = &http_,
                                       .hub_version = std::string{kHubVersion},
                                       .find_program = find_program_,
                                       .free_space = free_space_});
    }

    void use_runpod(std::string_view key, std::string pod_id = "pod1") {
        settings_.runpod = RunpodSettings{
            .api_key = Secret{std::string{key}}, .pod_id = std::move(pod_id), .api_url = runpod_.api_url()};
    }
    void use_runtime(std::string_view token) {
        settings_.runtime =
            runtime_client::Endpoint{.url = runtime_.url(), .token = Secret{std::string{token}}};
    }

    using ProgramFinder = std::function<std::optional<std::filesystem::path>(std::string_view)>;
    using SpaceProbe = std::function<std::optional<std::uintmax_t>(const std::filesystem::path&)>;
    void set_find_program(ProgramFinder finder) { find_program_ = std::move(finder); }
    void set_free_space(SpaceProbe probe) { free_space_ = std::move(probe); }

private:
    testkit::TempDir dir_;
    HubSettings settings_{.data_dir = dir_ / "data"};
    FakeRunpod runpod_;
    FakeRuntime runtime_;
    net::HttpClient http_;
    ProgramFinder find_program_ = [](std::string_view program) {
        return std::optional{std::filesystem::path{"/usr/bin"} / program};
    };
    SpaceProbe free_space_ = [](const std::filesystem::path&) {
        return std::optional<std::uintmax_t>{std::uintmax_t{100} << 30U};
    };
};

const Finding& about(const std::vector<Finding>& findings, std::string_view check) {
    const auto found = std::ranges::find(findings, check, &Finding::check);
    if (found == findings.end()) {
        throw std::runtime_error(std::format("no finding about {}", check));
    }
    return *found;
}

TEST_F(DoctorTest, AHubWithoutAPodIsHealthyAndSaysWhatIsMissing) {
    const std::vector<Finding> findings = run();
    EXPECT_TRUE(healthy(findings));
    EXPECT_EQ(about(findings, "data directory").severity, Severity::ok);
    EXPECT_THAT(about(findings, "data directory").detail, HasSubstr("100.0 GiB free"));
    EXPECT_EQ(about(findings, "library").detail, "'default' (schema version 5)") << "created on the way";
    EXPECT_EQ(about(findings, "yt-dlp").severity, Severity::ok);
    EXPECT_EQ(about(findings, "RunPod").severity, Severity::warning);
    EXPECT_EQ(about(findings, "runtime").severity, Severity::warning);
    EXPECT_THAT(about(findings, "runtime").advice, HasSubstr("runpod.pod_id"));
    EXPECT_EQ(about(findings, "models").severity, Severity::warning);
    EXPECT_THAT(about(findings, "models").detail, HasSubstr("no runtime is configured"));
    // In order: the data directory first, the runtime last.
    EXPECT_EQ(findings.front().check, "data directory");
    EXPECT_EQ(findings.back().check, "runtime");
}

TEST_F(DoctorTest, MissingToolsAndLittleRoomAreWarnings) {
    set_find_program([](std::string_view program) -> std::optional<std::filesystem::path> {
        return program == "deno" ? std::nullopt : std::optional{std::filesystem::path{"/usr/bin"} / program};
    });
    set_free_space(
        [](const std::filesystem::path&) { return std::optional<std::uintmax_t>{std::uintmax_t{5} << 30U}; });
    const std::vector<Finding> findings = run();
    EXPECT_TRUE(healthy(findings)) << "warnings only";
    EXPECT_EQ(about(findings, "deno").severity, Severity::warning);
    EXPECT_THAT(about(findings, "deno").advice, HasSubstr("deno.com"));
    EXPECT_EQ(about(findings, "data directory").severity, Severity::warning);
    EXPECT_THAT(about(findings, "data directory").advice, HasSubstr("at least 20.0 GiB"));
}

TEST_F(DoctorTest, AnUnusableDataDirectoryIsAFailure) {
    (void)dir().write_file("occupied", "a file where the directory should be");
    settings().data_dir = dir() / "occupied";
    const std::vector<Finding> findings = run();
    EXPECT_FALSE(healthy(findings));
    EXPECT_EQ(about(findings, "data directory").severity, Severity::failure);
    EXPECT_THAT(about(findings, "data directory").advice, HasSubstr("hub.data_dir"));
}

TEST_F(DoctorTest, ChecksThePodAtRunpod) {
    runpod().add_pod("pod1", "RUNNING");
    use_runpod(FakeRunpod::kAccountKey);
    EXPECT_THAT(about(run(), "RunPod").detail,
                HasSubstr("pod pod1 is RUNNING: 1 × NVIDIA GeForce RTX 4090, $0.44/h"));

    use_runpod("");
    EXPECT_EQ(about(run(), "RunPod").severity, Severity::failure);
    use_runpod("wrong-key");
    EXPECT_THAT(about(run(), "RunPod").advice, HasSubstr("refused the API key"));
    use_runpod(FakeRunpod::kAccountKey, "nosuchpod");
    EXPECT_THAT(about(run(), "RunPod").advice, HasSubstr("has no pod nosuchpod"));
}

TEST_F(DoctorTest, ChecksTheRuntime) {
    use_runtime("good");
    const std::vector<Finding> ready = run();
    EXPECT_EQ(about(ready, "runtime").severity, Severity::ok);
    EXPECT_THAT(about(ready, "runtime").detail, HasSubstr("profile 24gb, RTX 4090 (24564 MiB, 20000 used)"));
    EXPECT_THAT(about(ready, "runtime").detail, HasSubstr("commander (commander) ready"));
    EXPECT_EQ(about(ready, "models").severity, Severity::ok);
    EXPECT_THAT(about(ready, "models").detail,
                HasSubstr("local only: every call goes to the runtime at http"))
        << "ADR-0017: one model endpoint";

    runtime().set(runtime_api::EngineState::starting, std::string{kHubVersion});
    EXPECT_EQ(about(run(), "runtime").severity, Severity::warning) << "still loading";

    runtime().set(runtime_api::EngineState::ready, "0.0.9 (older)");
    EXPECT_THAT(about(run(), "versions").detail, HasSubstr("the runtime 0.0.9 (older)"));

    use_runtime("bad");
    const std::vector<Finding> refused = run();
    EXPECT_FALSE(healthy(refused));
    EXPECT_THAT(about(refused, "runtime").advice, HasSubstr("must equal the pod's RR_RUNTIME_TOKEN"));

    use_runtime("");
    EXPECT_THAT(about(run(), "runtime").detail, HasSubstr("runtime.token is not set"));
}

TEST_F(DoctorTest, AReplayCallsNoModel) {
    settings().llm_mode = LlmMode::replay;
    settings().recordings = "/tmp/recorded";
    const Finding models = about(run(), "models");
    EXPECT_EQ(models.severity, Severity::ok);
    EXPECT_EQ(models.detail, "replayed from /tmp/recorded: no model is called");
}

TEST_F(DoctorTest, AStoppedPodsRuntimeIsNotAskedFor) {
    runpod().add_pod("pod1", "EXITED");
    use_runpod(FakeRunpod::kAccountKey);
    use_runtime("good");
    const std::vector<Finding> findings = run();
    const Finding& runtime_finding = about(findings, "runtime");
    EXPECT_EQ(runtime_finding.severity, Severity::warning);
    EXPECT_THAT(runtime_finding.detail, HasSubstr("the pod is EXITED"));
    EXPECT_THAT(runtime_finding.advice, HasSubstr("rreport pod start"));
}

TEST(FindOnPath, FindsExecutablesOnly) {
    const testkit::TempDir first;
    const testkit::TempDir second;
    (void)first.write_file("tool", "not executable");
    const auto executable = second.write_file("tool", "#!/bin/sh\n");
    ASSERT_EQ(::chmod(executable.c_str(), 0755), 0);
    EXPECT_EQ(find_on_path("tool", first.path().string() + ":" + second.path().string()), executable);
    EXPECT_FALSE(find_on_path("tool", first.path().string()).has_value());
    EXPECT_FALSE(find_on_path("absent", second.path().string()).has_value());
    EXPECT_EQ(find_on_path(executable.string(), ""), executable) << "a path is taken as it is";
    EXPECT_FALSE(find_on_path((first / "tool").string(), second.path().string()).has_value())
        << "and not looked up: the file it names is not executable";
    const auto nested = second.write_file("sub/tool", "#!/bin/sh\n");
    ASSERT_EQ(::chmod(nested.c_str(), 0755), 0);
    EXPECT_FALSE(find_on_path("sub/tool", second.path().string()).has_value())
        << "a relative path is the current directory's, as a shell takes it, never PATH's";
}

TEST_F(DoctorTest, ChecksTheYtDlpTheConfigurationNames) {
    settings().ytdlp = "/opt/tools/yt-dlp";
    std::vector<std::string> looked_for;
    set_find_program([&](std::string_view program) -> std::optional<std::filesystem::path> {
        looked_for.emplace_back(program);
        return std::nullopt;
    });
    const std::vector<Finding> findings = run();
    EXPECT_EQ(about(findings, "yt-dlp").severity, Severity::warning);
    EXPECT_EQ(looked_for.front(), "/opt/tools/yt-dlp");
}

}  // namespace
}  // namespace rr::hub
