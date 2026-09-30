// rr/runtime_client/pod_manager.hpp — the hub's control of its GPU pod.
//
// Why this exists
//   The pod is rented while there is work (plan §4.3). PodManager starts it
//   when the hub needs it, so the pod boots and loads its models while the
//   hub does its own share (capturing a video), waits until the runtime
//   reports every engine ready, and stops the pod when the work is done. The
//   pod also stops itself once idle (the runtime's dead-man switch), so a hub
//   that crashes cannot leave it billing.
//
//   * ensure_ready(): a RUNNING pod is waited for until its engines are
//     ready; an EXITED (or ERROR) pod is started first; a pod PROVISIONING
//     or STARTING is waited for. A TERMINATED pod cannot come back.
//   * RunPod may refuse to start a stopped pod whose machine is full
//     (Errc::unavailable, with RunPod's explanation): reported at once, not
//     retried for hours.
//   * A pod that falls back to EXITED or ERROR while booting ends the wait.
//   * stop_pod(): stops a pod that is up or coming up; one that is already
//     stopped is left alone.
//
// Thread-safety: thread-safe (const operations over thread-safe clients).
#pragma once

#include <chrono>
#include <stop_token>
#include <string>

#include <rr/core/result.hpp>
#include <rr/runpod/runpod.hpp>
#include <rr/runtime_api/protocol.hpp>
#include <rr/runtime_client/runtime_client.hpp>

namespace rr::runtime_client {

struct ReadyPod {
    runpod::Pod pod{};                   // as RunPod reported it, running
    runtime_api::HealthReport health{};  // every engine ready
    bool started = false;                // this call started the pod
    std::chrono::milliseconds waited{};  // from the call until ready: a cold start, if `started`
};

class PodManager {
public:
    // `api` and `runtime` must outlive the manager; `runtime` must be aimed
    // at this pod's runtime.
    PodManager(const runpod::RunpodApi& api, std::string pod_id, const RuntimeClient& runtime);

    [[nodiscard]] const std::string& pod_id() const noexcept { return pod_id_; }

    [[nodiscard]] Result<runpod::Pod> status(const std::stop_token& stop = {}) const;

    // How often RunPod and the runtime are asked while waiting.
    static constexpr std::chrono::milliseconds kPoll{5'000};

    // The pod up and its runtime ready, within `timeout` (see the header comment).
    [[nodiscard]] Result<ReadyPod> ensure_ready(std::chrono::milliseconds timeout,
                                                const std::stop_token& stop = {},
                                                std::chrono::milliseconds poll_every = kPoll) const;

    // The pod stopped, or on its way there.
    [[nodiscard]] Result<runpod::Pod> stop_pod(const std::stop_token& stop = {}) const;

private:
    const runpod::RunpodApi* api_;
    std::string pod_id_;
    const RuntimeClient* runtime_;
};

}  // namespace rr::runtime_client
