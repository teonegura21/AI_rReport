// The rreport executable, driven from the outside against a real
// rreport-runtime (with fake engines: a commander, and infantry with four
// slots) and a fake RunPod API: what an operator runs before the first video.
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/file_io.hpp>
#include <rr/core/hash.hpp>
#include <rr/fakes/fake_runpod.hpp>
#include <rr/json/json.hpp>
#include <rr/library/call_log.hpp>
#include <rr/library/library.hpp>
#include <rr/proc/process.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr {
namespace {

using namespace std::chrono_literals;
using fakes::FakeRunpod;
using ::testing::HasSubstr;
using ::testing::Not;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kToken = "hub-cli-token";
constexpr std::string_view kWeights = "pretend these are gigabytes of weights";

// rreport-runtime serving a commander ("chat") and infantry ("squad", four
// slots), both fake, on free ports.
class Runtime {
public:
    Runtime() {
        const std::string sha = sha256(kWeights).hex();
        (void)dir_.write_file("models/chat.gguf", kWeights);
        (void)dir_.write_file("models/squad.gguf", kWeights);
        (void)dir_.write_file("catalog.toml", std::format(R"toml(
[[model]]
id = "chat"
role = "commander"
file = "chat.gguf"
sha256 = "{0}"
[[model]]
id = "squad"
role = "infantry"
file = "squad.gguf"
sha256 = "{0}"

[[profile]]
name = "test"
  [[profile.engine]]
  model = "chat"
  extra_args = ["--fake-token-ms", "2"]
  [[profile.engine]]
  model = "squad"
  slots = 4
  extra_args = ["--fake-token-ms", "2"]
)toml",
                                                          sha));
        std::vector<std::string> argv{RR_RUNTIME_BINARY, "serve"};
        for (const std::string& setting :
             {std::string{"server.host=127.0.0.1"}, std::string{"server.port=0"},
              std::format("models.catalog={}", (dir_ / "catalog.toml").string()),
              std::format("models.dir={}", (dir_ / "models").string()), std::string{"models.profile=test"},
              std::format("blobs.dir={}", (dir_ / "blobs").string()),
              std::format("engines.llama_server={}", RR_FAKE_ENGINE),
              std::format("engines.first_port={}", testkit::free_port_range(2)),
              std::string{"idle.stop_after=0s"}, std::string{"log.level=warn"}}) {
            argv.emplace_back("--set");
            argv.push_back(setting);
        }
        process_.emplace(unwrap(proc::ChildProcess::start(
            proc::Command{.argv = std::move(argv),
                          .environment = {{"RR_RUNTIME_TOKEN", std::string{kToken}}},
                          .inherit_environment = false},
            proc::ChildProcess::Options{.on_output = [this](std::string_view line) {
                constexpr std::string_view kListening = "listening on 127.0.0.1:";
                if (line.starts_with(kListening)) {
                    const std::lock_guard lock(mutex_);
                    port_ = std::stoi(std::string{line.substr(kListening.size())});
                }
            }})));
        const auto deadline = Clock::now() + 20s;
        while (port() == 0 && Clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        if (port() == 0) {
            throw std::runtime_error("the runtime did not start");
        }
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    ~Runtime() {
        if (process_.has_value()) {
            (void)::kill(process_->pid(), SIGTERM);
            (void)process_->wait(15s);
        }
    }

    [[nodiscard]] int port() const {
        const std::lock_guard lock(mutex_);
        return port_;
    }
    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}", port()); }

private:
    testkit::TempDir dir_{"rr_hub_cli_runtime"};
    mutable std::mutex mutex_;
    int port_ = 0;
    std::optional<proc::ChildProcess> process_;
};

// The hub's side: a data directory, a pod at RunPod (faked), and the runtime.
class Hub : public ::testing::Test {
protected:
    Hub() { runpod_.add_pod("pod123", "RUNNING"); }

    [[nodiscard]] FakeRunpod& runpod() { return runpod_; }
    [[nodiscard]] const testkit::TempDir& home() const { return home_; }
    [[nodiscard]] std::filesystem::path data_dir() const { return home_ / "data"; }

