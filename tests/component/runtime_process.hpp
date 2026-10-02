// tests/component/runtime_process.hpp — rreport-runtime as a process, serving
// a test's catalog of fake engines on free ports.
//
// Why this exists
//   The component tests drive the real executables from the outside. Each
//   needs a runtime whose engines are rr_fake_engine processes, scripted by
//   the catalog's extra_args: the CLI tests a commander and infantry of
//   their own sizes, the end-to-end test engines that answer as the oracle.
//   This starts one, waits until it listens, and stops it when the test is
//   done.
//
// Thread-safety: port() may be read from any thread; the rest from the
//   test's.
#pragma once

#include <chrono>
#include <csignal>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rr/core/hash.hpp>
#include <rr/core/strings.hpp>
#include <rr/proc/process.hpp>
#include <rr/testkit/ports.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::component {

inline constexpr std::string_view kRuntimeToken = "component-runtime-token";
// What every model file holds: pretend weights, so its checksum is known.
inline constexpr std::string_view kModelWeights = "pretend these are gigabytes of weights";

// rreport-runtime serving profile "test" of a catalog, with rr_fake_engine
// as llama-server. Its engines listen on engine_port(0), engine_port(1), …
// in the profile's order.
class RuntimeProcess {
public:
    // `catalog` is the catalog's TOML, "{sha}" standing for the SHA-256 of
    // the model files `files` names, each written with kModelWeights.
    RuntimeProcess(std::string_view catalog, const std::vector<std::string>& files) {
        for (const std::string& file : files) {
            (void)dir_.write_file("models/" + file, kModelWeights);
        }
        (void)dir_.write_file("catalog.toml", replace_all(catalog, "{sha}", sha256(kModelWeights).hex()));
        first_engine_port_ = testkit::free_port_range(static_cast<int>(files.size()));
        std::vector<std::string> argv{RR_RUNTIME_BINARY, "serve"};
        for (const std::string& setting :
             {std::string{"server.host=127.0.0.1"}, std::string{"server.port=0"},
              std::format("models.catalog={}", (dir_ / "catalog.toml").string()),
              std::format("models.dir={}", (dir_ / "models").string()), std::string{"models.profile=test"},
              std::format("blobs.dir={}", (dir_ / "blobs").string()),
              std::format("engines.llama_server={}", RR_FAKE_ENGINE),
              std::format("engines.first_port={}", first_engine_port_), std::string{"idle.stop_after=0s"},
              std::string{"log.level=warn"}}) {
            argv.emplace_back("--set");
            argv.push_back(setting);
        }
        process_.emplace(testkit::unwrap(proc::ChildProcess::start(
            proc::Command{.argv = std::move(argv),
                          .environment = {{"RR_RUNTIME_TOKEN", std::string{kRuntimeToken}}},
                          .inherit_environment = false},
            proc::ChildProcess::Options{.on_output = [this](std::string_view line) {
                constexpr std::string_view kListening = "listening on 127.0.0.1:";
                if (line.starts_with(kListening)) {
                    const std::lock_guard lock(mutex_);
                    port_ = std::stoi(std::string{line.substr(kListening.size())});
                }
            }})));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (port() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        if (port() == 0) {
            throw std::runtime_error("the runtime did not start");
        }
    }
    RuntimeProcess(const RuntimeProcess&) = delete;
    RuntimeProcess& operator=(const RuntimeProcess&) = delete;
    RuntimeProcess(RuntimeProcess&&) = delete;
    RuntimeProcess& operator=(RuntimeProcess&&) = delete;
    ~RuntimeProcess() {
        if (process_.has_value()) {
            (void)::kill(process_->pid(), SIGTERM);
            (void)process_->wait(std::chrono::seconds{15});
        }
    }

    [[nodiscard]] int port() const {
        const std::lock_guard lock(mutex_);
        return port_;
    }
    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}", port()); }
    // The port of the profile's `index`th engine (from 0).
    [[nodiscard]] int engine_port(int index) const { return first_engine_port_ + index; }

private:
    testkit::TempDir dir_{"rr_component_runtime"};
    int first_engine_port_ = 0;
    mutable std::mutex mutex_;
    int port_ = 0;
    std::optional<proc::ChildProcess> process_;
};

}  // namespace rr::component
