// rreport — the hub, on your own machine.
//
// Commands
//   ingest SOURCE     take a video (its web address) or a caption file through
//                     the pipeline's stages, as a new job: so far its source,
//                     then its transcript (plan P15)
//   resume JOB        take a job up again where it stopped
//   jobs              the library's jobs, newest first
//   jobs show JOB     a job's stages: done, found done or failed, and what
//                     they could not do
//   eval fetch [ID…]  read each video of the eval set once, through the source
//                     stage, and keep it with the eval: eval/data/<id>/ (P16)
//   eval annotate [ID…]
//                     draft each video's gold annotation with the pod's models,
//                     as silver in eval/gold/<id>.toml, for a person to correct
//                     (runbook R2)
//   eval sheet ID     the sheet a person corrects a gold file with (Markdown)
//   doctor            check the data directory, the library, the capture tools,
//                     RunPod and the runtime, and say what to do about problems
//   bench             measure the pod: its engines, the commander's and the
//                     infantry's speed (alone and several requests at once), the
//                     prompt cache, peak VRAM, the cold start, dollars per million
//                     tokens
//   pod status        the pod as RunPod sees it, and its runtime
//   pod start         start the pod and wait until every engine is ready
//   pod stop          stop the pod (its /workspace volume is kept)
//   config show       print the configuration (--origin: where each value came from)
//   version
//
// Exit codes
//   0 ok · 1 failure (a job not done: failed, stopped, or needing you) · 2 usage
//   or configuration error · 128+N interrupted by signal N
//
// This file only parses arguments and wires modules together; the rules live
// in src/ (rr/hub/*.hpp), where they are unit-tested. Configuration:
// config/default.toml.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include <rr/config/config.hpp>
#include <rr/core/environment.hpp>
#include <rr/core/file_io.hpp>
#include <rr/core/strings.hpp>
#include <rr/core/version.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/annotate.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/set.hpp>
#include <rr/eval/sheet.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/hub/bench.hpp>
#include <rr/hub/doctor.hpp>
#include <rr/hub/hub_config.hpp>
#include <rr/hub/job_army.hpp>
#include <rr/hub/model_stack.hpp>
#include <rr/library/call_log.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/log/log.hpp>
#include <rr/media/ytdlp.hpp>
#include <rr/net/http_client.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/executor.hpp>
#include <rr/pipeline/job_run.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/plan_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/write_stage.hpp>
#include <rr/proc/termination_signals.hpp>
#include <rr/prompt/library.hpp>
#include <rr/reason/tier.hpp>
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
    if (settings.llm_mode != LlmMode::live) {
        return failed(
            kBadConfiguration,
            Error{Errc::invalid_argument, "bench measures the pod's models: run it with llm.mode = live"});
    }
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
    auto models = ModelStack::build(settings, http, calls, SystemClock::instance(), {.retries = false});
    if (!models.has_value()) {
        return failed(kBadConfiguration, models.error());
    }
    auto result = run_bench(models->provider(), runtime, options, stop, [](std::string_view line) {
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
                    (void)std::printf("  %s (%s): %s, %d slot(s), up to %d tokens each%s\n",
                                      engine.model.c_str(), engine.role.c_str(),
                                      std::string{to_string(engine.state)}.c_str(), engine.slots,
                                      engine.ctx_per_slot, engine.kv_unified ? " from a shared pool" : "");
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

// ---------------------------------------------------------------------------------------
// ingest, resume, jobs
// ---------------------------------------------------------------------------------------
void say(std::string_view line) {
    (void)std::fprintf(stderr, "  %.*s\n", static_cast<int>(line.size()), line.data());
}

// "2026-10-01 20:31 UTC"
std::string when(std::chrono::system_clock::time_point at) {
    return std::format("{:%Y-%m-%d %H:%M} UTC", std::chrono::floor<std::chrono::minutes>(at));
}

// "4.2 s", "3 min 12 s", "1 h 05 min"
std::string took(std::chrono::milliseconds elapsed) {
    using std::chrono::duration_cast;
    if (elapsed < std::chrono::minutes{1}) {
        return std::format("{:.1f} s", static_cast<double>(elapsed.count()) / 1000.0);
    }
    if (elapsed < std::chrono::hours{1}) {
        return std::format("{} min {:02} s", duration_cast<std::chrono::minutes>(elapsed).count(),
                           duration_cast<std::chrono::seconds>(elapsed).count() % 60);
    }
    return std::format("{} h {:02} min", duration_cast<std::chrono::hours>(elapsed).count(),
                       duration_cast<std::chrono::minutes>(elapsed).count() % 60);
}

// How long a job's video lasts, asked by its army when it is raised: the
// job's ceilings are per hour of it (rr/hub/job_army.hpp).
std::function<Result<std::chrono::milliseconds>()> video_length_of(db::Database& database,
                                                                   std::string job_id) {
    return [&database, job_id = std::move(job_id)] {
        const library::StageStore store{database, SystemClock::instance()};
        return pipeline::video_length(store, job_id);
    };
}

// The stages a job runs, so far (plan P20): its source, its transcript, its
// outline, its atoms, its plan, then its article.
Result<pipeline::Executor> job_stages(const HubSettings& settings) {
    std::vector<std::unique_ptr<pipeline::Stage>> stages;
    stages.push_back(std::make_unique<pipeline::SourceStage>(
        media::YtDlp{media::YtDlpOptions{.executable = settings.ytdlp, .timeout = settings.ytdlp_timeout}}));
    stages.push_back(std::make_unique<pipeline::TranscriptStage>());
    stages.push_back(std::make_unique<pipeline::OutlineStage>());
    stages.push_back(std::make_unique<pipeline::AtomizeStage>());
    stages.push_back(std::make_unique<pipeline::PlanStage>());
    stages.push_back(std::make_unique<pipeline::WriteStage>());
    return pipeline::Executor::create(std::move(stages), SystemClock::instance());
}

// What a run did, and what to do next; the exit code that says it. `again`
// is the command that takes the job up again.
int report(const pipeline::JobRun& run, std::string_view again, const proc::TerminationSignals& signals) {
    const library::Job& job = run.job;
    const std::string title = job.title.value_or(job.source);
    bool told = false;
    for (const pipeline::StageReport& stage : run.report.stages) {
        for (const std::string& finding : stage.findings) {
            if (!std::exchange(told, true)) {
                (void)std::printf("what the stages could not do:\n");
            }
            (void)std::printf("  %s: %s\n", stage.stage.c_str(), finding.c_str());
        }
    }
    switch (job.status) {
        case library::JobStatus::done:
            (void)std::printf("job %s is done: %s (%s)\n", job.id.c_str(), title.c_str(),
                              took(job.elapsed).c_str());
            return kOk;
        case library::JobStatus::stopped:
            (void)std::printf("job %s stopped: %s\n  take it up again with: %.*s\n", job.id.c_str(),
                              job.error.value_or("").c_str(), static_cast<int>(again.size()), again.data());
            return signals.received() != 0 ? 128 + signals.received() : kFailure;
        case library::JobStatus::needs_user:
            (void)std::printf("job %s needs you: %s\n  then: %.*s\n", job.id.c_str(),
                              job.error.value_or("").c_str(), static_cast<int>(again.size()), again.data());
            return kFailure;
        case library::JobStatus::failed:
        case library::JobStatus::running:
            break;
    }
    (void)std::printf("job %s failed: %s\n", job.id.c_str(), job.error.value_or("").c_str());
    return kFailure;
}

// Takes `job` through the stages, raising the army only if a stage asks for it.
int run_job(const Hub& hub, db::Database& database, library::Job job,
            const proc::TerminationSignals& signals) {
    const HubSettings& settings = hub.settings;
    auto stages = job_stages(settings);
    if (!stages.has_value()) {
        return failed(kFailure, stages.error());
    }
    auto prompts = prompt::Library::embedded();
    if (!prompts.has_value()) {
        return failed(kFailure, prompts.error());
    }
    const net::HttpClient http = make_http();
    JobArmy forces{JobArmy::Inputs{.settings = &settings,
                                   .http = &http,
                                   .database = &database,
                                   .clock = &SystemClock::instance(),
                                   .job_id = job.id,
                                   .elapsed_before = job.elapsed,
                                   .video_length = video_length_of(database, job.id),
                                   .stop = signals.stop_token(),
                                   .progress = say}};
    pipeline::StageContext context{
        .prompts = &*prompts,
        .work_directory = settings.library_database().parent_path() / "jobs" / job.id,
        .stop = signals.stop_token(),
        .progress = say};
    // Without a pod, or recordings to replay, no model can be asked: a stage
    // that needs one does what it can without, and says so.
    if (settings.llm_mode == LlmMode::replay || settings.runtime.has_value()) {
        context.army = [&forces] { return forces.raise(); };
    }
    (void)std::fprintf(stderr, "job %s: %s\n", job.id.c_str(), job.title.value_or(job.source).c_str());
    const std::string again = std::format("rreport resume {}", job.id);
    auto run =
        pipeline::run_job(std::move(job), *stages, database, SystemClock::instance(), std::move(context));
    if (!run.has_value()) {
        return failed(kFailure, run.error());
    }
    return report(*run, again, signals);
}

struct IngestFlags {
    std::string source;
    std::string tier{reason::kDefaultTier};
    std::int64_t salt = 0;
};

int ingest(const Global& global, const IngestFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    if (auto tier = reason::builtin_tier(flags.tier); !tier.has_value()) {
        return failed(kBadConfiguration, tier.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    // A caption file is named by its absolute path, so that the job can be
    // taken up again from any directory.
    std::string source = flags.source;
    if (!pipeline::is_web_address(source)) {
        std::error_code error;
        source = std::filesystem::absolute(source, error).string();
        if (error) {
            return failed(kBadConfiguration, Error{Errc::invalid_argument,
                                                   std::format("'{}': {}", flags.source, error.message())});
        }
    }
    library::Jobs jobs{*database, SystemClock::instance()};
    auto job = jobs.create(source, flags.tier, flags.salt);
    if (!job.has_value()) {
        return failed(kFailure, job.error());
    }
    return run_job(*hub, *database, *std::move(job), signals);
}

int resume(const Global& global, const std::string& id, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    const library::Jobs jobs{*database, SystemClock::instance()};
    auto job = jobs.find(id);
    if (!job.has_value()) {
        return failed(kFailure, job.error());
    }
    if (job->status == library::JobStatus::done) {
        (void)std::printf("job %s is done already: %s\n", job->id.c_str(),
                          job->title.value_or(job->source).c_str());
        return kOk;
    }
    if (job->kind != library::JobKind::article) {
        return failed(
            kBadConfiguration,
            Error{Errc::invalid_argument,
                  std::format("job {} drafts an eval annotation: `rreport eval annotate` takes it up "
                              "again, with the video's id",
                              job->id)});
    }
    return run_job(*hub, *database, *std::move(job), signals);
}

int list_jobs(const Global& global, std::size_t limit) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    const library::Jobs jobs{*database, SystemClock::instance()};
    auto recent = jobs.recent(limit);
    if (!recent.has_value()) {
        return failed(kFailure, recent.error());
    }
    if (recent->empty()) {
        (void)std::printf("no jobs yet: rreport ingest <address or caption file> starts one\n");
        return kOk;
    }
    (void)std::printf("%-26s  %-10s  %-10s  %-8s  %-20s  %s\n", "JOB", "KIND", "STATUS", "TIER", "UPDATED",
                      "TITLE");
    for (const library::Job& job : *recent) {
        (void)std::printf("%-26s  %-10s  %-10s  %-8s  %-20s  %s\n", job.id.c_str(),
                          std::string{library::to_string(job.kind)}.c_str(),
                          std::string{library::to_string(job.status)}.c_str(), job.tier.c_str(),
                          when(job.updated_at).c_str(), job.title.value_or(job.source).c_str());
    }
    return kOk;
}

// How a stage's run ended, as `jobs show` says it.
std::string_view ended(const library::StageRun& run) {
    if (run.status == library::StageStatus::failed) {
        return "failed";
    }
    return run.cached ? "found done" : "done";
}

int show_job(const Global& global, const std::string& id) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto database = library::open_database(hub->settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    const library::Jobs jobs{*database, SystemClock::instance()};
    auto job = jobs.find(id);
    if (!job.has_value()) {
        return failed(kFailure, job.error());
    }
    const library::StageStore store{*database, SystemClock::instance()};
    auto runs = store.runs(job->id);
    if (!runs.has_value()) {
        return failed(kFailure, runs.error());
    }
    (void)std::printf("job %s: %s\n", job->id.c_str(), std::string{library::to_string(job->status)}.c_str());
    if (job->title.has_value()) {
        (void)std::printf("  title:   %s\n", job->title->c_str());
    }
    (void)std::printf("  makes:   %s\n", std::string{library::to_string(job->kind)}.c_str());
    (void)std::printf("  source:  %s\n", job->source.c_str());
    (void)std::printf("  tier:    %s, salt %lld\n", job->tier.c_str(), static_cast<long long>(job->salt));
    (void)std::printf("  spent:   %s\n", took(job->elapsed).c_str());
    (void)std::printf("  created: %s, updated %s\n", when(job->created_at).c_str(),
                      when(job->updated_at).c_str());
    if (job->error.has_value()) {
        (void)std::printf("  why:     %s\n", job->error->c_str());
    }
    for (const library::StageRun& run : *runs) {
        (void)std::printf("  stage %-12s %-10s %s\n", run.stage.c_str(), std::string{ended(run)}.c_str(),
                          run.error.value_or(run.artifact.value_or("")).c_str());
        for (const std::string& finding : run.findings) {
            (void)std::printf("    could not do: %s\n", finding.c_str());
        }
    }
    return kOk;
}

// ---------------------------------------------------------------------------------------
// eval fetch, annotate, sheet
// ---------------------------------------------------------------------------------------
struct EvalFlags {
    std::filesystem::path root{"eval"};
    std::string set{"m2"};
    std::vector<std::string> videos;           // none: every video of the set
    bool again = false;                        // fetch: read a video kept already once more
    bool replace = false;                      // annotate: draft a silver gold file again
    std::optional<std::filesystem::path> out;  // sheet: where to write it
};

// The command that does `verb` to `video`, with the flags that differ from
// the defaults: what the user is told to run next.
std::string eval_command(const EvalFlags& flags, std::string_view verb, std::string_view video) {
    std::string command = std::format("rreport eval {}", verb);
    if (flags.root != EvalFlags{}.root) {
        command += std::format(" --eval-dir {}", flags.root.string());
    }
    if (flags.set != EvalFlags{}.set) {
        command += std::format(" --eval-set {}", flags.set);
    }
    return std::format("{} {}", command, video);
}

// The set the flags name. One exists once its candidates are approved.
Result<eval::EvalSet> eval_set(const EvalFlags& flags) {
    auto set = eval::read_set(flags.root, flags.set);
    if (!set.has_value() && set.error().code() == Errc::not_found) {
        return fail(Errc::not_found,
                    std::format("{} does not exist: a set is made from the approved candidates ({})",
                                (flags.root / "sets" / std::format("{}.toml", flags.set)).string(),
                                (flags.root / "README.md").string()));
    }
    return set;
}

// The videos of `set` the command names, or all of them.
Result<std::vector<eval::EvalVideo>> chosen(const eval::EvalSet& set, const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return set.videos;
    }
    std::vector<eval::EvalVideo> videos;
    videos.reserve(ids.size());
    for (const std::string& id : ids) {
        const eval::EvalVideo* video = set.find(id);
        if (video == nullptr) {
            return fail(Errc::not_found, std::format("the set '{}' has no video '{}'", set.name, id));
        }
        videos.push_back(*video);
    }
    return videos;
}

// Says what went wrong with one video, and goes on with the next.
int video_failed(const eval::EvalVideo& video, const Error& error) {
    (void)std::fprintf(stderr, "rreport: %s: %s\n", video.id.c_str(), error.describe().c_str());
    return kFailure;
}

int eval_fetch(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto videos = chosen(*set, flags.videos);
    if (!videos.has_value()) {
        return failed(kBadConfiguration, videos.error());
    }
    const HubSettings& settings = hub->settings;
    const pipeline::SourceStage stage{
        media::YtDlp{media::YtDlpOptions{.executable = settings.ytdlp, .timeout = settings.ytdlp_timeout}}};
    int code = kOk;
    for (const eval::EvalVideo& video : *videos) {
        if (!flags.again) {
            // Read once: a snapshot kept is never read again unless asked.
            auto kept = eval::read_snapshot(flags.root, video);
            if (!kept.has_value()) {
                code = video_failed(video, kept.error());
                continue;
            }
            if (kept->has_value()) {
                (void)std::printf("%s: kept already, snapshot %s\n", video.id.c_str(), (*kept)->hash.c_str());
                continue;
            }
        }
        const std::filesystem::path work =
            settings.library_database().parent_path() / "eval-fetch" / video.id;
        auto snapshot = eval::fetch_snapshot(flags.root, video, stage, work, signals.stop_token(),
                                             std::chrono::system_clock::now());
        std::error_code ignored;  // scratch: what is kept is in the eval's directory
        std::filesystem::remove_all(work, ignored);
        if (!snapshot.has_value()) {
            if (snapshot.error().code() == Errc::cancelled) {
                return interrupted_or(kFailure, snapshot.error(), signals);
            }
            code = video_failed(video, snapshot.error());
            continue;
        }
        (void)std::printf("%s: kept %s, snapshot %s\n", video.id.c_str(),
                          eval::snapshot_file(flags.root, video.id).string().c_str(), snapshot->hash.c_str());
        if (!video.snapshot.has_value()) {
            (void)std::printf("  pin it in the set, under the video: snapshot = \"%s\"\n",
                              snapshot->hash.c_str());
        }
    }
    return code;
}

int eval_annotate(const Global& global, const EvalFlags& flags, const proc::TerminationSignals& signals) {
    auto hub = load(global);
    if (!hub.has_value()) {
        return failed(kBadConfiguration, hub.error());
    }
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto videos = chosen(*set, flags.videos);
    if (!videos.has_value()) {
        return failed(kBadConfiguration, videos.error());
    }
    const HubSettings& settings = hub->settings;
    auto database = library::open_database(settings.library_database());
    if (!database.has_value()) {
        return failed(kFailure, database.error());
    }
    auto prompts = prompt::Library::embedded();
    if (!prompts.has_value()) {
        return failed(kFailure, prompts.error());
    }
    const net::HttpClient http = make_http();
    int code = kOk;
    for (const eval::EvalVideo& video : *videos) {
        auto job =
            eval::start_annotation(flags.root, video, *database, SystemClock::instance(), flags.replace);
        if (!job.has_value()) {
            code = video_failed(video, job.error());
            continue;
        }
        (void)std::fprintf(stderr, "job %s: annotating %s, %s\n", job->id.c_str(), video.id.c_str(),
                           video.title.c_str());
        JobArmy forces{JobArmy::Inputs{.settings = &settings,
                                       .http = &http,
                                       .database = &*database,
                                       .clock = &SystemClock::instance(),
                                       .job_id = job->id,
                                       .elapsed_before = job->elapsed,
                                       .video_length = video_length_of(*database, job->id),
                                       .stop = signals.stop_token(),
                                       .progress = say}};
        pipeline::StageContext context{.prompts = &*prompts,
                                       .work_directory = settings.library_database().parent_path() / "jobs",
                                       .stop = signals.stop_token(),
                                       .progress = say};
        if (settings.llm_mode == LlmMode::replay || settings.runtime.has_value()) {
            context.army = [&forces] { return forces.raise(); };
        }
        auto annotated = eval::run_annotation(flags.root, video, *std::move(job), *database,
                                              SystemClock::instance(), std::move(context));
        if (!annotated.has_value()) {
            code = video_failed(video, annotated.error());
            continue;
        }
        const int done = report(annotated->run, eval_command(flags, "annotate", video.id), signals);
        if (annotated->gold.has_value()) {
            const eval::Gold& gold = *annotated->gold;
            const auto core = std::ranges::count(gold.facts, eval::Importance::core, &eval::Fact::importance);
            (void)std::printf(
                "  %zu facts (%lld core), %zu must-quotes and %zu terms, drafted into %s\n"
                "  correct it with its sheet: %s\n",
                gold.facts.size(), static_cast<long long>(core), gold.must_quotes.size(), gold.terms.size(),
                eval::gold_file(flags.root, video.id).string().c_str(),
                eval_command(flags, "sheet", video.id).c_str());
        }
        if (done != kOk) {
            code = done;
            if (signals.received() != 0) {
                return done;  // interrupted: the next videos wait for the next run
            }
        }
    }
    return code;
}

int eval_sheet(const EvalFlags& flags) {
    auto set = eval_set(flags);
    if (!set.has_value()) {
        return failed(kBadConfiguration, set.error());
    }
    auto videos = chosen(*set, flags.videos);
    if (!videos.has_value()) {
        return failed(kBadConfiguration, videos.error());
    }
    const eval::EvalVideo& video = videos->front();
    auto snapshot = eval::read_snapshot(flags.root, video);
    if (!snapshot.has_value()) {
        return failed(kFailure, snapshot.error());
    }
    if (!snapshot->has_value()) {
        return failed(kFailure,
                      Error{Errc::not_found, std::format("{} has no snapshot yet: {} reads it", video.id,
                                                         eval_command(flags, "fetch", video.id))});
    }
    const std::filesystem::path file = eval::gold_file(flags.root, video.id);
    auto text = read_file(file);
    if (!text.has_value()) {
        return failed(kFailure, text.error().code() == Errc::not_found
                                    ? Error{Errc::not_found,
                                            std::format("{} does not exist yet: {} drafts it", file.string(),
                                                        eval_command(flags, "annotate", video.id))}
                                    : text.error());
    }
    auto gold = eval::parse_gold(*text, file.string());
    if (!gold.has_value()) {
        return failed(kFailure, gold.error());
    }
    auto transcript = eval::caption_transcript((*snapshot)->source);
    if (!transcript.has_value()) {
        return failed(kFailure, transcript.error());
    }
    const std::string sheet = eval::review_sheet(*gold, video, *transcript, (*snapshot)->hash);
    if (!flags.out.has_value()) {
        (void)std::fputs(sheet.c_str(), stdout);
        return kOk;
    }
    if (auto written = write_file_atomically(*flags.out, sheet); !written.has_value()) {
        return failed(kFailure, written.error());
    }
    (void)std::printf("wrote %s\n", flags.out->string().c_str());
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

    CLI::App app{"rreport: understand any video completely"};
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

    IngestFlags ingest_flags;
    auto* ingest_command =
        app.add_subcommand("ingest", "Take a video, or a caption file, through the pipeline as a new job");
    ingest_command->add_option("source", ingest_flags.source, "A video's web address, or a caption file")
        ->required();
    ingest_command->add_option("--tier", ingest_flags.tier,
                               "How hard to work: fast, thorough or max (the default)");
    ingest_command->add_option("--salt", ingest_flags.salt,
                               "Draw new samples: the eval's replicate (default 0)");

    std::string job_id;
    auto* resume_command = app.add_subcommand("resume", "Take a job up again where it stopped");
    resume_command->add_option("job", job_id, "The job (rreport jobs lists them)")->required();

    std::size_t limit = 20;
    auto* jobs_command = app.add_subcommand("jobs", "The library's jobs, newest first");
    jobs_command->add_option("--limit", limit, "How many")->check(CLI::Range(1, 10'000));
    auto* job_show = jobs_command->add_subcommand("show", "A job's stages, and what they could not do");
    job_show->add_option("job", job_id, "The job")->required();

    EvalFlags eval_flags;
    auto* eval_group = app.add_subcommand("eval", "The evaluation's data: snapshots and gold annotations")
                           ->require_subcommand(1);
    eval_group->add_option("--eval-dir", eval_flags.root, "The eval's directory (default: eval)");
    eval_group->add_option("--eval-set", eval_flags.set,
                           "The set of videos: <eval-dir>/sets/<set>.toml (default: m2)");
    auto* eval_fetch_command =
        eval_group->add_subcommand("fetch", "Read each video once through the source stage, and keep it");
    eval_fetch_command->add_option("videos", eval_flags.videos,
                                   "The videos (default: every video of the set)");
    eval_fetch_command->add_flag("--again", eval_flags.again, "Read a video kept already once more");
    auto* eval_annotate_command =
        eval_group->add_subcommand("annotate", "Draft each video's gold annotation with the pod's models");
    eval_annotate_command->add_option("videos", eval_flags.videos,
                                      "The videos (default: every video of the set)");
    eval_annotate_command->add_flag("--replace", eval_flags.replace,
                                    "Draft a silver gold file again, losing what was corrected in it");
    std::string sheet_video;
    auto* eval_sheet_command =
        eval_group->add_subcommand("sheet", "The sheet a person corrects a gold file with");
    eval_sheet_command->add_option("video", sheet_video, "The video")->required();
    eval_sheet_command->add_option("--out", eval_flags.out, "Write it to this file (default: print it)");

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
    bench_command
        ->add_option("--commander-parallel", bench_flags.options.commander_parallel,
                     "Commander requests at once, e.g. 1,2,4: what its parallel slots are worth")
        ->delimiter(',')
        ->check(CLI::PositiveNumber);
    bench_command->add_flag("--cache-check", bench_flags.options.cache_check,
                            "Check that each engine's prompt cache serves a shared system message");
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

    if (*ingest_command) {
        return ingest(global, ingest_flags, signals);
    }
    if (*resume_command) {
        return resume(global, job_id, signals);
    }
    if (*job_show) {
        return show_job(global, job_id);
    }
    if (*jobs_command) {
        return list_jobs(global, limit);
    }
    if (*eval_fetch_command) {
        return eval_fetch(global, eval_flags, signals);
    }
    if (*eval_annotate_command) {
        return eval_annotate(global, eval_flags, signals);
    }
    if (*eval_sheet_command) {
        eval_flags.videos = {sheet_video};
        return eval_sheet(eval_flags);
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
