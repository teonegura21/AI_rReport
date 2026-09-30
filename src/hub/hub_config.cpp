#include <rr/hub/hub_config.hpp>

#include <cstdint>
#include <format>
#include <system_error>
#include <utility>

#include <rr/embedded/hub_defaults.hpp>
#include <rr/runpod/runpod.hpp>
#include <rr/runtime_api/protocol.hpp>

namespace rr::hub {

namespace {

// An XDG base directory: the variable when it holds an absolute path (the
// specification says to ignore relative ones), else ~/<fallback>.
std::optional<std::filesystem::path> xdg_dir(const std::optional<std::string>& variable,
                                             const std::optional<std::string>& home,
                                             std::string_view fallback) {
    if (variable.has_value() && std::filesystem::path{*variable}.is_absolute()) {
        return std::filesystem::path{*variable};
    }
    if (home.has_value() && !home->empty()) {
        return std::filesystem::path{*home} / fallback;
    }
    return std::nullopt;
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

Result<LlmMode> llm_mode(std::string_view name) {
    if (name == "live") {
        return LlmMode::live;
    }
    if (name == "record") {
        return LlmMode::record;
    }
    if (name == "replay") {
        return LlmMode::replay;
    }
    return fail(Errc::invalid_argument,
                std::format("llm.mode must be live, record or replay, not '{}'", name));
}

// hub.data_dir, else $XDG_DATA_HOME/rreport, else ~/.local/share/rreport.
Result<std::filesystem::path> data_directory(const config::Config& config, const HubFacts& facts) {
    RR_TRY_ASSIGN(std::filesystem::path configured, config.get_path("hub.data_dir"));
    if (!configured.empty()) {
        return configured;
    }
    if (const auto base = xdg_dir(facts.xdg_data_home, facts.home, ".local/share"); base.has_value()) {
        return *base / "rreport";
    }
    return fail(Errc::invalid_argument,
                "hub.data_dir is not set, and neither is XDG_DATA_HOME nor HOME: set hub.data_dir");
}

// The pod, when runpod.pod_id names one.
Result<std::optional<RunpodSettings>> runpod_settings(const config::Config& config) {
    RR_TRY_ASSIGN(std::string pod_id, config.get_string("runpod.pod_id"));
    RR_TRY_ASSIGN(const std::int64_t port, config.get_int("runpod.port"));
    if (port < 1 || port > 65535) {
        return fail(Errc::invalid_argument, std::format("runpod.port must be a port number, not {}", port));
    }
    if (pod_id.empty()) {
        return std::optional<RunpodSettings>{};
    }
    RR_TRY(with_context(runpod::validate_pod_id(pod_id), "runpod.pod_id"));
    RunpodSettings runpod{.pod_id = std::move(pod_id), .port = static_cast<int>(port)};
    RR_TRY_ASSIGN(runpod.api_key, config.get_secret("runpod.api_key"));
    RR_TRY_ASSIGN(runpod.api_url, config.get_string("runpod.api_url"));
    return std::optional<RunpodSettings>{std::move(runpod)};
}

// runtime.url, else RunPod's proxy URL for the pod, else none.
Result<std::optional<runtime_client::Endpoint>> runtime_endpoint(
    const config::Config& config, const std::optional<RunpodSettings>& runpod) {
    RR_TRY_ASSIGN(std::string url, config.get_string("runtime.url"));
    if (url.empty() && runpod.has_value()) {
        url = runpod::proxy_url(runpod->pod_id, runpod->port);
    }
    if (url.empty()) {
        return std::optional<runtime_client::Endpoint>{};
    }
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        return fail(Errc::invalid_argument,
                    std::format("runtime.url must start with https:// (or http://), not '{}'", url));
    }
    RR_TRY_ASSIGN(Secret token, config.get_secret("runtime.token"));
    return std::optional<runtime_client::Endpoint>{
        runtime_client::Endpoint{.url = std::move(url), .token = std::move(token)}};
}

Result<void> apply_llm_settings(const config::Config& config, HubSettings& settings) {
    RR_TRY_ASSIGN(const std::string mode, config.get_string("llm.mode"));
    RR_TRY_ASSIGN(settings.llm_mode, llm_mode(mode));
    RR_TRY_ASSIGN(const std::filesystem::path recordings, config.get_path("llm.recordings"));
    settings.recordings = recordings.empty() ? settings.data_dir / "recordings" : recordings;
    RR_TRY_ASSIGN(settings.call_limits.idle_timeout, config.get_duration("llm.idle_timeout"));
    RR_TRY_ASSIGN(const std::chrono::milliseconds call_timeout, config.get_duration("llm.call_timeout"));
    settings.call_limits.call_timeout =
        call_timeout.count() == 0 ? std::nullopt : std::optional<std::chrono::milliseconds>{call_timeout};
    RR_TRY_ASSIGN(const std::int64_t attempts, config.get_int("llm.attempts"));
    if (attempts < 1 || attempts > 20) {
        return fail(Errc::invalid_argument,
                    std::format("llm.attempts must be between 1 and 20, not {}", attempts));
    }
    settings.call_attempts = static_cast<int>(attempts);
    return ok();
}

}  // namespace

std::string_view hub_defaults() noexcept {
    // The embedded file is part of the build: it is always there.
    return embedded::hub_defaults::find("config/default.toml").value_or("");
}

ConfigFiles config_files(const std::optional<std::filesystem::path>& config_flag,
                         const std::optional<std::filesystem::path>& secrets_flag, const HubFacts& facts) {
    ConfigFiles files;
    const auto directory = xdg_dir(facts.xdg_config_home, facts.home, ".config");
    if (config_flag.has_value()) {
        files.config = *config_flag;
        files.config_required = true;
    } else if (facts.config_env.has_value() && !facts.config_env->empty()) {
        files.config = std::filesystem::path{*facts.config_env};
        files.config_required = true;
    } else if (directory.has_value() && file_exists(*directory / "rreport" / "config.toml")) {
        files.config = *directory / "rreport" / "config.toml";
    }
    if (secrets_flag.has_value()) {
        files.secrets = *secrets_flag;
    } else if (directory.has_value() && file_exists(*directory / "rreport" / "secrets.toml")) {
        files.secrets = *directory / "rreport" / "secrets.toml";
    }
    return files;
}

std::filesystem::path HubSettings::library_database() const {
    return data_dir / "libraries" / library / "library.db";
}

Result<HubSettings> hub_settings_from_config(const config::Config& config, const HubFacts& facts) {
    HubSettings settings;
    RR_TRY_ASSIGN(settings.data_dir, data_directory(config, facts));
    RR_TRY_ASSIGN(settings.library, config.get_string("hub.library"));
    RR_TRY(with_context(runtime_api::validate_library_name(settings.library), "hub.library"));
    RR_TRY_ASSIGN(settings.runpod, runpod_settings(config));
    RR_TRY_ASSIGN(settings.runtime, runtime_endpoint(config, settings.runpod));
    RR_TRY_ASSIGN(settings.ready_timeout, config.get_duration("runtime.ready_timeout"));
    RR_TRY(apply_llm_settings(config, settings));
    return settings;
}

}  // namespace rr::hub
