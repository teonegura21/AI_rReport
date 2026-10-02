// A job's stages, each found done or run, and kept (rr/pipeline/executor.hpp).
#include <rr/pipeline/executor.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/knowledge/artifacts.hpp>
#include <rr/llm/sampling.hpp>

namespace rr::pipeline {

namespace {

using std::chrono::milliseconds;

void say(const StageContext& context, std::string_view line) {
    if (context.progress) {
        context.progress(line);
    }
}

// The artifacts the run has made or found so far, by stage: what the stages
// after them read.
struct Made {
    std::map<std::string, std::string, std::less<>> hashes{};
    std::map<std::string, json::Json, std::less<>> artifacts{};
};

// One stage of `job`: found done in the stage cache, or run, checked against
// its schema and committed. Sets `key` as soon as it is known, for a failure
// to be recorded under it.
Result<StageReport> run_stage(const Stage& stage, const library::Job& job, library::StageStore& store,
                              StageContext& context, Made& made,
                              std::chrono::system_clock::time_point started, std::string& key) {
    const std::string name{stage.name()};
    context.inputs.clear();
    std::map<std::string, std::string, std::less<>> inputs;
    for (const std::string& input : stage.inputs()) {
        // Executor::create() saw that every input runs first.
        context.inputs.emplace(input, made.artifacts.at(input));
        inputs.emplace(input, made.hashes.at(input));
    }
    if (context.stop.stop_requested()) {
        return fail(Errc::cancelled, std::format("stopped before the {} stage", name));
    }
    context.fingerprint = nullptr;
    RR_TRY_ASSIGN(context.fingerprint, stage.fingerprint(context));
    key = cache_key(stage, context.fingerprint, inputs, job.salt);

    StageReport done{.stage = name};
    RR_TRY_ASSIGN(const std::optional<library::CachedResult> cached, store.cached(key));
    if (cached.has_value()) {
        RR_TRY_ASSIGN(json::Json artifact, store.artifact(cached->artifact));
        RR_TRY(store.reuse(job.id, name, key, *cached, started));
        done.cached = true;
        done.artifact = cached->artifact;
        done.findings = cached->findings;
        made.artifacts[name] = std::move(artifact);
    } else {
        say(context, std::format("{}: running", name));
        RR_TRY_ASSIGN(StageOutput output, stage.run(context));
        // A stage's own mistake, never kept: what follows would build on it.
        if (Result<void> valid = knowledge::artifact_schemas().validate(stage.schema(), output.artifact);
            !valid.has_value()) {
            return fail(Errc::internal,
                        std::format("the {} stage made an artifact that breaks its schema: {}", name,
                                    valid.error().message()));
        }
        RR_TRY_ASSIGN(done.artifact, store.commit(library::StageResult{.job_id = job.id,
                                                                       .stage = name,
                                                                       .version = stage.version(),
                                                                       .key = key,
                                                                       .schema = std::string{stage.schema()},
                                                                       .artifact = output.artifact,
                                                                       .findings = output.findings,
                                                                       .started_at = started,
                                                                       .reusable = output.reusable}));
        done.findings = std::move(output.findings);
        made.artifacts[name] = std::move(output.artifact);
    }
    made.hashes[name] = done.artifact;
    return done;
}

}  // namespace

std::string cache_key(const Stage& stage, const json::Json& fingerprint,
                      const std::map<std::string, std::string, std::less<>>& inputs, std::int64_t salt) {
    const json::Schema* schema = knowledge::artifact_schemas().find(stage.schema());
    RR_ASSERT(schema != nullptr, "Executor::create() refuses a stage whose schema this binary lacks");
    json::Json hashes = json::Json::object();
    for (const auto& [name, hash] : inputs) {
        hashes[name] = hash;
    }
    json::Json key{{"stage", std::string{stage.name()}},
                   {"version", stage.version()},
                   {"schema", json::content_hash(schema->document()).hex()},
                   {"fingerprint", fingerprint},
                   {"inputs", std::move(hashes)}};
    if (stage.samples()) {
        key["salt"] = salt;
    }
    return json::content_hash(key).hex();
}

Result<reason::Tier> job_tier(const StageContext& context) {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    return context.tiers ? context.tiers(context.job->tier) : reason::builtin_tier(context.job->tier);
}

json::Json army_fingerprint(const agents::Army& army) {
    std::vector<const agents::Engine*> engines;
    for (const agents::Engine& engine : army.engines()) {
        engines.push_back(&engine);
    }
    std::ranges::sort(engines, {}, &agents::Engine::model);
    json::Json written = json::Json::array();
    for (const agents::Engine* engine : engines) {
        written.push_back(json::Json{{"model", engine->model},
                                     {"checksum", engine->checksum},
                                     {"roles", engine->roles},
                                     {"direct", llm::to_json(engine->direct)},
                                     {"thinking", llm::to_json(engine->thinking)}});
    }
    return written;
}

Executor::Executor(std::vector<std::unique_ptr<Stage>> stages, const Clock& clock) : clock_(&clock) {
    for (std::unique_ptr<Stage>& stage : stages) {
        stages_.emplace_back(std::move(stage));
    }
}

Result<Executor> Executor::create(std::vector<std::unique_ptr<Stage>> stages, const Clock& clock) {
    std::set<std::string, std::less<>> before;
    for (const std::unique_ptr<Stage>& stage : stages) {
        for (const std::string& input : stage->inputs()) {
            if (!before.contains(input)) {
                return fail(Errc::invalid_argument,
                            std::format("the {} stage reads '{}', which does not run before it",
                                        stage->name(), input));
            }
        }
        if (knowledge::artifact_schemas().find(stage->schema()) == nullptr) {
            return fail(Errc::invalid_argument,
                        std::format("the {} stage writes '{}' artifacts, a schema this rReport does not have",
                                    stage->name(), stage->schema()));
        }
        if (!before.emplace(stage->name()).second) {
            return fail(Errc::invalid_argument, std::format("two stages are named '{}'", stage->name()));
        }
    }
    return Executor{std::move(stages), clock};
}

RunReport Executor::run(const library::Job& job, library::StageStore& store, StageContext context) const {
    RunReport report;
    context.job = &job;
    Made made;
    for (const std::shared_ptr<const Stage>& stage : stages_) {
        const auto started = clock_->now();
        const auto steady = clock_->steady_now();
        std::string key;  // known once the fingerprint is taken
        Result<StageReport> done = run_stage(*stage, job, store, context, made, started, key);
        if (!done.has_value()) {
            const Error error = std::move(done).error();
            // A failure that cannot be recorded is the one to report: the
            // library is in trouble.
            const Result<void> recorded =
                store.record_failure(job.id, stage->name(), key, error.message(), started);
            report.error = recorded.has_value() ? error : recorded.error();
            say(context, std::format("{}: failed: {}", stage->name(), report.error->message()));
            return report;
        }
        done->took = std::chrono::duration_cast<milliseconds>(clock_->steady_now() - steady);
        say(context, std::format("{}: {}", stage->name(), done->cached ? "found done" : "done"));
        report.stages.push_back(*std::move(done));
    }
    return report;
}

library::JobStatus Executor::status_after(const Error& error) noexcept {
    switch (error.code()) {
        case Errc::needs_user:
            return library::JobStatus::needs_user;
        case Errc::cancelled:
        case Errc::budget_exceeded:
            return library::JobStatus::stopped;
        default:
            return is_transient(error.code()) ? library::JobStatus::stopped : library::JobStatus::failed;
    }
}

}  // namespace rr::pipeline
