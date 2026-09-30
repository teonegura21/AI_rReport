// The rreport-runtime executable, driven from the outside the way RunPod and
// the hub drive it: configuration and its mistakes, the model check before
// serving, the line scripts wait for, and a clean stop on SIGTERM that takes
// the engines down with it.
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

#include <rr/core/hash.hpp>
#include <rr/net/http_client.hpp>
#include <rr/proc/process.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using ::testing::Not;
using testkit::unwrap;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kToken = "component-token";
constexpr std::string_view kModelBytes = "pretend these are gigabytes of weights";

bool eventually(const std::function<bool()>& done, std::chrono::milliseconds within) {
    const auto deadline = Clock::now() + within;
    while (!done()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

// A pod's files: a catalog whose one model file is present and correct.
class Pod {
public:
    Pod() {
        (void)dir_.write_file("models/chat.gguf", kModelBytes);
        (void)dir_.write_file("catalog.toml", std::format(R"toml(
[[model]]
id = "chat"
role = "commander"
file = "chat.gguf"
sha256 = "{}"

[[profile]]
name = "test"
  [[profile.engine]]
  model = "chat"
  extra_args = ["--fake-load-ms", "100"]
)toml",
                                                          sha256(kModelBytes).hex()));
    }

    [[nodiscard]] const testkit::TempDir& dir() const { return dir_; }
    [[nodiscard]] int engine_port() const { return engine_port_; }

    // A runtime command line: `command`, then settings pointing it at these
    // files, the fake engine and free ports.
    [[nodiscard]] std::vector<std::string> argv(std::vector<std::string> command) const {
        std::vector<std::string> argv{RR_RUNTIME_BINARY};
        argv.insert(argv.end(), command.begin(), command.end());
        for (const std::string& setting :
             {std::string{"server.host=127.0.0.1"}, std::string{"server.port=0"},
              std::format("models.catalog={}", (dir_ / "catalog.toml").string()),
              std::format("models.dir={}", (dir_ / "models").string()), std::string{"models.profile=test"},
              std::format("blobs.dir={}", (dir_ / "blobs").string()),
              std::format("engines.llama_server={}", RR_FAKE_ENGINE),
              std::format("engines.first_port={}", engine_port_), std::string{"idle.stop_after=0s"}}) {
            argv.emplace_back("--set");
            argv.push_back(setting);
        }
        return argv;
    }

    // Runs a command to completion, in a hermetic environment.
    [[nodiscard]] proc::RunResult run(std::vector<std::string> command, bool with_token = true) const {
        return unwrap(proc::run(proc::Command{.argv = argv(std::move(command)),
                                              .environment = environment(with_token),
                                              .inherit_environment = false},
                                proc::RunOptions{.timeout = 30s}));
    }

    [[nodiscard]] static std::vector<EnvironmentVariable> environment(bool with_token) {
        if (!with_token) {
            return {};
        }
        return {EnvironmentVariable{"RR_RUNTIME_TOKEN", std::string{kToken}}};
    }

private:
    testkit::TempDir dir_{"rr_runtime_app"};
    int engine_port_ = testkit::free_port_range(1);
};

TEST(RuntimeApp, ServesUntilSigtermThenStopsItsEngines) {
    const Pod pod;
    std::mutex mutex;
    std::optional<int> port;
    std::string output;
    auto runtime = unwrap(proc::ChildProcess::start(
        proc::Command{
            .argv = pod.argv({"serve"}), .environment = Pod::environment(true), .inherit_environment = false},
        proc::ChildProcess::Options{.on_output = [&](std::string_view line) {
            const std::lock_guard lock(mutex);
            output.append(line).push_back('\n');
            constexpr std::string_view kListening = "listening on 127.0.0.1:";
            if (line.starts_with(kListening)) {
                port = std::stoi(std::string{line.substr(kListening.size())});
            }
        }}));
    const auto seen = [&] {
        const std::lock_guard lock(mutex);
        return std::pair{port, output};
    };
    ASSERT_TRUE(eventually([&] { return seen().first.has_value(); }, 20s)) << seen().second;

    const net::HttpClient client;
    const std::string base = std::format("http://127.0.0.1:{}", *seen().first);
    const net::Headers auth{{"Authorization", std::format("Bearer {}", kToken)}};
    EXPECT_EQ(unwrap(client.send({.url = base + "/ping"})).body, "ok");
    EXPECT_EQ(unwrap(client.send({.url = base + "/v1/health"})).status, 401) << "the token is required";
    ASSERT_TRUE(eventually(
        [&] {
            const auto health = client.send({.url = base + "/v1/health", .headers = auth});
            return health.has_value() && health->body.find(R"("state":"ready")") != std::string::npos;
        },
        20s))
        << seen().second;
    ASSERT_TRUE(testkit::can_connect(pod.engine_port())) << "the engine is up";

    // SIGTERM, as `docker stop` and RunPod send it.
    ASSERT_EQ(::kill(runtime.pid(), SIGTERM), 0);
    const proc::ExitStatus status = unwrap(runtime.wait(15s));
    EXPECT_TRUE(status.success()) << status.describe() << "\n" << seen().second;
    EXPECT_FALSE(testkit::can_connect(pod.engine_port())) << "the engine went down with it";
}

TEST(RuntimeApp, RefusesToServeWithoutAToken) {
    const Pod pod;
    const proc::RunResult result = pod.run({"serve"}, /*with_token=*/false);
    EXPECT_EQ(result.status.code, 2) << result.stderr_text;
    EXPECT_THAT(result.stderr_text, HasSubstr("RR_RUNTIME_TOKEN"));
}

TEST(RuntimeApp, RefusesToServeWrongModelFiles) {
    const Pod pod;
    (void)pod.dir().write_file("models/chat.gguf", "tampered");
    const proc::RunResult result = pod.run({"serve"});
    EXPECT_EQ(result.status.code, 3) << result.stderr_text;
    EXPECT_THAT(result.stdout_text, HasSubstr("mismatch"));
    EXPECT_THAT(result.stderr_text, HasSubstr("rreport-runtime models sync"));
}

TEST(RuntimeApp, VerifiesModelFilesOnRequest) {
    const Pod pod;
    const proc::RunResult good = pod.run({"models", "verify"});
    EXPECT_EQ(good.status.code, 0) << good.stderr_text;
    EXPECT_THAT(good.stdout_text, HasSubstr("verified"));
    EXPECT_THAT(good.stdout_text, HasSubstr("chat.gguf"));

    std::filesystem::remove(pod.dir() / "models" / "chat.gguf");
    EXPECT_EQ(pod.run({"models", "verify"}).status.code, 3);
}

// Also proves a global option given after the command reaches the
// configuration loader, and is not refused by the argument parser: the
// command line holds `--set server.port=0` too, so only the loader's own
// words show who answered.
TEST(RuntimeApp, CatchesMisspeltSettings) {
    const Pod pod;
    const proc::RunResult result = pod.run({"serve", "--set", "server.prot=9000"});
    EXPECT_EQ(result.status.code, 2);
    EXPECT_THAT(result.stderr_text, HasSubstr("unknown configuration key 'server.prot'"));
    EXPECT_THAT(result.stderr_text, HasSubstr("did you mean 'server.port'"));
}

TEST(RuntimeApp, ShowsItsConfigurationWithoutTheToken) {
    const Pod pod;
    const proc::RunResult result = pod.run({"config", "show", "--origin"});
    EXPECT_EQ(result.status.code, 0) << result.stderr_text;
    EXPECT_THAT(result.stdout_text, HasSubstr("server.token"));
    EXPECT_THAT(result.stdout_text, HasSubstr("RR_RUNTIME_SERVER__TOKEN")) << "where it came from";
    EXPECT_THAT(result.stdout_text, Not(HasSubstr(std::string{kToken}))) << "never the secret itself";
}

TEST(RuntimeApp, TellsItsVersion) {
    const Pod pod;
    const proc::RunResult result = pod.run({"version"});
    EXPECT_EQ(result.status.code, 0);
    EXPECT_THAT(result.stdout_text, HasSubstr("rreport-runtime 0."));
}

}  // namespace
}  // namespace rr
