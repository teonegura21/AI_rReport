// rreport — the hub, on your own machine. In M0 it carries the operator's tools.
//
// Commands
//   doctor            check the data directory, the library, the capture tools,
//                     RunPod and the runtime, and say what to do about problems
//   bench             measure the pod: the commander's and the infantry's speed,
//                     peak VRAM, the cold start, dollars per million tokens
//   pod status        the pod as RunPod sees it, and its runtime
//   pod start         start the pod and wait until every engine is ready
//   pod stop          stop the pod (its /workspace volume is kept)
//   config show       print the configuration (--origin: where each value came from)
//   version
//
// Exit codes
//   0 ok · 1 failure · 2 usage or configuration error · 128+N interrupted by signal N
//
// This file only parses arguments and wires modules together; the rules live
// in src/ (rr/hub/*.hpp), where they are unit-tested. Configuration:
// config/default.toml.
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <rr/config/config.hpp>
#include <rr/core/environment.hpp>
#include <rr/core/file_io.hpp>
#include <rr/core/strings.hpp>
#include <rr/core/version.hpp>
#include <rr/db/database.hpp>
#include <rr/hub/bench.hpp>
#include <rr/hub/doctor.hpp>
#include <rr/hub/hub_config.hpp>
#include <rr/library/call_log.hpp>
#include <rr/library/library.hpp>
#include <rr/llm/accounting.hpp>
#include <rr/log/log.hpp>
#include <rr/net/http_client.hpp>
#include <rr/proc/termination_signals.hpp>
#include <rr/runpod/runpod.hpp>
#include <rr/runtime_client/pod_manager.hpp>
#include <rr/runtime_client/runtime_client.hpp>

namespace {

using namespace rr;
using namespace rr::hub;

// Exit codes (the table above). Plain ints rather than an enum: the set is
// open, since 128 + N for signal N is computed.
constexpr int kOk = 0;
constexpr int kFailure = 1;
constexpr int kBadConfiguration = 2;

// Options shared by every command.
struct Global {
    std::optional<std::filesystem::path> config_file;
    std::optional<std::filesystem::path> secrets_file;
    std::vector<std::string> overrides;
};

// Prints an error for people and returns `code`.
int failed(int code, const Error& error) {
    (void)std::fprintf(stderr, "rreport: %s\n", error.describe().c_str());
    return code;
}

// A command stopped by a signal exits 128 + its number, the shell convention.
int interrupted_or(int code, const Error& error, const proc::TerminationSignals& signals) {
    if (error.code() == Errc::cancelled && signals.received() != 0) {
        (void)std::fprintf(stderr, "rreport: interrupted\n");
        return 128 + signals.received();
    }
    return failed(code, error);
}

HubFacts hub_facts() {
    return HubFacts{.home = environment_variable("HOME"),
                    .xdg_config_home = environment_variable("XDG_CONFIG_HOME"),
                    .xdg_data_home = environment_variable("XDG_DATA_HOME"),
                    .config_env = environment_variable("RREPORT_CONFIG")};
}

Result<config::Config> load_config(const Global& global) {
    const ConfigFiles files = config_files(global.config_file, global.secrets_file, hub_facts());
    return config::Config::load(config::LoadOptions{.defaults_toml = hub_defaults(),
                                                    .file = files.config,
                                                    .file_required = files.config_required,
                                                    .secrets_file = files.secrets,
                                                    .env_prefix = std::string{kHubEnvPrefix},
                                                    .overrides = global.overrides});
}

Result<void> start_logging(const config::Config& config) {
    RR_TRY_ASSIGN(const std::string level_name, config.get_string("log.level"));
    const auto level = log::level_from_string(level_name);
    if (!level.has_value()) {
        return fail(Errc::invalid_argument,
                    std::format("log.level must be trace, debug, info, warn or error, not '{}'", level_name));
    }
    RR_TRY_ASSIGN(const std::filesystem::path file, config.get_path("log.file"));
    return log::init(log::Options{.level = *level, .console = true, .color = true, .jsonl_file = file});
}

struct Hub {
    config::Config config;
    HubSettings settings;
};

Result<Hub> load(const Global& global) {
    RR_TRY_ASSIGN(config::Config config, load_config(global));
    RR_TRY(start_logging(config));
    RR_TRY_ASSIGN(HubSettings settings, hub_settings_from_config(config, hub_facts()));
    return Hub{.config = std::move(config), .settings = std::move(settings)};
}

net::HttpClient make_http() {
    return net::HttpClient{net::HttpClient::Options{.user_agent = std::format("rreport/{}", kVersion)}};
}

// The pod's controls, when RunPod is configured.
struct PodControls {
    runpod::RunpodApi api;
    runtime_client::RuntimeClient runtime;
    runtime_client::PodManager manager;