    // Runs `rreport <command…>` with settings aimed at the fakes, then `extra`
    // settings (the last value of a setting wins).
    [[nodiscard]] proc::RunResult rreport(std::vector<std::string> command,
                                          const std::vector<std::string>& extra = {},
                                          std::vector<EnvironmentVariable> environment = {}) const {
        std::vector<std::string> argv{RR_HUB_BINARY};
        argv.insert(argv.end(), command.begin(), command.end());
        std::vector<std::string> settings{
            std::format("hub.data_dir={}", data_dir().string()),
            std::format("runtime.url={}", runtime_.url()), "runpod.pod_id=pod123",
            std::format("runpod.api_url={}", runpod_.api_url()), "log.level=warn"};
        settings.insert(settings.end(), extra.begin(), extra.end());
        for (const std::string& setting : settings) {
            argv.emplace_back("--set");
            argv.push_back(setting);
        }
        environment.emplace_back("HOME", home_.path().string());
        environment.emplace_back("RREPORT_RUNTIME__TOKEN", std::string{kToken});
        environment.emplace_back("RREPORT_RUNPOD__API_KEY", std::string{FakeRunpod::kAccountKey});
        environment.emplace_back("PATH", "/usr/bin:/bin");
        return unwrap(proc::run(
            proc::Command{
                .argv = std::move(argv), .environment = std::move(environment), .inherit_environment = false},
            proc::RunOptions{.timeout = 120s}));
    }

private:
    testkit::TempDir home_{"rr_hub_cli_home"};
    FakeRunpod runpod_;
    Runtime runtime_;
};

TEST_F(Hub, DoctorChecksEverythingAndSaysSo) {
    // The engines load after the runtime starts listening: wait, as an operator would.
    ASSERT_EQ(rreport({"pod", "start"}).status.code, 0);
    const proc::RunResult result = rreport({"doctor"});
    EXPECT_EQ(result.status.code, 0) << result.stdout_text << result.stderr_text;
    // ok, or a warning on a disk with little room (CI runners): either way, found and writable.
    EXPECT_THAT(result.stdout_text, HasSubstr(std::format("] data directory: '{}'", data_dir().string())));
    EXPECT_THAT(result.stdout_text, HasSubstr("[ok  ] library: 'default' (schema version 1)"));
    EXPECT_THAT(result.stdout_text, HasSubstr("[ok  ] RunPod: pod pod123 is RUNNING"));
    EXPECT_THAT(result.stdout_text, HasSubstr("[ok  ] runtime:"));
    EXPECT_THAT(result.stdout_text, HasSubstr("chat (commander) ready, squad (infantry) ready"));
    EXPECT_THAT(result.stdout_text, Not(HasSubstr("[warn] versions"))) << "one build, one version";
}

TEST_F(Hub, DoctorExplainsWhatIsWrong) {
    const proc::RunResult wrong_token = rreport({"doctor"}, {"runtime.token=not-the-token"});
    EXPECT_EQ(wrong_token.status.code, 1) << "a failure";
    EXPECT_THAT(wrong_token.stdout_text, HasSubstr("[FAIL] runtime"));
    EXPECT_THAT(wrong_token.stdout_text, HasSubstr("must equal the pod's RR_RUNTIME_TOKEN"));

    runpod().add_pod("pod123", "EXITED");
    const proc::RunResult stopped = rreport({"doctor"});
    EXPECT_EQ(stopped.status.code, 0) << "a stopped pod is how it should be when idle";
    EXPECT_THAT(stopped.stdout_text, HasSubstr("[warn] runtime: not checked: the pod is EXITED"));
    EXPECT_THAT(stopped.stdout_text, HasSubstr("rreport pod start"));

    const proc::RunResult bad_key = rreport({"doctor"}, {"runpod.api_key=wrong"});
    EXPECT_EQ(bad_key.status.code, 1);
    EXPECT_THAT(bad_key.stdout_text, HasSubstr("RunPod refused the API key"));
}

TEST_F(Hub, StartsStopsAndReportsThePod) {
    runpod().add_pod("pod123", "EXITED");
    const proc::RunResult started = rreport({"pod", "start"});
    EXPECT_EQ(started.status.code, 0) << started.stderr_text;
    EXPECT_THAT(started.stdout_text, HasSubstr("pod pod123 is RUNNING"));
    EXPECT_THAT(started.stdout_text, HasSubstr("every engine ready"));
    EXPECT_EQ(runpod().actions("start"), 1);

    const proc::RunResult status = rreport({"pod", "status"});
    EXPECT_EQ(status.status.code, 0);
    EXPECT_THAT(status.stdout_text, HasSubstr("$0.44/h"));
    EXPECT_THAT(status.stdout_text, HasSubstr("squad (infantry): ready"));

    const proc::RunResult stopped = rreport({"pod", "stop"});
    EXPECT_EQ(stopped.status.code, 0);
    EXPECT_THAT(stopped.stdout_text, HasSubstr("pod pod123 is EXITED"));
    EXPECT_EQ(runpod().status("pod123"), "EXITED");
}

TEST_F(Hub, BenchMeasuresThePodAndKeepsTheResults) {
    const std::filesystem::path out = home() / "bench.json";
    const proc::RunResult result = rreport({"bench", "--parallel", "1,2,6", "--output-tokens", "8",
                                            "--rounds", "2", "--prompt-tokens", "64", "--out", out.string()});
    ASSERT_EQ(result.status.code, 0) << result.stdout_text << result.stderr_text;
    EXPECT_THAT(result.stdout_text, HasSubstr("## Commander `chat`"));
    EXPECT_THAT(result.stdout_text, HasSubstr("## Infantry `squad`"));
    EXPECT_THAT(result.stdout_text, HasSubstr("| 6 (queued) | 4 |")) << "more requests than slots";
    EXPECT_THAT(result.stdout_text, HasSubstr("Price: $0.44 per hour")) << "RunPod's price for the pod";

    const json::Json saved = unwrap(json::parse(unwrap(read_file(out))));
    EXPECT_EQ(saved["commander"]["model"], "chat");
    EXPECT_EQ(saved["commander"]["rounds"], 2);
    ASSERT_EQ(saved["infantry"]["rows"].size(), 3U);
    EXPECT_EQ(saved["infantry"]["rows"][1]["tokens"], 16) << "two answers of exactly eight tokens";
    EXPECT_EQ(saved["infantry"]["rows"][2]["failed"], 0);
    EXPECT_GT(saved["commander"]["generation_tokens_per_second"].get<double>(), 0);

    // Every call is accounted for in the library: 2 + 1 + 2 + 6.
    db::Database library =
        unwrap(library::open_database(data_dir() / "libraries" / "default" / "library.db"));
    const library::CallLog calls{library};
    EXPECT_EQ(unwrap(calls.list({.purpose = "bench"})).size(), 11U);
}

TEST_F(Hub, BenchTimesAColdStart) {
    const std::filesystem::path out = home() / "cold.json";
    const proc::RunResult result =
        rreport({"bench", "--cold-start", "--parallel", "1", "--output-tokens", "4", "--rounds", "1",
                 "--prompt-tokens", "32", "--out", out.string()});
    ASSERT_EQ(result.status.code, 0) << result.stdout_text << result.stderr_text;
    EXPECT_THAT(result.stdout_text, HasSubstr("Cold start:"));
    EXPECT_EQ(runpod().actions("stop"), 1);
    EXPECT_EQ(runpod().actions("start"), 1);
    EXPECT_TRUE(unwrap(json::parse(unwrap(read_file(out))))["cold_start_ms"].is_number());
}

TEST_F(Hub, ConfigurationMistakesAndSecrets) {
    const proc::RunResult typo = rreport({"doctor"}, {"runtime.tokn=x"});
    EXPECT_EQ(typo.status.code, 2);
    EXPECT_THAT(typo.stderr_text, HasSubstr("did you mean 'runtime.token'"));

    const proc::RunResult shown = rreport({"config", "show", "--origin"});
    EXPECT_EQ(shown.status.code, 0);
    EXPECT_THAT(shown.stdout_text, HasSubstr("RREPORT_RUNTIME__TOKEN")) << "where the token came from";
    EXPECT_THAT(shown.stdout_text, Not(HasSubstr(std::string{kToken}))) << "never the token itself";
    EXPECT_THAT(shown.stdout_text, Not(HasSubstr(std::string{FakeRunpod::kAccountKey})));

    const proc::RunResult version = rreport({"version"});
    EXPECT_EQ(version.status.code, 0);
    EXPECT_THAT(version.stdout_text, HasSubstr("rreport 0."));
}

}  // namespace
}  // namespace rr
