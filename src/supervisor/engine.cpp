#include <rr/supervisor/engine.hpp>

#include <algorithm>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include <rr/net/http_client.hpp>
#include <rr/proc/process.hpp>

namespace rr::supervisor {

using runtime_api::EngineState;

namespace {

constexpr std::chrono::milliseconds kMaxBackoff{60'000};
constexpr std::chrono::milliseconds kHealthTimeout{2000};

}  // namespace

struct Engine::State {
    Settings settings;
    net::HttpClient health_client{net::HttpClient::Options{.connect_timeout = kHealthTimeout}};

    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;  // state changes, and interruptible waits
    runtime_api::EngineInfo info;
    std::jthread manager;  // declared last: joined before the members above go away

    void set_state(EngineState state, std::optional<std::string> error = std::nullopt) {
        {
            const std::lock_guard lock(mutex);
            info.state = state;
            if (error.has_value()) {
                info.last_error = std::move(error);
            }
        }
        changed.notify_all();
    }

    // Sleeps for `delay` unless a stop is requested first; false when stopped.
    bool pause(std::chrono::milliseconds delay, const std::stop_token& stop) {
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, stop, delay, [] { return false; });
        return !stop.stop_requested();
    }

    [[nodiscard]] bool healthy() const {
        const auto response = health_client.send(net::HttpClient::Request{
            .url = std::format("http://127.0.0.1:{}/health", settings.port), .timeout = kHealthTimeout});
        return response.has_value() && response->status == 200;
    }

    // One life of the process: start it, wait for it to become ready, serve
    // until it exits or a stop is requested. Returns why it ended (nullopt
    // when stopped on request), and whether it had become ready.
    struct Outcome {
        std::optional<std::string> failure;
        bool was_ready = false;
    };

    Outcome run_once(const std::stop_token& stop) {
        set_state(EngineState::starting);
        auto child = proc::ChildProcess::start(
            proc::Command{.argv = settings.command},
            proc::ChildProcess::Options{.on_output = settings.on_output, .stop_grace = settings.stop_grace});
        if (!child.has_value()) {
            return Outcome{.failure = child.error().describe()};
        }

        // Starting: wait for /health to answer 200.
        const auto deadline = std::chrono::steady_clock::now() + settings.startup_timeout;
        bool ready = false;
        while (!stop.stop_requested() && child->running()) {
            if (healthy()) {
                ready = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                child->stop();
                return Outcome{
                    .failure = std::format(
                        "not ready after {} s",
                        std::chrono::duration_cast<std::chrono::seconds>(settings.startup_timeout).count())};
            }
            (void)pause(settings.poll_interval, stop);
        }
        if (ready) {
            set_state(EngineState::ready);
            // Ready: serve until the process ends or we are told to stop.
            while (!stop.stop_requested() && child->running()) {
                (void)child->wait(settings.poll_interval);
            }
        }
        if (stop.stop_requested()) {
            child->stop();
            return Outcome{.failure = std::nullopt, .was_ready = ready};
        }
        const auto status = child->exit_status();
        return Outcome{
            .failure = std::format("the process ended ({})",
                                   status.has_value() ? status->describe() : std::string{"unknown"}),
            .was_ready = ready};
    }

    void manage(const std::stop_token& stop) {
        int consecutive_failures = 0;
        while (!stop.stop_requested()) {
            const Outcome outcome = run_once(stop);
            if (stop.stop_requested()) {
                break;
            }
            consecutive_failures = outcome.was_ready ? 1 : consecutive_failures + 1;
            {
                const std::lock_guard lock(mutex);
                ++info.restarts;
            }
            if (consecutive_failures >= settings.max_consecutive_failures) {
                set_state(EngineState::failed,
                          std::format("gave up after {} consecutive failures; last: {}", consecutive_failures,
                                      outcome.failure.value_or("unknown")));
                return;
            }
            set_state(EngineState::failed, outcome.failure);
            const auto backoff = std::min(
                settings.restart_backoff * (1 << std::min(consecutive_failures - 1, 16)), kMaxBackoff);
            if (!pause(backoff, stop)) {
                break;
            }
        }
        set_state(EngineState::stopped);
    }
};

Engine::Engine(std::string model, std::string role, int slots, Settings settings)
    : state_(std::make_unique<State>()) {
    state_->settings = std::move(settings);
    state_->info.model = std::move(model);
    state_->info.role = std::move(role);
    state_->info.slots = slots;
}

Engine::~Engine() {
    stop();
}

void Engine::start() {
    if (state_->manager.joinable()) {
        return;
    }
    state_->manager =
        std::jthread{[state = state_.get()](const std::stop_token& stop) { state->manage(stop); }};
}

void Engine::stop() {
    if (!state_->manager.joinable()) {
        return;
    }
    state_->manager.request_stop();
    state_->changed.notify_all();
    state_->manager.join();
}

runtime_api::EngineInfo Engine::info() const {
    const std::lock_guard lock(state_->mutex);
    return state_->info;
}

bool Engine::ready() const {
    const std::lock_guard lock(state_->mutex);
    return state_->info.state == EngineState::ready;
}

const std::string& Engine::model() const noexcept {
    return state_->info.model;  // set at construction, never changed
}

std::string Engine::base_url() const {
    return std::format("http://127.0.0.1:{}", state_->settings.port);
}

Result<void> Engine::wait_until_settled(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(state_->mutex);
    const bool settled = state_->changed.wait_for(lock, timeout, [this] {
        return state_->info.state == EngineState::ready || state_->info.state == EngineState::failed;
    });
    if (!settled) {
        return fail(Errc::timeout, std::format("engine '{}' is still starting", state_->info.model));
    }
    if (state_->info.state == EngineState::failed) {
        return fail(Errc::unavailable, std::format("engine '{}' failed: {}", state_->info.model,
                                                   state_->info.last_error.value_or("unknown")));
    }
    return ok();
}

}  // namespace rr::supervisor
