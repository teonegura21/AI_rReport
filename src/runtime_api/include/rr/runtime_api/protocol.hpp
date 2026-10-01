// rr/runtime_api/protocol.hpp — the hub ↔ runtime wire protocol, version 2.
//
// Why this exists
//   The hub (on the laptop) and the runtime (on the GPU pod) are separate
//   programs, released together but deployed apart (ADR-0001). Everything
//   they must agree on lives here, in one small module both link:
//     * the endpoint paths;
//     * the health report, and its JSON form (strictly parsed: a field of
//       the wrong type is an error, not a silent default);
//     * validation of path parameters that become filesystem paths: a
//       library name must never be able to escape the blob root.
//   Errors travel as rr::net structured error bodies (rr/net/http.hpp).
//
//   The version changes whenever either side could misread the other; the
//   hub refuses a runtime of another version, and says to update both.
//     v1  M0.
//     v2  Each engine reports its context: the pool, the most one sequence
//         may hold, whether its slots share the pool, and the checksum of
//         the model file it serves (what a job's memo is keyed by).
//
//   Endpoints (all but /ping require "Authorization: Bearer <token>"):
//     GET  /ping                              liveness, no details, no auth
//     GET  /v1/health                         HealthReport
//     POST /v1/heartbeat                      "the hub is alive" (dead-man switch)
//     HEAD /v1/blobs/{library}/{blake3}       200 if present, 404 if not
//     PUT  /v1/blobs/{library}/{blake3}       upload; the hash is verified
//     GET  /v1/models                         OpenAI-compatible model list
//     POST /v1/chat/completions               proxied to the engine serving `model`
//     POST /v1/embeddings, /v1/rerank         likewise
//
//   Streamed calls ("stream": true) answer text/event-stream (ADR-0014):
//     * the runtime may answer 200 before the engine does, and fills silences
//       with comment lines (": keep-alive"), which SSE parsers ignore;
//     * an error after the 200 arrives as an event, data: {"error": {...}}
//       (net::error_from_event() decodes it into the exact Errc);
//     * a call succeeded only if its "[DONE]" event arrived. A transfer cut
//       short (Errc::network) or ended without "[DONE]" is to be retried.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>

namespace rr::runtime_api {

inline constexpr std::string_view kProtocolVersion = "2";

inline constexpr std::string_view kPathPing = "/ping";
inline constexpr std::string_view kPathHealth = "/v1/health";
inline constexpr std::string_view kPathHeartbeat = "/v1/heartbeat";
inline constexpr std::string_view kPathBlobPattern = "/v1/blobs/:library/:hash";
inline constexpr std::string_view kPathModels = "/v1/models";
inline constexpr std::string_view kPathChatCompletions = "/v1/chat/completions";
inline constexpr std::string_view kPathEmbeddings = "/v1/embeddings";
inline constexpr std::string_view kPathRerank = "/v1/rerank";

// "/v1/blobs/<library>/<hash>".
[[nodiscard]] std::string blob_path(std::string_view library, std::string_view hash_hex);

// A library name: 1–64 characters of [a-z0-9_-], not starting with '-'.
// It names a directory on the pod, so anything else (dots, slashes, …) is
// refused rather than sanitized.
[[nodiscard]] Result<void> validate_library_name(std::string_view name);

struct GpuInfo {
    int index = 0;
    std::string name{};
    std::int64_t memory_total_mib = 0;
    std::int64_t memory_used_mib = 0;
    int utilization_percent = 0;
    std::string driver_version{};

    friend bool operator==(const GpuInfo&, const GpuInfo&) = default;
};

enum class EngineState : std::uint8_t {
    starting,
    ready,
    failed,
    stopped
};

[[nodiscard]] std::string_view to_string(EngineState state) noexcept;
[[nodiscard]] std::optional<EngineState> engine_state_from_string(std::string_view text) noexcept;

struct EngineInfo {
    std::string model{};  // the id clients send as "model"
    std::string role{};   // commander, infantry, embedder, reranker
    EngineState state = EngineState::stopped;
    int slots = 0;  // parallel sequences (llama-server -np)

    // Tokens of attention cache, all slots together (llama-server --ctx-size).
    int context = 0;
    // The most tokens one sequence (a prompt and its answer) may hold: the
    // catalog's arithmetic until the engine is first ready, then the engine's
    // own figure (llama-server's /props, after its caps), read again at each
    // restart.
    int ctx_per_slot = 0;
    // Whether the slots share one pool of `context` tokens. Then one sequence
    // may grow to all of it, a request's n choices keep its prompt once, and
    // the sequences at work together must fit the pool: llama-server fails
    // every one of them when they do not. Otherwise each slot has its own
    // share, and only a sequence that outgrows it fails.
    bool kv_unified = false;
    // SHA-256 of the model file served, as verified before it was loaded:
    // what a request's model id means, now.
    std::string model_sha256{};

    int restarts = 0;
    std::optional<std::string> last_error{};

    friend bool operator==(const EngineInfo&, const EngineInfo&) = default;
};

struct HealthReport {
    std::string protocol{kProtocolVersion};
    std::string version{};  // rReport version of the runtime
    std::string profile{};  // the GPU profile in use
    std::vector<GpuInfo> gpus{};
    std::vector<EngineInfo> engines{};
    std::int64_t uptime_s = 0;
    std::int64_t idle_s = 0;                   // since the last request or heartbeat
    std::optional<std::int64_t> stops_in_s{};  // when the pod will stop itself, if enabled

    friend bool operator==(const HealthReport&, const HealthReport&) = default;
};

[[nodiscard]] json::Json to_json(const HealthReport& report);
[[nodiscard]] Result<HealthReport> health_report_from_json(const json::Json& document);

}  // namespace rr::runtime_api
