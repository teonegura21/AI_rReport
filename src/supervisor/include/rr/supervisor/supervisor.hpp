// rr/supervisor/supervisor.hpp — the runtime: one HTTP door to the GPU pod.
//
// Why this exists
//   This is `rreport-runtime` minus argument parsing (ADR-0001, ADR-0003).
//   It wires together:
//     * the HTTP server, with bearer-token authentication on every endpoint
//       except /ping (checked in constant time, before any body is read);
//     * one Engine per model in the chosen GPU profile, on localhost ports;
//     * the OpenAI-compatible proxy, routing by the request's "model";
//     * content-addressed blob uploads, verified against their hash and kept
//       in one store per library;
//     * the Watchdog: when idle long enough, it runs the configured stop
//       command (on RunPod, `runpodctl stop pod $RUNPOD_POD_ID`), and it
//       clears blobs that have not been used for `blob_retention`.
//   Protocol details: rr/runtime_api/protocol.hpp.
//
// Thread-safety: start() and stop() from one thread; the server's handlers
//   run concurrently, and everything they share is synchronized.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/core/secret.hpp>
#include <rr/runtime_api/protocol.hpp>
#include <rr/supervisor/catalog.hpp>

namespace rr::supervisor {

struct SupervisorSettings {
    // Serving
    std::string host = "0.0.0.0";
    int port = 8080;
    Secret token{};  // required: the hub's bearer token
    std::size_t worker_threads = 16;

    // Engines
    Catalog catalog{};
    std::string profile{};  // a name from the catalog
    std::filesystem::path models_dir{};
    std::string llama_server = "llama-server";
    int first_engine_port = 18080;
    std::chrono::milliseconds engine_startup_timeout{std::chrono::minutes{15}};
    std::chrono::milliseconds engine_stop_grace{std::chrono::seconds{20}};
    std::chrono::milliseconds engine_restart_backoff{std::chrono::seconds{2}};
    std::chrono::milliseconds engine_poll_interval{500};
    std::chrono::milliseconds proxy_idle_timeout{std::chrono::minutes{10}};
    // Streamed calls ("stream": true): the longest the connection to the hub
    // may stay silent (RunPod's proxy closes silent connections after about
    // 100 s), and how long to wait for an engine's status before answering
    // 200 on its behalf. std::nullopt turns keep-alives off.
    std::optional<std::chrono::milliseconds> stream_keep_alive{std::chrono::seconds{15}};
    std::chrono::milliseconds stream_head_patience{std::chrono::seconds{1}};
    std::function<void(const std::string& model, std::string_view line)> on_engine_output{};

    // Blobs
    std::filesystem::path blobs_dir{};
    std::uint64_t max_upload_bytes = std::uint64_t{8} << 30U;
    std::chrono::milliseconds blob_retention{std::chrono::hours{24}};

    // Stopping the pod
    std::optional<std::chrono::milliseconds> idle_stop{};  // std::nullopt: never
    std::vector<std::string> stop_command{};               // run once idle; empty: only log
    std::chrono::milliseconds watchdog_interval{std::chrono::seconds{15}};

    // Seams for tests
    const Clock* clock = nullptr;                                             // null: the system clock
    std::function<Result<std::vector<runtime_api::GpuInfo>>()> query_gpus{};  // null: nvidia-smi
    std::string version = "dev";
};

class Supervisor {
public:
    // Validates the settings (a token is required; the profile must exist).
    [[nodiscard]] static Result<std::unique_ptr<Supervisor>> create(SupervisorSettings settings);

    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    Supervisor(Supervisor&&) = delete;
    Supervisor& operator=(Supervisor&&) = delete;
    ~Supervisor();  // stops

    // Starts the engines (they load in the background), the server and the watchdog.
    [[nodiscard]] Result<void> start();
    // Stops accepting requests, then the engines. Idempotent.
    void stop();

    [[nodiscard]] int port() const noexcept;
    [[nodiscard]] runtime_api::HealthReport health() const;
    // True once the watchdog has decided the pod should stop (and ran the command).
    [[nodiscard]] bool stop_requested() const noexcept;

private:
    struct Impl;
    explicit Supervisor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rr::supervisor
