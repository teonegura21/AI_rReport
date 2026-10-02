// A job's army, raised the first time a stage needs it (rr/hub/job_army.hpp).
#include <rr/hub/job_army.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <ratio>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/core/file_io.hpp>
#include <rr/hub/army_engines.hpp>
#include <rr/hub/model_stack.hpp>
#include <rr/json/json.hpp>
#include <rr/library/call_log.hpp>
#include <rr/library/call_memo.hpp>
#include <rr/library/mission_log.hpp>
#include <rr/llm/memo.hpp>
#include <rr/runpod/runpod.hpp>
#include <rr/runtime_client/pod_manager.hpp>
#include <rr/runtime_client/runtime_client.hpp>

namespace rr::hub {

namespace {

void say(const JobArmy::Inputs& inputs, std::string_view line) {
    if (inputs.progress) {
        inputs.progress(line);
    }
}

// The pod's health report once every engine is ready. A pod RunPod manages
// is started first if it is stopped.
Result<runtime_api::HealthReport> live_engines(const JobArmy::Inputs& inputs) {
    const HubSettings& settings = *inputs.settings;
    if (!settings.runtime.has_value()) {
        return fail(Errc::needs_user,
                    "no pod is configured to ask models: set runpod.pod_id (or runtime.url)");
    }
    if (settings.runtime->token.empty()) {
        return fail(
            Errc::needs_user,
            "runtime.token is not set: it must equal the pod's RR_RUNTIME_TOKEN (RREPORT_RUNTIME__TOKEN)");
    }
    const runtime_client::RuntimeClient runtime{*inputs.http, *settings.runtime};
    if (settings.runpod.has_value() && !settings.runpod->api_key.empty()) {
        const runpod::RunpodApi api{*inputs.http, runpod::Options{.api_key = settings.runpod->api_key,
                                                                  .api_url = settings.runpod->api_url}};
        const runtime_client::PodManager manager{api, settings.runpod->pod_id, runtime};
        say(inputs, std::format("army: waiting for pod {} and its engines", settings.runpod->pod_id));
        RR_TRY_ASSIGN(runtime_client::ReadyPod ready,
                      manager.ensure_ready(settings.ready_timeout, inputs.stop));
        return std::move(ready.health);
    }
    say(inputs, std::format("army: waiting for the engines of the runtime at {}", settings.runtime->url));
    return runtime.wait_until_ready(settings.ready_timeout, inputs.stop);
}

// The engines record mode kept, for a replay.
Result<runtime_api::HealthReport> replayed_engines(const HubSettings& settings) {
    const std::filesystem::path path = recorded_engines(settings);
    Result<std::string> text = read_file(path);
    if (!text.has_value() && text.error().code() == Errc::not_found) {
        return fail(
            Errc::needs_user,
            std::format("replay mode raises the army the answers were recorded with, and {} is missing: "
                        "record the job first (llm.mode = record)",
                        path.string()));
    }
    RR_TRY(text);
    RR_TRY_ASSIGN(const json::Json document, json::parse(*text));
    return runtime_api::health_report_from_json(document);
}

Result<void> keep_engines(const HubSettings& settings, const runtime_api::HealthReport& report) {
    if (std::error_code error; !std::filesystem::create_directories(settings.recordings, error) && error) {
        return fail(Errc::io, std::format("cannot make the recordings' directory '{}': {}",
                                          settings.recordings.string(), error.message()));
    }
    return write_file_atomically(recorded_engines(settings),
                                 json::dump_pretty(runtime_api::to_json(report)) + "\n");
}

// What each model id means on this pod: its file's checksum. The memo keys
// every answer by it (rr/llm/memo.hpp).
llm::ModelFingerprint checksums_of(const runtime_api::HealthReport& report) {
    std::map<std::string, std::string, std::less<>> checksums;
    for (const runtime_api::EngineInfo& engine : report.engines) {
        checksums.emplace(engine.model, engine.model_sha256);
    }
    return [checksums = std::move(checksums)](std::string_view model) -> Result<std::string> {
        const auto found = checksums.find(model);
        if (found == checksums.end()) {
            return fail(Errc::not_found, std::format("the pod serves no model '{}'", model));
        }
        return found->second;
    };
}

// "qwen3.8-27b-q4 (commander, 4 slots), gemma-4-26b-a4b (infantry, 16 slots)"
std::string describe(const std::vector<agents::Engine>& engines) {
    std::string text;
    for (const agents::Engine& engine : engines) {
        std::string roles;
        for (const std::string& role : engine.roles) {
            roles += roles.empty() ? role : " and " + role;
        }
        text += std::format("{}{} ({}, {} slot{})", text.empty() ? "" : ", ", engine.model, roles,
                            engine.slots, engine.slots == 1 ? "" : "s");
    }
    return text;
}

// "45 min", "2 h 05 min": a stretch of time, as the progress says it.
std::string spoken(std::chrono::milliseconds time) {
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(time).count();
    return minutes < 60 ? std::format("{} min", minutes)
                        : std::format("{} h {:02} min", minutes / 60, minutes % 60);
}

// "2,500,000": a count of a few million, as a reader takes it in.
std::string grouped(std::int64_t count) {
    std::string digits = std::to_string(count);
    const std::size_t first = digits.starts_with('-') ? 1 : 0;
    for (std::size_t at = digits.size(); at > first + 3; at -= 3) {
        digits.insert(at - 3, 1, ',');
    }
    return digits;
}

}  // namespace

std::filesystem::path recorded_engines(const HubSettings& settings) {
    return settings.recordings / "engines.json";
}

agents::Ceilings job_ceilings(std::chrono::milliseconds length, const JobLimits& limits) {
    const double hours =
        std::chrono::duration<double, std::ratio<3600>>(std::max(length, limits.least_video)).count();
    return agents::Ceilings{
        .completion_tokens =
            static_cast<std::int64_t>(std::ceil(hours * static_cast<double>(limits.tokens_per_hour))),
        .wall_clock = std::chrono::duration_cast<std::chrono::milliseconds>(limits.time_per_hour * hours)};
}

// What the army stands on. Each part refers to the ones before it, so each
// keeps its address: they live on the heap, and are destroyed in reverse.
struct JobArmy::Forces {
    std::unique_ptr<library::CallLog> calls{};
    std::unique_ptr<ModelStack> stack{};
    std::unique_ptr<library::CallMemo> memo{};
    std::unique_ptr<llm::MemoProvider> remembered{};
    std::unique_ptr<library::MissionLog> log{};
    std::unique_ptr<agents::Army> army{};  // last: it uses everything above
};

JobArmy::JobArmy(Inputs inputs) : inputs_(std::move(inputs)) {}

JobArmy::~JobArmy() = default;

Result<agents::Army*> JobArmy::raise() {
    const std::scoped_lock lock{mutex_};
    if (forces_ != nullptr) {
        return forces_->army.get();
    }
    const HubSettings& settings = *inputs_.settings;
    runtime_api::HealthReport report;
    if (settings.llm_mode == LlmMode::replay) {
        RR_TRY_ASSIGN(report, replayed_engines(settings));
    } else {
        RR_TRY_ASSIGN(report, live_engines(inputs_));
        if (settings.llm_mode == LlmMode::record) {
            RR_TRY(keep_engines(settings, report));
        }
    }
    RR_TRY_ASSIGN(std::vector<agents::Engine> engines, army_engines(report, settings.sampling));
    const std::string raised = describe(engines);
    agents::Ceilings ceilings{};
    std::string bounded;  // what the ceilings are, said
    if (inputs_.video_length) {
        RR_TRY_ASSIGN(const std::chrono::milliseconds length, inputs_.video_length());
        ceilings = job_ceilings(length, settings.jobs);
        RR_ASSERT(ceilings.completion_tokens.has_value() && ceilings.wall_clock.has_value(),
                  "a job's ceilings bound both its tokens and its time");
        bounded =
            std::format("army: the job may spend {} completion tokens and {}, for {} of video",
                        grouped(*ceilings.completion_tokens), spoken(*ceilings.wall_clock), spoken(length));
    }

    auto forces = std::make_unique<Forces>();
    forces->calls = std::make_unique<library::CallLog>(*inputs_.database);
    RR_TRY_ASSIGN(ModelStack stack,
                  ModelStack::build(settings, *inputs_.http, *forces->calls, *inputs_.clock, {}));
    forces->stack = std::make_unique<ModelStack>(std::move(stack));
    forces->memo = std::make_unique<library::CallMemo>(*inputs_.database, inputs_.job_id, *inputs_.clock);
    forces->remembered =
        std::make_unique<llm::MemoProvider>(forces->stack->provider(), *forces->memo, checksums_of(report));
    forces->log = std::make_unique<library::MissionLog>(*inputs_.database);
    RR_TRY_ASSIGN(agents::Army army,
                  agents::Army::create(agents::Army::Options{.engines = std::move(engines),
                                                             .job_id = inputs_.job_id,
                                                             .ceilings = ceilings,
                                                             .elapsed_before = inputs_.elapsed_before},
                                       *forces->remembered, *inputs_.clock, forces->log.get()));
    forces->army = std::make_unique<agents::Army>(std::move(army));
    forces_ = std::move(forces);
    say(inputs_, std::format("army: raised on {}", raised));
    if (!bounded.empty()) {
        say(inputs_, bounded);
    }
    return forces_->army.get();
}

}  // namespace rr::hub
