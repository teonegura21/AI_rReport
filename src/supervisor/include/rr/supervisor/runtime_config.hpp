// rr/supervisor/runtime_config.hpp — from rreport-runtime's configuration to
// the settings of a running supervisor.
//
// Why this exists
//   The executable only parses arguments and wires modules together; the
//   rules that turn configuration into SupervisorSettings live here, where
//   they are unit-tested.
//
//   * The built-in defaults (config/runtime.default.toml, compiled in) are
//     the schema: every settable key, with its type (rr_config).
//   * Environment variables use the RR_RUNTIME_ prefix. The hub reads
//     RREPORT_* and refuses keys it does not know, so the two programs must
//     not share a prefix when they run on one machine. Two aliases are
//     accepted: RR_RUNTIME_TOKEN for server.token (the name the RunPod
//     template sets) and HF_TOKEN for models.hub_token (the name every
//     Hugging Face tool reads). An alias that disagrees with the canonical
//     variable is an error, never a silent choice.
//   * profile = "auto" picks the catalog profile for the smallest GPU
//     present: a profile describes one GPU's layout, so a pod with unequal
//     GPUs runs what the weakest one holds.
//   * On RunPod (RUNPOD_POD_ID set) with no stop command configured, idle
//     stop runs `runpodctl stop pod <id>`: RunPod's documented form, still
//     accepted by the current CLI (whose own form is `pod stop`) and the one
//     older runpodctl builds shipped inside pods understand.
//   * A duration of "0s" turns idle stop and stream keep-alives off.
//
// Thread-safety: pure functions.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/config/config.hpp>
#include <rr/core/result.hpp>
#include <rr/runtime_api/protocol.hpp>
#include <rr/supervisor/catalog.hpp>
#include <rr/supervisor/supervisor.hpp>

namespace rr::supervisor {

inline constexpr std::string_view kRuntimeEnvPrefix = "RR_RUNTIME_";

// config/runtime.default.toml, as compiled into the binary.
[[nodiscard]] std::string_view runtime_defaults() noexcept;

using Environment = std::vector<std::pair<std::string, std::string>>;

// The environment as the configuration loader should read it: each alias
// copied to its canonical name. Errc::invalid_argument when an alias and its
// canonical variable are both set, to different values.
[[nodiscard]] Result<Environment> resolve_environment_aliases(Environment environment);

// The profile to run: `configured` itself, or for "auto" the most demanding
// profile the smallest GPU holds. Errc::unavailable when "auto" finds no GPU.
[[nodiscard]] Result<std::string> resolve_profile(const Catalog& catalog, std::string_view configured,
                                                  const std::vector<runtime_api::GpuInfo>& gpus);

// What the configuration is applied to.
struct RuntimeFacts {
    std::vector<runtime_api::GpuInfo> gpus{};
    std::optional<std::string> runpod_pod_id{};  // RUNPOD_POD_ID
    std::string version = "dev";
};

// Everything Supervisor::create() needs. The catalog is loaded by the caller
// (commands that only verify or sync models need it too).
[[nodiscard]] Result<SupervisorSettings> settings_from_config(const config::Config& config, Catalog catalog,
                                                              const RuntimeFacts& facts);

}  // namespace rr::supervisor
