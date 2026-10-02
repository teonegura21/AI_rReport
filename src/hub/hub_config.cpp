#include <rr/hub/hub_config.hpp>

#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

#include <rr/core/strings.hpp>
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

// [jobs]: each ceiling above zero, since a ceiling of nothing stops every job.
Result<JobLimits> job_limits(const config::Config& config) {
    JobLimits limits;
    RR_TRY_ASSIGN(limits.tokens_per_hour, config.get_int("jobs.tokens_per_video_hour"));
    RR_TRY_ASSIGN(limits.time_per_hour, config.get_duration("jobs.time_per_video_hour"));
    RR_TRY_ASSIGN(limits.least_video, config.get_duration("jobs.least_video"));
    if (limits.tokens_per_hour <= 0) {
        return fail(Errc::invalid_argument,
                    std::format("jobs.tokens_per_video_hour must be above zero, not {}: a ceiling of nothing "
                                "stops every job",
                                limits.tokens_per_hour));
    }
    if (limits.time_per_hour.count() <= 0) {
        return fail(
            Errc::invalid_argument,
            "jobs.time_per_video_hour must be longer than nothing: a ceiling of nothing stops every job");
    }
    if (limits.least_video.count() <= 0) {
        return fail(Errc::invalid_argument, "jobs.least_video must be longer than nothing");
    }
    return limits;
}

// An integer setting that must fit an int (a token count, top-k).
Result<int> int_setting(const config::Config& config, const std::string& key) {
    RR_TRY_ASSIGN(const std::int64_t value, config.get_int(key));
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return fail(Errc::invalid_argument, std::format("{} is out of range: {}", key, value));
    }
    return static_cast<int>(value);
}

// One [sampling.<name>] table. Every field is required: the defaults define
// them all, and rr_config refuses keys the defaults do not have.
Result<llm::SamplingPreset> sampling_preset(const config::Config& config, std::string_view name) {
    const auto key = [&](std::string_view field) { return std::format("sampling.{}.{}", name, field); };
    llm::SamplingPreset preset;
    RR_TRY_ASSIGN(preset.temperature, config.get_double(key("temperature")));
    RR_TRY_ASSIGN(preset.top_p, config.get_double(key("top_p")));
    RR_TRY_ASSIGN(preset.top_k, int_setting(config, key("top_k")));
    RR_TRY_ASSIGN(preset.min_p, config.get_double(key("min_p")));
    RR_TRY_ASSIGN(preset.presence_penalty, config.get_double(key("presence_penalty")));
    RR_TRY_ASSIGN(preset.repeat_penalty, config.get_double(key("repeat_penalty")));
    RR_TRY_ASSIGN(preset.thinking, config.get_bool(key("thinking")));
    RR_TRY_ASSIGN(const int budget, int_setting(config, key("thinking_budget")));
    RR_TRY_ASSIGN(std::string closing, config.get_string(key("thinking_closing")));
    if (budget >= 0) {
        preset.thinking_budget = llm::ThinkingBudget{.tokens = budget, .closing = std::move(closing)};
    } else if (budget != -1) {
        return fail(Errc::invalid_argument,
                    std::format("{} must be a number of tokens, or -1 for none, not {}",
                                key("thinking_budget"), budget));
    }
    RR_TRY_ASSIGN(const std::string effort, config.get_string(key("reasoning_effort")));
    if (!effort.empty()) {
        preset.reasoning_effort = llm::reasoning_effort_from_string(effort);
        if (!preset.reasoning_effort.has_value()) {
            return fail(Errc::invalid_argument,
                        std::format("{} must be low, medium, high, xhigh or empty, not '{}'",
                                    key("reasoning_effort"), effort));
        }
    }
    if (const std::vector<std::string> problems = llm::preset_problems(preset); !problems.empty()) {
        return fail(Errc::invalid_argument, std::format("sampling.{}: {}", name, join(problems, "; ")));
    }
    return preset;
}

// Every preset the configuration defines: the names under "sampling.".
Result<std::map<std::string, llm::SamplingPreset, std::less<>>> sampling_presets(
    const config::Config& config) {
    constexpr std::string_view kSection = "sampling.";
    std::set<std::string, std::less<>> names;
    for (const config::Config::Entry& entry : config.entries()) {
        const std::string_view key = entry.key;
        if (key.starts_with(kSection)) {
            const std::string_view rest = key.substr(kSection.size());
            names.emplace(rest.substr(0, rest.find('.')));
        }
    }
    std::map<std::string, llm::SamplingPreset, std::less<>> presets;
    for (const std::string& name : names) {
        RR_TRY_ASSIGN(llm::SamplingPreset preset, sampling_preset(config, name));
        presets.emplace(name, std::move(preset));
    }
    return presets;
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
    RR_TRY_ASSIGN(settings.ytdlp, config.get_string("media.ytdlp"));
    if (settings.ytdlp.empty()) {
        return fail(Errc::invalid_argument, "media.ytdlp is empty: name yt-dlp, or give its path");
    }
    RR_TRY_ASSIGN(settings.ytdlp_timeout, config.get_duration("media.ytdlp_timeout"));
    if (settings.ytdlp_timeout.count() <= 0) {
        return fail(Errc::invalid_argument, "media.ytdlp_timeout must be longer than nothing");
    }
    RR_TRY(apply_llm_settings(config, settings));
    RR_TRY_ASSIGN(settings.sampling, sampling_presets(config));
    RR_TRY_ASSIGN(settings.jobs, job_limits(config));
    return settings;
}

}  // namespace rr::hub
