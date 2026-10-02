// What rreport's commands share (cli.hpp).
#include "cli.hpp"

#include <cstdio>
#include <format>
#include <memory>
#include <utility>

#include <rr/core/environment.hpp>
#include <rr/core/version.hpp>
#include <rr/hub/job_army.hpp>
#include <rr/hub/model_stack.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/log/log.hpp>
#include <rr/media/ytdlp.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/executor.hpp>
#include <rr/pipeline/job_run.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/plan_stage.hpp>
#include <rr/pipeline/seams_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/write_stage.hpp>
#include <rr/prompt/library.hpp>

namespace rr::cli {

namespace {

using namespace rr::hub;

HubFacts hub_facts() {
    return HubFacts{.home = environment_variable("HOME"),
                    .xdg_config_home = environment_variable("XDG_CONFIG_HOME"),
                    .xdg_data_home = environment_variable("XDG_DATA_HOME"),
                    .config_env = environment_variable("RREPORT_CONFIG")};
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

// The stages a job runs: its source, its transcript, its outline, its atoms,
// its plan, its article, then its seams smoothed (plan P15–P20).
Result<pipeline::Executor> job_stages(const HubSettings& settings) {
    std::vector<std::unique_ptr<pipeline::Stage>> stages;
    stages.push_back(std::make_unique<pipeline::SourceStage>(
        media::YtDlp{media::YtDlpOptions{.executable = settings.ytdlp, .timeout = settings.ytdlp_timeout}}));
    stages.push_back(std::make_unique<pipeline::TranscriptStage>());
    stages.push_back(std::make_unique<pipeline::OutlineStage>());
    stages.push_back(std::make_unique<pipeline::AtomizeStage>());
    stages.push_back(std::make_unique<pipeline::PlanStage>());
    stages.push_back(std::make_unique<pipeline::WriteStage>());
    stages.push_back(std::make_unique<pipeline::SeamsStage>());
    return pipeline::Executor::create(std::move(stages), SystemClock::instance());
}

}  // namespace

int report_run(const pipeline::JobRun& run, std::string_view again, const proc::TerminationSignals& signals) {
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

int failed(int code, const Error& error) {
    (void)std::fprintf(stderr, "rreport: %s\n", error.describe().c_str());
    return code;
}

int interrupted_or(int code, const Error& error, const proc::TerminationSignals& signals) {
    if (error.code() == Errc::cancelled && signals.received() != 0) {
        (void)std::fprintf(stderr, "rreport: interrupted\n");
        return 128 + signals.received();
    }
    return failed(code, error);
}

Result<config::Config> load_config(const Global& global) {
    const hub::ConfigFiles files = hub::config_files(global.config_file, global.secrets_file, hub_facts());
    return config::Config::load(config::LoadOptions{.defaults_toml = hub::hub_defaults(),
                                                    .file = files.config,
                                                    .file_required = files.config_required,
                                                    .secrets_file = files.secrets,
                                                    .env_prefix = std::string{hub::kHubEnvPrefix},
                                                    .overrides = global.overrides});
}

Result<Hub> load(const Global& global) {
    RR_TRY_ASSIGN(config::Config config, load_config(global));
    RR_TRY(start_logging(config));
    RR_TRY_ASSIGN(hub::HubSettings settings, hub::hub_settings_from_config(config, hub_facts()));
    return Hub{.config = std::move(config), .settings = std::move(settings)};
}

net::HttpClient make_http() {
    return net::HttpClient{net::HttpClient::Options{.user_agent = std::format("rreport/{}", kVersion)}};
}

void say(std::string_view line) {
    (void)std::fprintf(stderr, "  %.*s\n", static_cast<int>(line.size()), line.data());
}

std::string when(std::chrono::system_clock::time_point at) {
    return std::format("{:%Y-%m-%d %H:%M} UTC", std::chrono::floor<std::chrono::minutes>(at));
}

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

std::function<Result<std::chrono::milliseconds>()> video_length_of(db::Database& database,
                                                                   std::string job_id) {
    return [&database, job_id = std::move(job_id)] {
        const library::StageStore store{database, SystemClock::instance()};
        return pipeline::video_length(store, job_id);
    };
}

int run_job(const Hub& hub, db::Database& database, library::Job job,
            const proc::TerminationSignals& signals) {
    const hub::HubSettings& settings = hub.settings;
    auto stages = job_stages(settings);
    if (!stages.has_value()) {
        return failed(kFailure, stages.error());
    }
    auto prompts = prompt::Library::embedded();
    if (!prompts.has_value()) {
        return failed(kFailure, prompts.error());
    }
    const net::HttpClient http = make_http();
    hub::JobArmy forces{hub::JobArmy::Inputs{.settings = &settings,
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
    if (settings.llm_mode == hub::LlmMode::replay || settings.runtime.has_value()) {
        context.army = [&forces] { return forces.raise(); };
    }
    (void)std::fprintf(stderr, "job %s: %s\n", job.id.c_str(), job.title.value_or(job.source).c_str());
    const std::string again = std::format("rreport resume {}", job.id);
    auto run =
        pipeline::run_job(std::move(job), *stages, database, SystemClock::instance(), std::move(context));
    if (!run.has_value()) {
        return failed(kFailure, run.error());
    }
    return report_run(*run, again, signals);
}

}  // namespace rr::cli
