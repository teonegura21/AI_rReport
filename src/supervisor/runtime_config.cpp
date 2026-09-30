#include <rr/supervisor/runtime_config.hpp>

#include <algorithm>
#include <array>
#include <format>

#include <rr/embedded/runtime_defaults.hpp>

namespace rr::supervisor {

namespace {

using std::chrono::milliseconds;

struct Alias {
    std::string_view alias;
    std::string_view canonical;
};

constexpr std::array<Alias, 2> kAliases{{
    {.alias = "RR_RUNTIME_TOKEN", .canonical = "RR_RUNTIME_SERVER__TOKEN"},
    {.alias = "HF_TOKEN", .canonical = "RR_RUNTIME_MODELS__HUB_TOKEN"},
}};

Result<int> port_setting(const config::Config& config, std::string_view key) {
    RR_TRY_ASSIGN(const std::int64_t port, config.get_int(key));
    if (port < 0 || port > 65535) {
        return fail(Errc::invalid_argument,
                    std::format("{} must be a port number (0–65535), not {}", key, port));
    }
    return static_cast<int>(port);
}

Result<std::int64_t> positive_setting(const config::Config& config, std::string_view key) {
    RR_TRY_ASSIGN(const std::int64_t value, config.get_int(key));
    if (value <= 0) {
        return fail(Errc::invalid_argument, std::format("{} must be positive, not {}", key, value));
    }
    return value;
}

// A duration where "0s" means "off".
Result<std::optional<milliseconds>> optional_duration(const config::Config& config, std::string_view key) {
    RR_TRY_ASSIGN(const milliseconds duration, config.get_duration(key));
    return duration.count() == 0 ? std::nullopt : std::optional<milliseconds>{duration};
}

}  // namespace

std::string_view runtime_defaults() noexcept {
    // The embedded file is part of the build: it is always there.
    return embedded::runtime_defaults::find("config/runtime.default.toml").value_or("");
}

Result<Environment> resolve_environment_aliases(Environment environment) {
    const auto find = [&](std::string_view name) {
        return std::ranges::find(environment, name, &std::pair<std::string, std::string>::first);
    };
    for (const Alias& alias : kAliases) {
        const auto aliased = find(alias.alias);
        if (aliased == environment.end()) {
            continue;
        }
        if (const auto canonical = find(alias.canonical); canonical != environment.end()) {
            if (canonical->second != aliased->second) {
                return fail(Errc::invalid_argument,
                            std::format("{} and {} are both set, to different values; "
                                        "unset one of them",
                                        alias.alias, alias.canonical));
            }
            continue;
        }
        // The value is copied before emplace_back() starts: growing the vector
        // may move the element `aliased` points to.
        environment.emplace_back(std::string{alias.canonical}, std::string{aliased->second});
    }
    return environment;
}

Result<std::string> resolve_profile(const Catalog& catalog, std::string_view configured,
                                    const std::vector<runtime_api::GpuInfo>& gpus) {
    if (configured != "auto") {
        if (catalog.find_profile(configured) == nullptr) {
            return fail(Errc::not_found, std::format("the catalog has no profile '{}'", configured));
        }
        return std::string{configured};
    }
    if (gpus.empty()) {
        return fail(
            Errc::unavailable,
            "models.profile is \"auto\", but no NVIDIA GPU was found; name a profile from the catalog");
    }
    const auto smallest = std::ranges::min_element(gpus, {}, &runtime_api::GpuInfo::memory_total_mib);
    RR_TRY_ASSIGN(const Profile* profile, choose_profile(catalog, smallest->memory_total_mib));
    return profile->name;
}

Result<SupervisorSettings> settings_from_config(const config::Config& config, Catalog catalog,
                                                const RuntimeFacts& facts) {
    SupervisorSettings settings;

    RR_TRY_ASSIGN(settings.host, config.get_string("server.host"));
    RR_TRY_ASSIGN(settings.port, port_setting(config, "server.port"));
    RR_TRY_ASSIGN(settings.token, config.get_secret("server.token"));
    if (settings.token.empty()) {
        return fail(Errc::invalid_argument,
                    "server.token is not set: the runtime needs the hub's bearer token "
                    "(RR_RUNTIME_TOKEN)");
    }
    RR_TRY_ASSIGN(const std::int64_t workers, positive_setting(config, "server.worker_threads"));
    settings.worker_threads = static_cast<std::size_t>(workers);

    RR_TRY_ASSIGN(const std::string profile, config.get_string("models.profile"));
    RR_TRY_ASSIGN(settings.profile, resolve_profile(catalog, profile, facts.gpus));
    settings.catalog = std::move(catalog);
    RR_TRY_ASSIGN(settings.models_dir, config.get_path("models.dir"));

    RR_TRY_ASSIGN(settings.llama_server, config.get_string("engines.llama_server"));
    RR_TRY_ASSIGN(settings.first_engine_port, port_setting(config, "engines.first_port"));
    RR_TRY_ASSIGN(settings.engine_startup_timeout, config.get_duration("engines.startup_timeout"));
    RR_TRY_ASSIGN(settings.engine_stop_grace, config.get_duration("engines.stop_grace"));
    RR_TRY_ASSIGN(settings.engine_restart_backoff, config.get_duration("engines.restart_backoff"));
    RR_TRY_ASSIGN(settings.proxy_idle_timeout, config.get_duration("engines.idle_timeout"));
    RR_TRY_ASSIGN(settings.stream_keep_alive, optional_duration(config, "engines.stream_keep_alive"));
    RR_TRY_ASSIGN(settings.stream_head_patience, config.get_duration("engines.stream_head_patience"));

    RR_TRY_ASSIGN(settings.blobs_dir, config.get_path("blobs.dir"));
    RR_TRY_ASSIGN(const std::int64_t max_upload, positive_setting(config, "blobs.max_upload_bytes"));
    settings.max_upload_bytes = static_cast<std::uint64_t>(max_upload);
    RR_TRY_ASSIGN(settings.blob_retention, config.get_duration("blobs.retention"));

    RR_TRY_ASSIGN(settings.idle_stop, optional_duration(config, "idle.stop_after"));
    RR_TRY_ASSIGN(settings.stop_command, config.get_string_list("idle.stop_command"));
    if (settings.stop_command.empty() && facts.runpod_pod_id.has_value() && !facts.runpod_pod_id->empty()) {
        settings.stop_command = {"runpodctl", "stop", "pod", *facts.runpod_pod_id};
    }
    RR_TRY_ASSIGN(settings.watchdog_interval, config.get_duration("idle.check_every"));
    if (settings.watchdog_interval.count() == 0) {
        return fail(Errc::invalid_argument, "idle.check_every must be longer than 0s");
    }

    settings.version = facts.version;
    return settings;
}

}  // namespace rr::supervisor
