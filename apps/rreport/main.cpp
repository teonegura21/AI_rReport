// rreport — the hub, on your own machine.
//
// Commands
//   ingest SOURCE     take a video (its web address) or a caption file through
//                     the pipeline's stages, as a new job: its source, its
//                     transcript, its outline, its atoms, its plan, its
//                     article and its seams (plan P15–P20)
//   resume JOB        take a job up again where it stopped
//   jobs              the library's jobs, newest first
//   jobs show JOB     a job's stages: done, found done or failed, and what
//                     they could not do
//   report JOB        the job's article as one web page, with its Fidelity
//                     Report (P21)
//   eval fetch [ID…]  read each video of the eval set once, through the source
//                     stage, and keep it with the eval: eval/data/<id>/ (P16)
//   eval annotate [ID…]
//                     draft each video's gold annotation with the pod's models,
//                     as silver in eval/gold/<id>.toml, for a person to correct
//                     (runbook R2)
//   eval sheet ID     the sheet a person corrects a gold file with (Markdown)
//   eval run --label L [ID…]
//                     take the set's videos through the article stages, each
//                     --replicates times, at --tier; the same label again
//                     takes the run up (P23)
//   eval score --label L
//                     score each done job of a run by code: what it kept, what
//                     it cost
//   eval export --label L
//                     a run's scores, a row per video and replicate (CSV, JSON)
//   eval judge --label L [--panel P]
//                     each done job of a run read by the eval's frozen judges:
//                     which facts its article tells, which of its sentences
//                     the video supports (P24)
//   eval compare A B  two runs side by side, every metric: the difference video
//                     by video, its interval, and what the videos could see
//                     (P25, ADR-0019); with --flow, each video's two articles
//                     read side by side by the frozen judges, part by part
//   eval gate A B     whether candidate B may replace baseline A, as
//                     eval/gates/<gate>.toml says; with --aa, whether the
//                     gate's margins hold, B being A run again
//   eval sweep NAME   each variant of eval/sweeps/<NAME>.toml run, scored and
//                     judged, then compared with the sweep's baseline (P26)
//   eval report       every eval run on one page: quality against cost, the
//                     runs, each run's videos (eval/report.html)
//   eval label NAME --label L [--against B]
//                     draw items the frozen judges read of a run, for a person
//                     to label: eval/labels/<name>.toml (P27, runbook R5)
//   eval calibrate NAME
//                     whether the judges read as the labels do (ADR-0019)
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
// This file, and eval_commands.cpp for the eval's commands, only parse
// arguments and wire modules together, sharing cli.hpp; the rules live in
// src/ (rr/hub/*.hpp, rr/eval/*.hpp), where they are unit-tested.
// Configuration: config/default.toml.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
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
#include <rr/hub/bench.hpp>
#include <rr/hub/doctor.hpp>
#include <rr/hub/hub_config.hpp>
#include <rr/hub/model_stack.hpp>
#include <rr/library/call_log.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/log/log.hpp>
#include <rr/net/http_client.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/proc/termination_signals.hpp>
#include <rr/reason/tier.hpp>
#include <rr/report/contents.hpp>
#include <rr/report/page.hpp>
#include <rr/runpod/runpod.hpp>
#include <rr/runtime_client/pod_manager.hpp>
#include <rr/runtime_client/runtime_client.hpp>

#include "cli.hpp"
#include "eval_commands.hpp"

namespace {

using namespace rr;
using namespace rr::cli;
using namespace rr::hub;

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
    if (auto tier = reason::find_tier(flags.tier); !tier.has_value()) {
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
    switch (job->kind) {
        case library::JobKind::article:
            break;
        case library::JobKind::annotation:
            return failed(kBadConfiguration,
                          Error{Errc::invalid_argument,
                                std::format("job {} drafts an eval annotation: `rreport eval annotate` takes "
                                            "it up again, with the video's id",
                                            job->id)});
        case library::JobKind::judgement:
            return failed(
                kBadConfiguration,
                Error{Errc::invalid_argument,
                      std::format("job {} judges an article for the eval: `rreport eval judge` takes "
                                  "it up again, with its run's label",
                                  job->id)});
        case library::JobKind::pairing:
            return failed(
                kBadConfiguration,
                Error{Errc::invalid_argument,
                      std::format("job {} compares two articles for the eval: `rreport eval compare --flow` "
                                  "takes it up again, with its runs' labels",
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
// report
// ---------------------------------------------------------------------------------------

int report_job(const Global& global, const std::string& id, const std::optional<std::filesystem::path>& out) {
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
    auto contents = report::contents_of(*job, store);
    if (!contents.has_value()) {
        // An eval annotation has no article: asking for its page is a usage error.
        return failed(contents.error().code() == Errc::invalid_argument ? kBadConfiguration : kFailure,
                      contents.error());
    }
    const std::filesystem::path page =
        out.value_or(hub->settings.library_database().parent_path() / "reports" / (job->id + ".html"));
    if (std::error_code made;
        page.has_parent_path() && !std::filesystem::create_directories(page.parent_path(), made) && made) {
        return failed(kFailure, Error{Errc::io, std::format("cannot create '{}': {}",
                                                            page.parent_path().string(), made.message())});
    }
    if (auto written = write_file_atomically(page, report::render_page(*contents)); !written.has_value()) {
        return failed(kFailure, written.error());
    }
    (void)std::printf("%s\n", page.string().c_str());
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

    std::optional<std::filesystem::path> report_out;
    auto* report_command =
        app.add_subcommand("report", "A job's article as a web page, with its Fidelity Report");
    report_command->add_option("job", job_id, "The job (rreport jobs lists them)")->required();
    report_command->add_option("--out", report_out,
                               "Where to write the page (default: reports/<job>.html beside the library)");

    const EvalCommands eval{app};

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
    if (*report_command) {
        return report_job(global, job_id, report_out);
    }
    if (*jobs_command) {
        return list_jobs(global, limit);
    }
    if (const std::optional<int> code = eval.run(global, signals); code.has_value()) {
        return *code;
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
