// A runtime for the runtime client's tests: a real Supervisor serving one
// profile ("chat", then "embed") of rr_fake_engine processes.
#pragma once

#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/runtime_client/runtime_client.hpp>
#include <rr/supervisor/supervisor.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::runtime_client::testing {

using namespace std::chrono_literals;

constexpr std::string_view kToken = "runtime-client-token";

// A runtime: a Supervisor serving one profile of fake engines.
class Runtime {
public:
    explicit Runtime(std::string_view engine_args = R"("--fake-load-ms", "100")") {
        supervisor::SupervisorSettings settings;
        settings.host = "127.0.0.1";
        settings.port = 0;
        settings.token = Secret{std::string{kToken}};
        settings.catalog =
            testkit::unwrap(supervisor::parse_catalog(std::format(R"toml(
[[model]]
id = "chat"
role = "commander"
file = "chat.gguf"
sha256 = "{0}"
[[model]]
id = "embed"
role = "embedder"
file = "embed.gguf"
sha256 = "{0}"

[[profile]]
name = "test"
  [[profile.engine]]
  model = "chat"
  extra_args = [{1}]
  [[profile.engine]]
  model = "embed"
)toml",
                                                                  std::string(64, 'a'), engine_args)));
        settings.profile = "test";
        settings.models_dir = dir_ / "models";
        settings.blobs_dir = dir_ / "blobs";
        settings.llama_server = RR_FAKE_ENGINE;
        settings.first_engine_port = testkit::free_port_range(2);
        settings.engine_startup_timeout = 20s;
        settings.engine_stop_grace = 2s;
        settings.engine_restart_backoff = 50ms;
        settings.engine_poll_interval = 50ms;
        settings.watchdog_interval = 20ms;
        settings.clock = &clock_;
        settings.version = "test-version";
        settings.query_gpus = [] {
            return Result<std::vector<runtime_api::GpuInfo>>{std::vector<runtime_api::GpuInfo>{
                {.index = 0, .name = "Fake GPU", .memory_total_mib = 24576, .driver_version = "0.0"}}};
        };
        supervisor_ = testkit::unwrap(supervisor::Supervisor::create(std::move(settings)));
        testkit::unwrap(supervisor_->start());
    }

    [[nodiscard]] Endpoint endpoint(std::string_view token = kToken) const {
        return Endpoint{.url = std::format("http://127.0.0.1:{}/", supervisor_->port()),
                        .token = Secret{std::string{token}}};
    }
    [[nodiscard]] FakeClock& clock() { return clock_; }
    [[nodiscard]] const testkit::TempDir& dir() const { return dir_; }

private:
    testkit::TempDir dir_{"rr_runtime_client_test"};
    FakeClock clock_;
    std::unique_ptr<supervisor::Supervisor> supervisor_;
};

}  // namespace rr::runtime_client::testing
