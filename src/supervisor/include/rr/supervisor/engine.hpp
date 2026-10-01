// rr/supervisor/engine.hpp — one model server process, kept alive.
//
// Why this exists
//   Each resident model is served by its own llama-server process (ADR-0003:
//   GPU engines are processes, so a crash cannot take the supervisor down
//   and VRAM is released for certain when one exits). An Engine owns one
//   such process and runs its lifecycle on a thread of its own:
//
//     starting ──(GET /health answers 200)──▶ ready
//        │  ▲                                  │
//        │  └──── restart, with backoff ◀──────┤ the process exited
//        ▼                                     ▼
//      failed (startup timeout, or too many consecutive failures)
//
//   * Readiness is llama-server's own signal: /health answers 503 while the
//     model loads (minutes for a large model), then 200.
//   * Before the engine is declared ready, it is asked how much context
//     each slot really has (/props): llama-server caps the catalog's
//     arithmetic at the model's training context, and the hub sizes its
//     windows by the result. If it cannot say, the catalog's figure stands,
//     and the discrepancy or the failure is logged.
//   * A crash is followed by a restart after an exponentially growing pause.
//     The count of consecutive failures resets once the engine becomes
//     ready, so a model that works but crashes once a day is not given up.
//   * Every wait is interruptible: stop() returns promptly, and the process
//     is stopped with SIGTERM, then SIGKILL after the grace period.
//
// Thread-safety: all public members are thread-safe.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/runtime_api/protocol.hpp>
#include <rr/supervisor/catalog.hpp>

namespace rr::supervisor {

class Engine {
public:
    struct Settings {
        std::vector<std::string> command{};  // the full argv (see llama_server_command())
        int port = 0;                        // where the process listens, on 127.0.0.1
        std::chrono::milliseconds startup_timeout{std::chrono::minutes{15}};
        std::chrono::milliseconds stop_grace{std::chrono::seconds{20}};
        std::chrono::milliseconds poll_interval{500};
        std::chrono::milliseconds restart_backoff{
            std::chrono::seconds{2}};  // doubles per consecutive failure
        int max_consecutive_failures = 4;
        std::function<void(std::string_view line)> on_output{};  // the process's log lines
    };

    // Serves `model` as `spec` says (the health report describes it so);
    // settings.command is the process that does it (llama_server_command()).
    Engine(const Model& model, const EngineSpec& spec, Settings settings);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine();  // stops

    // Starts managing the process (returns at once; loading continues in the background).
    void start();
    // Stops the process and the management thread. Idempotent.
    void stop();

    [[nodiscard]] runtime_api::EngineInfo info() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] const std::string& model() const noexcept;
    // "http://127.0.0.1:<port>"
    [[nodiscard]] std::string base_url() const;
    // Waits until the engine is ready (ok) or has failed (its last error),
    // or `timeout` passes (Errc::timeout).
    [[nodiscard]] Result<void> wait_until_settled(std::chrono::milliseconds timeout) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace rr::supervisor
