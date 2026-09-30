// rr/runpod/runpod.hpp — RunPod's pod API: read a pod, start it, stop it.
//
// Why this exists
//   The hub starts the GPU pod when there is work and stops it when there is
//   none (plan §4.3), and the pod stops itself once idle (the dead-man
//   switch). Both speak to RunPod through this client, which follows RunPod's
//   REST API v2 (https://api.runpod.io/v2; v1 retires on 2026-11-15):
//     GET  /pods/{id}             the pod, with its status and cost
//     POST /pods/{id}/action      {"action": "start" | "stop"}
//
//   Failures keep RunPod's own explanation (RFC 9457 problem details) and
//   get the class that says what to do:
//     401, 403  permission_denied  the key is wrong, or may not do this
//     404       not_found          no such pod (or not this key's)
//     409       conflict           not possible in the pod's current status
//     400 with "not enough free GPUs"
//               unavailable        a stopped pod resumes on its own machine
//                                  only, and that machine is full; wait, or
//                                  create a new pod on the same network volume
//     429, 5xx  rate_limited, unavailable (retried; Retry-After is honoured)
//
//   A pod stopping itself uses the pod-scoped key RunPod gives it
//   (RUNPOD_API_KEY). Reports from 2026 say the REST API refuses that key
//   while the older GraphQL API accepts it, so stop_self() tries REST first
//   and falls back to GraphQL's podStop (retiring in early 2027) when REST
//   refuses the key. Whether the pod key may stop its pod is checked on a real
//   pod (deploy/runpod/README.md); a key the owner creates works either way.
//
// Thread-safety: thread-safe; requests may run concurrently.
#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/secret.hpp>
#include <rr/net/http_client.hpp>
#include <rr/net/retry.hpp>

namespace rr::runpod {

inline constexpr std::string_view kDefaultApiUrl = "https://api.runpod.io/v2";
inline constexpr std::string_view kDefaultGraphqlUrl = "https://api.runpod.io/graphql";

enum class PodStatus : std::uint8_t {
    provisioning,
    starting,
    running,  // the container is up (the runtime inside may still be loading)
    exited,   // stopped: no compute is billed; /workspace is kept
    error,
    terminated,
    unknown  // a status this client does not know yet
};

[[nodiscard]] std::string_view to_string(PodStatus status) noexcept;
[[nodiscard]] PodStatus pod_status_from_string(std::string_view name) noexcept;  // "RUNNING" → running

struct Pod {
    std::string id{};
    std::string name{};
    PodStatus status = PodStatus::unknown;
    std::string status_name{};           // as RunPod wrote it
    std::vector<std::string> actions{};  // what the pod accepts now ("start", "stop", …)
    std::string gpu{};                   // e.g. "NVIDIA GeForce RTX 4090"
    int gpu_count = 0;
    double cost_per_hour = 0;                // USD; 0 while stopped
    std::optional<std::int64_t> uptime_s{};  // while running
    std::string data_center{};
};

// RunPod's proxy URL for an HTTP port of a pod: https://<pod>-<port>.proxy.runpod.net.
[[nodiscard]] std::string proxy_url(std::string_view pod_id, int port);

// A pod id: 1–64 characters of [A-Za-z0-9]. It goes into URLs and queries,
// so anything else is refused rather than escaped.
[[nodiscard]] Result<void> validate_pod_id(std::string_view pod_id);

struct Options {
    Secret api_key{};
    std::string api_url{kDefaultApiUrl};
    std::string graphql_url{kDefaultGraphqlUrl};
};

class RunpodApi {
public:
    // `http` must outlive the client.
    RunpodApi(const net::HttpClient& http, Options options, net::RetryPolicy retry = {});

    [[nodiscard]] Result<Pod> get_pod(std::string_view pod_id, const std::stop_token& stop = {}) const;
    // The pod as RunPod accepted the action; it keeps moving (STARTING, then
    // RUNNING; or on to EXITED). Errc::conflict when the pod's status does
    // not allow the action: see Pod::actions.
    [[nodiscard]] Result<Pod> start_pod(std::string_view pod_id, const std::stop_token& stop = {}) const;
    [[nodiscard]] Result<Pod> stop_pod(std::string_view pod_id, const std::stop_token& stop = {}) const;

    // Stops the pod this code runs in, with the pod's own key (see the header comment).
    [[nodiscard]] Result<void> stop_self(std::string_view pod_id, const std::stop_token& stop = {}) const;

private:
    [[nodiscard]] Result<Pod> act(std::string_view pod_id, std::string_view action,
                                  const std::stop_token& stop) const;
    [[nodiscard]] Result<void> stop_with_graphql(std::string_view pod_id, const std::stop_token& stop) const;
    [[nodiscard]] net::HttpClient::Request request(net::Method method, std::string url) const;

    const net::HttpClient* http_;
    Options options_;
    net::RetryPolicy retry_;
};

}  // namespace rr::runpod
