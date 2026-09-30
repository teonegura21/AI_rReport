// rreport-runtime — the GPU pod's one door: model engines, the model relay,
// blobs, and stopping the pod when nobody needs it (ADR-0001, ADR-0003).
//
// Commands
//   serve              verify the profile's model files, start the engines and
//                      the server; stop cleanly on SIGTERM, SIGINT or SIGHUP
//   models verify      check the profile's model files (--full: ignore the cache)
//   models sync        download what is missing or wrong, verifying everything
//   gpu                list the GPUs nvidia-smi sees
//   config show        print the configuration (--origin: where each value came from)
//   version
//
// Exit codes
//   0 ok · 1 failure · 2 usage or configuration error · 3 model files missing
//   or wrong · 128+N interrupted by signal N
//
// This file only parses arguments and wires modules together; the rules live
// in src/ (rr/supervisor/runtime_config.hpp and its neighbours), where they
// are unit-tested. Configuration: config/runtime.default.toml.
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <rr/config/config.hpp>
#include <rr/core/assert.hpp>
#include <rr/core/environment.hpp>
#include <rr/core/version.hpp>
#include <rr/log/log.hpp>
#include <rr/net/http_client.hpp>
#include <rr/proc/process.hpp>
#include <rr/proc/termination_signals.hpp>
#include <rr/supervisor/catalog.hpp>
#include <rr/supervisor/gpu.hpp>
#include <rr/supervisor/models.hpp>
#include <rr/supervisor/runtime_config.hpp>
#include <rr/supervisor/supervisor.hpp>

