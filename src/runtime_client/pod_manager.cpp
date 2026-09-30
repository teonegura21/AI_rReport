#include <rr/runtime_client/pod_manager.hpp>

#include <algorithm>
#include <format>
#include <utility>

#include <rr/log/log.hpp>
#include <rr/net/retry.hpp>

namespace rr::runtime_client {

namespace {

using Clock = std::chrono::steady_clock;
using runpod::PodStatus;
using std::chrono::milliseconds;

// How long RunPod may go on reporting a pod's old status after accepting a start.
constexpr milliseconds kStartGrace{60'000};

milliseconds until(Clock::time_point deadline) {
    return std::max(milliseconds{0}, std::chrono::duration_cast<milliseconds>(deadline - Clock::now()));
}

}  // namespace

PodManager::PodManager(const runpod::RunpodApi& api, std::string pod_id, const RuntimeClient& runtime)
    : api_(&api)
    , pod_id_(std::move(pod_id))
    , runtime_(&runtime) {}

Result<runpod::Pod> PodManager::status(const std::stop_token& stop) const {
    return api_->get_pod(pod_id_, stop);
}

Result<ReadyPod> PodManager::ensure_ready(milliseconds timeout, const std::stop_token& stop,
                                          milliseconds poll_every) const {
    const auto began = Clock::now();
    const auto deadline = began + timeout;
    RR_TRY_ASSIGN(runpod::Pod pod, api_->get_pod(pod_id_, stop));
    if (pod.status == PodStatus::terminated) {
        return fail(Errc::not_found,
                    std::format("pod {} was terminated and cannot start again: create a new pod", pod_id_));
    }
    bool started = false;
    const PodStatus stopped_as = pod.status;
    if (pod.status == PodStatus::exited || pod.status == PodStatus::error) {
        log::info("starting pod {} ({})", pod_id_, pod.gpu.empty() ? std::string{"GPU unknown"} : pod.gpu);
        auto starting = api_->start_pod(pod_id_, stop);
        if (starting.has_value()) {
            pod = std::move(*starting);
        } else if (starting.error().code() == Errc::conflict) {
            RR_TRY_ASSIGN(pod, api_->get_pod(pod_id_, stop));  // someone else moved it: see where it is
        } else {
            return fail(std::move(starting).error());
        }
        started = true;
    }
    const auto start_accepted = Clock::now();

    while (pod.status != PodStatus::running) {
        // After a start, RunPod may report the old status for a while.
        const bool settling =
            started && pod.status == stopped_as && Clock::now() - start_accepted < kStartGrace;
        const bool down = pod.status == PodStatus::exited || pod.status == PodStatus::error ||
                          pod.status == PodStatus::terminated;
        if (down && !settling) {
            return fail(Errc::unavailable,
                        std::format("pod {} is {} instead of starting: see its logs in RunPod's console",
                                    pod_id_, pod.status_name));
        }
        if (Clock::now() >= deadline) {
            return fail(Errc::timeout,
                        std::format("pod {} was still {} after {} s", pod_id_, pod.status_name,
                                    std::chrono::duration_cast<std::chrono::seconds>(timeout).count()));
        }
        if (!net::pause_for(std::min(poll_every, until(deadline)), stop)) {
            return fail(Errc::cancelled, "stopped while waiting for the pod");
        }
        RR_TRY_ASSIGN(pod, api_->get_pod(pod_id_, stop));
    }

    RR_TRY_ASSIGN(runtime_api::HealthReport health,
                  with_context(runtime_->wait_until_ready(until(deadline), stop, poll_every),
                               std::format("while pod {} loads its models", pod_id_)));
    return ReadyPod{.pod = std::move(pod),
                    .health = std::move(health),
                    .started = started,
                    .waited = std::chrono::duration_cast<milliseconds>(Clock::now() - began)};
}

Result<runpod::Pod> PodManager::stop_pod(const std::stop_token& stop) const {
    RR_TRY_ASSIGN(runpod::Pod pod, api_->get_pod(pod_id_, stop));
    if (pod.status == PodStatus::exited || pod.status == PodStatus::terminated) {
        return pod;
    }
    auto stopped = api_->stop_pod(pod_id_, stop);
    if (!stopped.has_value() && stopped.error().code() == Errc::conflict) {
        return api_->get_pod(pod_id_, stop);  // it changed meanwhile; say where it is now
    }
    return stopped;
}

}  // namespace rr::runtime_client