    PodControls(const net::HttpClient& http, const RunpodSettings& runpod,
                const runtime_client::Endpoint& endpoint)
        : api(http, runpod::Options{.api_key = runpod.api_key, .api_url = runpod.api_url})
        , runtime(http, endpoint)
        , manager(api, runpod.pod_id, runtime) {}
    // The manager points at its neighbours: never copied, never moved.
    PodControls(const PodControls&) = delete;
    PodControls& operator=(const PodControls&) = delete;
    PodControls(PodControls&&) = delete;
    PodControls& operator=(PodControls&&) = delete;
    ~PodControls() = default;
};

// The runtime's endpoint, once it is known to be configured with a token.
Result<const runtime_client::Endpoint*> configured_runtime(const HubSettings& settings) {
    if (!settings.runtime.has_value()) {
        return fail(Errc::invalid_argument, "no runtime is configured: set runpod.pod_id, or runtime.url");
    }
    const runtime_client::Endpoint& endpoint = *settings.runtime;
    if (endpoint.token.empty()) {
        return fail(
            Errc::invalid_argument,
            "runtime.token is not set: it must equal the pod's RR_RUNTIME_TOKEN (RREPORT_RUNTIME__TOKEN)");
    }
    return &endpoint;
}

// RunPod's settings, once they are known to name a pod and a key.
Result<const RunpodSettings*> configured_runpod(const HubSettings& settings) {
    if (!settings.runpod.has_value() || settings.runpod->api_key.empty()) {
        return fail(
            Errc::invalid_argument,
            "RunPod is not configured: set runpod.pod_id and runpod.api_key (RREPORT_RUNPOD__API_KEY)");
    }
    return &*settings.runpod;
}

// ---------------------------------------------------------------------------------------
// doctor
// ---------------------------------------------------------------------------------------
int doctor(const Global& global, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    const net::HttpClient http = make_http();
    const std::string path = environment_variable("PATH").value_or("");
    const std::vector<Finding> findings = run_doctor(DoctorInputs{
        .settings = &hub->settings,
        .http = &http,
        .hub_version = std::string{kFullVersion},
        .find_program = [&path](std::string_view program) { return find_on_path(program, path); },
        .stop = signals.stop_token()});
    for (const Finding& finding : findings) {
        (void)std::printf("[%-4s] %s: %s\n", std::string{to_string(finding.severity)}.c_str(),
                          finding.check.c_str(), finding.detail.c_str());
        if (!finding.advice.empty()) {
            (void)std::printf("       %s\n", finding.advice.c_str());
        }
    }
    return healthy(findings) ? kOk : kFailure;
}

// ---------------------------------------------------------------------------------------
// bench
// ---------------------------------------------------------------------------------------
struct BenchFlags {
    BenchOptions options;
    bool cold_start = false;
    std::optional<std::filesystem::path> out;
};

int bench(const Global& global, const BenchFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    const HubSettings& settings = hub->settings;
    const auto endpoint = configured_runtime(settings);
    if (!endpoint.has_value()) {
        return failed(kBadConfiguration, endpoint.error());
    }
    const net::HttpClient http = make_http();
    const runtime_client::RuntimeClient runtime{http, **endpoint};
    BenchOptions options = flags.options;
    std::optional<std::chrono::milliseconds> cold_start;
    const std::stop_token stop = signals.stop_token();

    if (const auto runpod = configured_runpod(settings); runpod.has_value()) {
        const RunpodSettings& pod_settings = **runpod;
        const PodControls pod{http, pod_settings, **endpoint};
        if (flags.cold_start) {
            (void)std::fprintf(stderr, "bench: stopping pod %s for a cold start\n",
                               pod_settings.pod_id.c_str());
            if (auto stopped = pod.manager.stop_pod(stop); !stopped.has_value()) {
                return interrupted_or(kFailure, stopped.error(), signals);
            }
        }
        (void)std::fprintf(stderr, "bench: waiting for pod %s and its engines\n",
                           pod_settings.pod_id.c_str());
        auto ready = pod.manager.ensure_ready(settings.ready_timeout, stop);
        if (!ready.has_value()) {
            return interrupted_or(kFailure, ready.error(), signals);
        }
        if (ready->started) {
            cold_start = ready->waited;
        }
        if (!options.price_per_hour.has_value() && ready->pod.cost_per_hour > 0) {
            options.price_per_hour = ready->pod.cost_per_hour;
        }
    } else {
        if (flags.cold_start) {
            return failed(kBadConfiguration, Error{Errc::invalid_argument,
                                                   "--cold-start needs runpod.pod_id and runpod.api_key"});
        }
        if (auto ready = runtime.wait_until_ready(settings.ready_timeout, stop); !ready.has_value()) {
            return interrupted_or(kFailure, ready.error(), signals);
        }
    }

    // Every call is recorded in the library (purpose "bench"), and is not
    // retried: a failure is a result.
    auto database = library::open_database(settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    library::CallLog calls{*database};
    const auto model = runtime_client::runtime_provider(http, **endpoint, settings.call_limits);
    const llm::AccountedProvider accounted{*model, calls, SystemClock::instance()};
    auto result = run_bench(accounted, runtime, options, stop, [](std::string_view line) {
        (void)std::fprintf(stderr, "bench: %.*s\n", static_cast<int>(line.size()), line.data());
    });
    if (!result.has_value()) {
        return interrupted_or(kFailure, result.error(), signals);
    }
    result->cold_start = cold_start;

    (void)std::fputs(to_markdown(*result).c_str(), stdout);
    const std::filesystem::path out = flags.out.value_or(
        settings.data_dir / "bench" / std::format("{}.json", replace_all(result->started_at, ":", "")));
    std::error_code error;
    std::filesystem::create_directories(out.parent_path(), error);
    if (auto written = write_file_atomically(out, json::dump_pretty(to_json(*result)) + "\n");
        !written.has_value()) {
        return failed(kFailure, written.error());
    }
    (void)std::printf("\nSaved to %s. The pod stops itself once idle; to stop it now: rreport pod stop\n",
                      out.string().c_str());
    return kOk;
}

// ---------------------------------------------------------------------------------------
// pod
// ---------------------------------------------------------------------------------------
std::string describe(const runpod::Pod& pod) {
    std::string text = std::format("pod {} is {}", pod.id, pod.status_name);
    if (!pod.gpu.empty()) {
        text += std::format(": {} × {}", pod.gpu_count, pod.gpu);
    }
    if (pod.cost_per_hour > 0) {
        text += std::format(", ${:.2f}/h", pod.cost_per_hour);
    }
    if (pod.uptime_s.has_value()) {
        text += std::format(", up {} min", *pod.uptime_s / 60);
    }
    return text;
}

int pod_command(const Global& global, std::string_view action, bool wait,
                const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    const HubSettings& settings = hub->settings;
    const auto runpod = configured_runpod(settings);
    if (!runpod.has_value()) {
        return failed(kBadConfiguration, runpod.error());
    }
    const RunpodSettings& pod_settings = **runpod;
    // Only `pod start` needs the runtime (it waits for the engines); the
    // others use it when it is there.
    const auto endpoint = configured_runtime(settings);
    const net::HttpClient http = make_http();
    const PodControls pod{
        http, pod_settings,
        endpoint.has_value()
            ? **endpoint
            : runtime_client::Endpoint{.url = runpod::proxy_url(pod_settings.pod_id, pod_settings.port)}};
    const std::stop_token stop = signals.stop_token();

    if (action == "status") {
        auto status = pod.manager.status(stop);
        if (!status.has_value()) {
            return interrupted_or(kFailure, status.error(), signals);
        }
        (void)std::printf("%s\n", describe(*status).c_str());
        if (status->status == runpod::PodStatus::running && endpoint.has_value()) {
            auto health = pod.runtime.health(stop);
            if (health.has_value()) {
                (void)std::printf("  profile %s, %s\n", health->profile.c_str(),
                                  describe_gpus(health->gpus).c_str());
                for (const runtime_api::EngineInfo& engine : health->engines) {
                    (void)std::printf("  %s (%s): %s\n", engine.model.c_str(), engine.role.c_str(),
                                      std::string{to_string(engine.state)}.c_str());
                }
            } else {
                (void)std::printf("  runtime: %s\n", health.error().message().c_str());
            }
        }
        return kOk;
    }
    if (action == "stop") {
        auto stopped = pod.manager.stop_pod(stop);
        if (!stopped.has_value()) {
            return interrupted_or(kFailure, stopped.error(), signals);
        }
        (void)std::printf("%s\n", describe(*stopped).c_str());
        return kOk;
    }
    // start
    if (!wait) {
        auto started = pod.api.start_pod(pod_settings.pod_id, stop);
        if (!started.has_value()) {
            return interrupted_or(kFailure, started.error(), signals);
        }
        (void)std::printf("%s\n", describe(*started).c_str());
        return kOk;
    }
    if (!endpoint.has_value()) {
        return failed(kBadConfiguration, endpoint.error());
    }
    auto ready = pod.manager.ensure_ready(settings.ready_timeout, stop);
    if (!ready.has_value()) {
        return interrupted_or(kFailure, ready.error(), signals);
    }
    (void)std::printf(
        "%s; every engine ready after %lld s\n", describe(ready->pod).c_str(),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(ready->waited).count()));
    return kOk;
}

int show_config(const Global& global, bool origin) {
    const auto config = load_config(global);
    if (!config.has_value()) {
        return failed(kBadConfiguration, config.error());
    }
    (void)std::fputs(config->show(origin).c_str(), stdout);
    return kOk;
}

// Reports an exception that reached main(): a bug, or the system running out
// of something (memory, threads). Until log::init() the log writes to stderr;
// after it, to the configured log file as well, flushed at once for an error.
int died(const char* what) noexcept {
    try {
        log::error("unexpected error, stopping: {}", what);
    } catch (...) {
        (void)std::fprintf(stderr, "rreport: unexpected error, stopping: %s\n", what);
    }
    return kFailure;
}

int run(int argc, char** argv) {
    // First, before any thread exists: every thread inherits the blocked mask.
    const proc::TerminationSignals signals;
    // A write to a closed pipe (output into `head`) is an error to handle, not
    // a reason to die.
    (void)std::signal(SIGPIPE, SIG_IGN);

    CLI::App app{"rreport: understand any video completely (M0: the operator's tools)"};
    app.require_subcommand(1);
    // Global options may also follow a command; set before the commands are
    // made, which inherit it then.
    app.fallthrough();
    Global global;
    app.add_option("--config", global.config_file,
                   "Configuration file (default: $RREPORT_CONFIG, else ~/.config/rreport/config.toml)");
    app.add_option("--secrets", global.secrets_file,
                   "Secrets file (default: ~/.config/rreport/secrets.toml; chmod 600)");
    app.add_option("--set", global.overrides, "Override one setting: section.key=value (repeatable)");

    auto* doctor_command = app.add_subcommand("doctor", "Check what the hub needs, and say what to do");

    BenchFlags bench_flags;
    auto* bench_command = app.add_subcommand("bench", "Measure the pod: speed, VRAM, cold start, cost");
    bench_command->add_option("--commander", bench_flags.options.commander,
                              "Commander model (default: the runtime's)");
    bench_command->add_option("--infantry", bench_flags.options.infantry,
                              "Infantry model (default: the runtime's)");
    bench_command
        ->add_option("--parallel", bench_flags.options.parallel, "Infantry requests at once, e.g. 1,4,8")
        ->delimiter(',')
        ->check(CLI::PositiveNumber);
    bench_command->add_option("--prompt-tokens", bench_flags.options.prompt_tokens, "Prompt size, about")
        ->check(CLI::Range(32, 200'000));
    bench_command->add_option("--output-tokens", bench_flags.options.output_tokens, "Tokens per answer")
        ->check(CLI::Range(1, 100'000));
    bench_command->add_option("--rounds", bench_flags.options.rounds, "Commander requests")
        ->check(CLI::Range(1, 100));
    bench_command
        ->add_option("--n", bench_flags.options.choices,
                     "Answers per commander request, as k drafts are asked for (at most its slots)")
        ->check(CLI::Range(1, 64));
    bench_command->add_option("--price-per-hour", bench_flags.options.price_per_hour,
                              "The pod's price in USD (default: RunPod's)");
    bench_command->add_flag("--cold-start", bench_flags.cold_start, "Stop the pod first, and time its start");
    bench_command->add_option("--out", bench_flags.out, "Where to save the results (JSON)");

    auto* pod_command_group =
        app.add_subcommand("pod", "The GPU pod: status, start, stop")->require_subcommand(1);
    auto* pod_status =
        pod_command_group->add_subcommand("status", "The pod as RunPod sees it, and its runtime");
    bool no_wait = false;
    auto* pod_start =
        pod_command_group->add_subcommand("start", "Start the pod and wait until its engines are ready");
    pod_start->add_flag("--no-wait", no_wait, "Only ask RunPod to start it");
    auto* pod_stop =
        pod_command_group->add_subcommand("stop", "Stop the pod (its /workspace volume is kept)");

    auto* config_command = app.add_subcommand("config", "Configuration")->require_subcommand(1);
    bool origin = false;
    auto* show_command = config_command->add_subcommand("show", "Print every setting (secrets masked)");
    show_command->add_flag("--origin", origin, "Show where each value came from");
    auto* version_command = app.add_subcommand("version", "Print the version");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const int code = app.exit(error);  // prints the help or the error
        return code == 0 ? kOk : kBadConfiguration;
    }

    if (*doctor_command) {
        return doctor(global, signals);
    }
    if (*bench_command) {
        return bench(global, bench_flags, signals);
    }
    if (*pod_status) {
        return pod_command(global, "status", false, signals);
    }
    if (*pod_start) {
        return pod_command(global, "start", !no_wait, signals);
    }
    if (*pod_stop) {
        return pod_command(global, "stop", false, signals);
    }
    if (*show_command) {
        return show_config(global, origin);
    }
    if (*version_command) {
        (void)std::printf("rreport %s\n", std::string{kFullVersion}.c_str());
        return kOk;
    }
    return kBadConfiguration;  // unreachable: a command is required
}

}  // namespace

// Every failure the program expects travels as a Result; this catches the
// rest. An exception escaping main() calls std::terminate(), and whether the
// stack unwinds first is up to the implementation. Caught here, it always
// does, so every destructor (open transfers, the log) runs.
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        return died(error.what());
    } catch (...) {
        return died("an exception of unknown type");
    }
}