namespace {

using namespace rr;
using namespace rr::supervisor;

// Exit codes (the table above). Plain ints rather than an enum: the set is
// open, since 128 + N for signal N is computed.
constexpr int kOk = 0;
constexpr int kFailure = 1;
constexpr int kBadConfiguration = 2;
constexpr int kModelsNotVerified = 3;

// Options shared by every command.
struct Global {
    std::optional<std::filesystem::path> config_file;
    std::optional<std::filesystem::path> secrets_file;
    std::vector<std::string> overrides;
};

std::string version_string() {
    return std::format("{} ({})", kVersion, kGitRevision);
}

// Prints an error for people (and the log, if it is up) and returns `code`.
int failed(int code, const Error& error) {
    (void)std::fprintf(stderr, "rreport-runtime: %s\n", error.describe().c_str());
    return code;
}

// A command stopped by a signal exits 128 + its number, the shell convention.
int interrupted_or(int code, const Error& error, const proc::TerminationSignals& signals) {
    if (error.code() == Errc::cancelled && signals.received() != 0) {
        (void)std::fprintf(stderr, "rreport-runtime: interrupted\n");
        return 128 + signals.received();
    }
    return failed(code, error);
}

Result<config::Config> load_config(const Global& global) {
    RR_TRY_ASSIGN(Environment environment, resolve_environment_aliases(environment_snapshot()));
    std::optional<std::filesystem::path> file = global.config_file;
    if (!file.has_value()) {
        if (const auto named = environment_variable("RR_RUNTIME_CONFIG");
            named.has_value() && !named->empty()) {
            file = *named;
        }
    }
    return config::Config::load(config::LoadOptions{.defaults_toml = runtime_defaults(),
                                                    .file = file,
                                                    .file_required = file.has_value(),  // named: must exist
                                                    .secrets_file = global.secrets_file,
                                                    .env_prefix = std::string{kRuntimeEnvPrefix},
                                                    .environment = std::move(environment),
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

// What every command that touches models or the server needs.
struct Setup {
    config::Config config;
    Catalog catalog;
    RuntimeFacts facts;
    std::string profile;
};

Result<Setup> setup(const Global& global) {
    RR_TRY_ASSIGN(config::Config config, load_config(global));
    RR_TRY(start_logging(config));
    RR_TRY_ASSIGN(const std::filesystem::path catalog_path, config.get_path("models.catalog"));
    RR_TRY_ASSIGN(Catalog catalog, load_catalog(catalog_path));
    RuntimeFacts facts{.runpod_pod_id = environment_variable("RUNPOD_POD_ID"), .version = version_string()};
    RR_TRY_ASSIGN(const std::string configured, config.get_string("models.profile"));
    if (configured == "auto") {
        auto gpus = query_gpus();
        if (!gpus.has_value()) {
            log::warn("cannot list the GPUs: {}", gpus.error().describe());
        } else {
            facts.gpus = std::move(*gpus);
        }
        if (facts.gpus.size() > 1) {
            log::warn(
                "{} GPUs found: profiles lay models out on one GPU, so the one sized for the smallest runs",
                facts.gpus.size());
        }
    }
    RR_TRY_ASSIGN(std::string profile, resolve_profile(catalog, configured, facts.gpus));
    return Setup{.config = std::move(config),
                 .catalog = std::move(catalog),
                 .facts = std::move(facts),
                 .profile = std::move(profile)};
}

// Prints one line per file, and whether all are verified.
bool report(const std::vector<FileCheck>& checks) {
    bool all_verified = true;
    for (const FileCheck& check : checks) {
        all_verified = all_verified && check.state == FileState::verified;
        (void)std::printf("%-9s %-24s %s%s\n", std::string{to_string(check.state)}.c_str(),
                          check.needed.model.c_str(), check.needed.file.file.string().c_str(),
                          check.from_cache ? "  (record)" : "");
    }
    return all_verified;
}

// Progress for people: one line per file per 10%.
ProgressSink progress_printer() {
    return [last_file = static_cast<const ModelFile*>(nullptr),
            last_tenth = -1](const Progress& progress) mutable {
        if (!progress.total.has_value() || *progress.total == 0) {
            return;
        }
        const auto tenth = static_cast<int>(progress.done * 10 / *progress.total);
        if (progress.file != last_file || tenth != last_tenth) {
            last_file = progress.file;
            last_tenth = tenth;
            (void)std::printf("  %s: %d%%\n", progress.file->file.string().c_str(), tenth * 10);
            (void)std::fflush(stdout);
        }
    };
}

int serve(const Global& global, const proc::TerminationSignals& signals) {
    auto prepared = setup(global);
    if (!prepared.has_value()) {
        return failed(kBadConfiguration, prepared.error());
    }
    Setup& ready = *prepared;
    const Profile* profile = ready.catalog.find_profile(ready.profile);
    RR_ASSERT(profile != nullptr, "resolve_profile() returns only profiles of the catalog");

    auto verify_on_start = ready.config.get_bool("models.verify_on_start");
    if (!verify_on_start.has_value()) {
        return failed(kBadConfiguration, verify_on_start.error());
    }
    if (*verify_on_start) {
        auto models_dir = ready.config.get_path("models.dir");
        if (!models_dir.has_value()) {
            return failed(kBadConfiguration, models_dir.error());
        }
        auto checks =
            verify_model_files(ready.catalog, *profile, *models_dir, {.stop = signals.stop_token()});
        if (!checks.has_value()) {
            return interrupted_or(kFailure, checks.error(), signals);
        }
        if (!report(*checks)) {
            (void)std::fprintf(stderr,
                               "rreport-runtime: model files are missing or wrong; "
                               "run `rreport-runtime models sync`\n");
            return kModelsNotVerified;
        }
    }

    auto settings = settings_from_config(ready.config, std::move(ready.catalog), ready.facts);
    if (!settings.has_value()) {
        return failed(kBadConfiguration, settings.error());
    }
    settings->on_engine_output = [](const std::string& model, std::string_view line) {
        const log::ScopedField field{"model", model};
        log::info("{}", line);
    };
    const std::string host = settings->host;
    auto supervisor = Supervisor::create(std::move(*settings));
    if (!supervisor.has_value()) {
        return failed(kBadConfiguration, supervisor.error());
    }
    if (auto started = (*supervisor)->start(); !started.has_value()) {
        return failed(kFailure, started.error());
    }
    log::info("rreport-runtime {} serving profile '{}' on {}:{}", version_string(), ready.profile, host,
              (*supervisor)->port());
    // The one line scripts (and the component test) wait for.
    (void)std::printf("listening on %s:%d\n", host.c_str(), (*supervisor)->port());
    (void)std::fflush(stdout);

    signals.wait();
    log::info("{} received: stopping", proc::signal_name(signals.received()));
    (*supervisor)->stop();
    log::info("stopped");
    log::flush();
    return kOk;
}

int models(const Global& global, bool sync, bool full, const proc::TerminationSignals& signals) {
    auto prepared = setup(global);
    if (!prepared.has_value()) {
        return failed(kBadConfiguration, prepared.error());
    }
    const Setup& ready = *prepared;
    const Profile* profile = ready.catalog.find_profile(ready.profile);
    RR_ASSERT(profile != nullptr, "resolve_profile() returns only profiles of the catalog");
    auto models_dir = ready.config.get_path("models.dir");
    if (!models_dir.has_value()) {
        return failed(kBadConfiguration, models_dir.error());
    }
    (void)std::printf("profile '%s', models in %s\n", ready.profile.c_str(), models_dir->string().c_str());

    Result<std::vector<FileCheck>> checks = fail(Errc::internal, "not run");
    if (!sync) {
        checks = verify_model_files(
            ready.catalog, *profile, *models_dir,
            {.full = full, .on_progress = progress_printer(), .stop = signals.stop_token()});
    } else {
        auto token = ready.config.get_secret("models.hub_token");
        if (!token.has_value()) {
            return failed(kBadConfiguration, token.error());
        }
        const net::HttpClient client{
            net::HttpClient::Options{.user_agent = "rreport-runtime/" + std::string{kVersion}}};
        checks =
            sync_model_files(client, ready.catalog, *profile, *models_dir,
                             SyncOptions{.hub_token = token->empty() ? std::nullopt : std::optional{*token},
                                         .on_progress = progress_printer(),
                                         .stop = signals.stop_token()});
    }
    if (!checks.has_value()) {
        return interrupted_or(kFailure, checks.error(), signals);
    }
    return report(*checks) ? kOk : kModelsNotVerified;
}

int gpu() {
    const auto gpus = query_gpus();
    if (!gpus.has_value()) {
        return failed(kFailure, gpus.error());
    }
    if (gpus->empty()) {
        (void)std::printf("no NVIDIA GPU found\n");
        return kFailure;
    }
    for (const auto& gpu : *gpus) {
        (void)std::printf("%d  %s  %lld MiB (%lld used)  %d%% busy  driver %s\n", gpu.index, gpu.name.c_str(),
                          static_cast<long long>(gpu.memory_total_mib),
                          static_cast<long long>(gpu.memory_used_mib), gpu.utilization_percent,
                          gpu.driver_version.c_str());
    }
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
        (void)std::fprintf(stderr, "rreport-runtime: unexpected error, stopping: %s\n", what);
    }
    return kFailure;
}

int run(int argc, char** argv) {
    // First, before any thread exists: every thread inherits the blocked mask.
    const proc::TerminationSignals signals;
    // A write to a closed pipe (stdout into `head`, a hub that left) is an
    // error to handle, not a reason to die.
    (void)std::signal(SIGPIPE, SIG_IGN);

    CLI::App app{
        "rreport-runtime: the GPU pod's supervisor (model engines, the model relay, blobs, idle stop)"};
    app.require_subcommand(1);
    // Global options may also follow a command (`rreport-runtime serve --set
    // server.port=9000`): an option a command does not know falls through to
    // its parent. Set before the commands are made: they inherit it then.
    app.fallthrough();
    Global global;
    app.add_option("--config", global.config_file, "Configuration file (default: $RR_RUNTIME_CONFIG)");
    app.add_option("--secrets", global.secrets_file, "Secrets file (must be readable by its owner only)");
    app.add_option("--set", global.overrides, "Override one setting: section.key=value (repeatable)");

    auto* serve_command = app.add_subcommand("serve", "Verify the model files, then serve until stopped");
    auto* models_command = app.add_subcommand("models", "Model files: verify or sync")->require_subcommand(1);
    bool full = false;
    auto* verify_command = models_command->add_subcommand("verify", "Check the profile's model files");
    verify_command->add_flag("--full", full, "Hash every file, ignoring the verification records");
    auto* sync_command = models_command->add_subcommand("sync", "Download missing or wrong model files");
    auto* gpu_command = app.add_subcommand("gpu", "List the GPUs nvidia-smi sees");
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

    if (*serve_command) {
        return serve(global, signals);
    }
    if (*verify_command) {
        return models(global, /*sync=*/false, full, signals);
    }
    if (*sync_command) {
        return models(global, /*sync=*/true, full, signals);
    }
    if (*gpu_command) {
        return gpu();
    }
    if (*show_command) {
        return show_config(global, origin);
    }
    if (*version_command) {
        (void)std::printf("rreport-runtime %s\n", version_string().c_str());
        return kOk;
    }
    return kBadConfiguration;  // unreachable: a command is required
}

}  // namespace

// Every failure the program expects travels as a Result; this catches the
// rest. An exception escaping main() calls std::terminate(), and whether the
// stack unwinds first is up to the implementation, so the supervisor's
// destructor, which takes the engines down, might never run. Caught here, it
// always does.
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        return died(error.what());
    } catch (...) {
        return died("an exception of unknown type");
    }
}
