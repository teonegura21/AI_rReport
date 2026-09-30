// rr/runtime_client/runtime_client.hpp — the hub's side of the runtime protocol.
//
// Why this exists
//   The hub talks to the pod's runtime over HTTPS, through RunPod's proxy,
//   with a bearer token (ADR-0001; the endpoints are in
//   rr/runtime_api/protocol.hpp). RuntimeClient is that conversation, typed:
//   liveness, the health report, heartbeats for the dead-man switch, blobs,
//   and waiting until the engines are ready. Model calls go through rr_llm
//   (runtime_provider() below), like calls to any other model API.
//
//   Every request here is safe to repeat (reads, heartbeats, uploads named by
//   their hash), so transient failures are retried: a pod that is restarting
//   an engine, or a proxy that briefly has no route to a booting pod.
//
// Thread-safety: thread-safe; requests may run concurrently.
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/hash.hpp>
#include <rr/core/result.hpp>
#include <rr/core/secret.hpp>
#include <rr/llm/provider.hpp>
#include <rr/net/http_client.hpp>
#include <rr/net/retry.hpp>
#include <rr/runtime_api/protocol.hpp>

namespace rr::runtime_client {

struct Endpoint {
    std::string url{};  // "https://<pod>-8080.proxy.runpod.net", no trailing slash
    Secret token{};     // the runtime's bearer token (RR_RUNTIME_TOKEN on the pod)
};

struct ModelInfo {
    std::string id{};    // what chat requests name as "model"
    std::string role{};  // commander, infantry, embedder, reranker
    std::string state{};

    friend bool operator==(const ModelInfo&, const ModelInfo&) = default;
};

class RuntimeClient {
public:
    // `http` must outlive the client.
    RuntimeClient(const net::HttpClient& http, Endpoint endpoint, net::RetryPolicy retry = {});

    [[nodiscard]] const Endpoint& endpoint() const noexcept { return endpoint_; }

    // The runtime answers at all (no token needed).
    [[nodiscard]] Result<void> ping(const std::stop_token& stop = {}) const;
    [[nodiscard]] Result<runtime_api::HealthReport> health(const std::stop_token& stop = {}) const;
    // "The hub is alive": resets the pod's dead-man switch.
    [[nodiscard]] Result<void> heartbeat(const std::stop_token& stop = {}) const;
    [[nodiscard]] Result<std::vector<ModelInfo>> models(const std::stop_token& stop = {}) const;

    [[nodiscard]] Result<bool> has_blob(std::string_view library, const Blake3Digest& hash,
                                        const std::stop_token& stop = {}) const;
    // Uploads `file`, which must hash to `hash`: the runtime verifies it.
    [[nodiscard]] Result<void> put_blob(std::string_view library, const Blake3Digest& hash,
                                        const std::filesystem::path& file,
                                        const std::stop_token& stop = {}) const;

    // Polls the health report until every engine is ready. Engines that are
    // starting, or restarting after a failure, are waited for; so is a pod
    // whose runtime does not answer yet. Errc::timeout after `timeout`, with
    // the engines' last states; Errc::cancelled on a stop request.
    [[nodiscard]] Result<runtime_api::HealthReport> wait_until_ready(
        std::chrono::milliseconds timeout, const std::stop_token& stop = {},
        std::chrono::milliseconds poll_every = std::chrono::seconds{2}) const;

private:
    [[nodiscard]] net::HttpClient::Request request(net::Method method, std::string_view path) const;
    [[nodiscard]] Result<net::HttpClient::Response> send(const net::HttpClient::Request& request,
                                                         const std::stop_token& stop) const;

    const net::HttpClient* http_;
    Endpoint endpoint_;
    net::RetryPolicy retry_;
};

struct CallLimits {
    // No byte for this long: the call failed. Longer than the runtime's
    // keep-alive interval (15 s), so a slow but living call never trips it.
    std::chrono::milliseconds idle_timeout{std::chrono::seconds{60}};
    std::optional<std::chrono::milliseconds> call_timeout{};
};

// A model provider aimed at the runtime: the OpenAI-compatible relay to its
// llama-server engines, with the runtime's token. Its name, "runtime", is
// what call records and recordings file its calls under. `http` must outlive it.
[[nodiscard]] std::unique_ptr<llm::LlmProvider> runtime_provider(const net::HttpClient& http,
                                                                 const Endpoint& endpoint,
                                                                 CallLimits limits = {});

}  // namespace rr::runtime_client
