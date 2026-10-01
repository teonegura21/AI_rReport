// rr/hub/hub_config.hpp — from rreport's configuration to the hub's settings.
//
// Why this exists
//   The hub's commands share one configuration: config/default.toml,
//   compiled in, which is also its schema (rr_config). The rules that turn
//   it into settings live here, where they are unit-tested:
//     * the files: --config, else RREPORT_CONFIG (both must exist), else
//       $XDG_CONFIG_HOME/rreport/config.toml (default ~/.config/rreport/)
//       when it exists; secrets.toml beside it likewise (chmod 600);
//     * the data directory: hub.data_dir, else $XDG_DATA_HOME/rreport, else
//       ~/.local/share/rreport;
//     * the runtime: runtime.url, else RunPod's proxy URL for runpod.pod_id
//       and runpod.port, else none (the hub works without a pod, but cannot
//       call models);
//     * llm.mode: live, record (live, keeping every answer) or replay
//       (recorded answers only: tests, never a live call);
//     * the sampling presets: every [sampling.<name>] table, checked
//       (llm::preset_problems()), so a bad value fails at startup instead
//       of in the middle of a job.
//   Environment variables are RREPORT_<SECTION>__<KEY>; the runtime's are
//   RR_RUNTIME_*, so the two never read each other's.
//
// Thread-safety: pure functions.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <rr/config/config.hpp>
#include <rr/core/result.hpp>
#include <rr/core/secret.hpp>
#include <rr/llm/sampling.hpp>
#include <rr/runtime_client/runtime_client.hpp>

namespace rr::hub {

inline constexpr std::string_view kHubEnvPrefix = "RREPORT_";

// config/default.toml, as compiled into the binary.
[[nodiscard]] std::string_view hub_defaults() noexcept;

// What the configuration is resolved against: the environment's directories.
struct HubFacts {
    std::optional<std::string> home{};             // $HOME
    std::optional<std::string> xdg_config_home{};  // $XDG_CONFIG_HOME
    std::optional<std::string> xdg_data_home{};    // $XDG_DATA_HOME
    std::optional<std::string> config_env{};       // $RREPORT_CONFIG
};

struct ConfigFiles {
    std::optional<std::filesystem::path> config{};
    bool config_required = false;  // named by the user: it must exist
    std::optional<std::filesystem::path> secrets{};
};

// Which files to load (see the header comment). Default locations are
// included only when they exist.
[[nodiscard]] ConfigFiles config_files(const std::optional<std::filesystem::path>& config_flag,
                                       const std::optional<std::filesystem::path>& secrets_flag,
                                       const HubFacts& facts);

enum class LlmMode : std::uint8_t {
    live,
    record,
    replay
};

struct RunpodSettings {
    Secret api_key{};
    std::string pod_id{};
    std::string api_url{};
    int port = 8080;
};

struct HubSettings {
    std::filesystem::path data_dir{};
    std::string library = "default";
    std::optional<runtime_client::Endpoint> runtime{};  // none: no pod configured
    std::chrono::milliseconds ready_timeout{};
    std::optional<RunpodSettings> runpod{};  // none: no runpod.pod_id
    LlmMode llm_mode = LlmMode::live;
    std::filesystem::path recordings{};
    runtime_client::CallLimits call_limits{};
    int call_attempts = 4;
    // How each model samples, by preset name ("qwen_thinking", ...).
    std::map<std::string, llm::SamplingPreset, std::less<>> sampling{};

    // <data_dir>/libraries/<library>/library.db
    [[nodiscard]] std::filesystem::path library_database() const;
};

[[nodiscard]] Result<HubSettings> hub_settings_from_config(const config::Config& config,
                                                           const HubFacts& facts);

}  // namespace rr::hub
